#include "core/spotify_api_client.hpp"
#include "core/track_metadata.hpp"

#define DDB_API_LEVEL 10
#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <deadbeef/gtkui_api.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace spotify_bridge;

namespace {

DB_functions_t* deadbeef = nullptr;
ddb_gtkui_t* gtkui = nullptr;
GtkWidget* standalone_window = nullptr;

std::string get_conf_str(const char* key, const char* def) {
    char buf[1024]{};
    if (deadbeef) deadbeef->conf_get_str(key, def, buf, sizeof(buf));
    return std::string(buf);
}

void sync_spotify_credentials() {
    SpotifyApiClient::instance().set_credentials(
        get_conf_str("spotify_soloist.client_id", ""),
        get_conf_str("spotify_soloist.client_secret", "")
    );
}

std::string format_duration(int64_t ms) {
    if (ms <= 0) return "";
    int total_sec = static_cast<int>(ms / 1000);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total_sec / 60, total_sec % 60);
    return std::string(buf);
}

void add_tracks_to_playlist(const std::vector<TrackMetadata>& tracks, bool play_first) {
    if (!deadbeef || tracks.empty()) return;
    ddb_playlist_t* plt = deadbeef->plt_get_curr();
    if (!plt) return;

    int first_idx = -1;
    DB_playItem_t* last = deadbeef->plt_get_last(plt, PL_MAIN);
    DB_playItem_t* after = last;
    for (const auto& t : tracks) {
        std::string uri = to_audacious_spotify_uri(t.location);
        DB_playItem_t* it = deadbeef->pl_item_alloc_init(uri.c_str(), "spotify_soloist");
        if (!it) continue;
        deadbeef->pl_add_meta(it, ":FILETYPE", "Spotify");
        if (!t.title.empty()) deadbeef->pl_add_meta(it, "title", t.title.c_str());
        if (!t.artist.empty()) deadbeef->pl_add_meta(it, "artist", t.artist.c_str());
        if (!t.album.empty()) deadbeef->pl_add_meta(it, "album", t.album.c_str());
        if (t.track_num > 0) deadbeef->pl_add_meta(it, "track", std::to_string(t.track_num).c_str());
        if (!t.image_url.empty()) deadbeef->pl_add_meta(it, ":SPOTIFY_IMAGE", t.image_url.c_str());
        if (t.duration_ms > 0) {
            deadbeef->plt_set_item_duration(plt, it, static_cast<float>(t.duration_ms) / 1000.0f);
        }
        after = deadbeef->plt_insert_item(plt, after, it);
        if (first_idx < 0) {
            first_idx = deadbeef->plt_get_item_idx(plt, it, PL_MAIN);
        }
        deadbeef->pl_item_unref(it);
    }
    if (last) deadbeef->pl_item_unref(last);

    deadbeef->plt_save_config(plt);
    deadbeef->sendmessage(DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
    if (play_first && first_idx >= 0) {
        deadbeef->sendmessage(DB_EV_PLAY_NUM, 0, static_cast<uint32_t>(first_idx), 0);
    }
    deadbeef->plt_unref(plt);
}

struct SpotifyPanelState {
    GtkWidget* root = nullptr;
    GtkWidget* status_label = nullptr;

    // Tab 1: Song Search
    GtkWidget* track_entry = nullptr;
    GtkWidget* artist_entry = nullptr;
    GtkWidget* album_entry = nullptr;
    GtkWidget* load_more_btn = nullptr;
    GtkListStore* song_store = nullptr;
    GtkWidget* song_tree = nullptr;
    std::vector<TrackMetadata> song_results;
    std::string last_q_track;
    std::string last_q_artist;
    std::string last_q_album;
    int song_offset = 0;

    // Tab 2: Artist Discography
    GtkWidget* discog_artist_entry = nullptr;
    GtkListStore* artist_store = nullptr;
    GtkWidget* artist_tree = nullptr;
    GtkListStore* album_store = nullptr;
    GtkWidget* album_tree = nullptr;
    GtkListStore* album_track_store = nullptr;
    GtkWidget* album_track_tree = nullptr;
    std::vector<SpotifyArtist> artist_results;
    std::vector<SpotifyAlbum> album_results;
    std::vector<TrackMetadata> album_track_results;

    bool alive = true;
};

void set_status(SpotifyPanelState* st, const std::string& msg) {
    if (st && st->alive && st->status_label) {
        gtk_label_set_text(GTK_LABEL(st->status_label), msg.c_str());
    }
}

void run_song_search(SpotifyPanelState* st, bool append) {
    sync_spotify_credentials();
    if (!SpotifyApiClient::instance().has_credentials()) {
        set_status(st, "Set Spotify Client ID & Secret in Preferences -> Plugins -> Spotify Search.");
        return;
    }

    if (!append) {
        st->last_q_track = gtk_entry_get_text(GTK_ENTRY(st->track_entry));
        st->last_q_artist = gtk_entry_get_text(GTK_ENTRY(st->artist_entry));
        st->last_q_album = gtk_entry_get_text(GTK_ENTRY(st->album_entry));
        st->song_offset = 0;
        st->song_results.clear();
        gtk_list_store_clear(st->song_store);
    }
    if (st->last_q_track.empty() && st->last_q_artist.empty() && st->last_q_album.empty()) {
        return;
    }

    set_status(st, "Searching Spotify tracks...");
    std::string q_tr = st->last_q_track;
    std::string q_ar = st->last_q_artist;
    std::string q_al = st->last_q_album;
    int offset = st->song_offset;

    std::thread([st, q_tr, q_ar, q_al, offset, append]() {
        std::string err;
        auto tracks = SpotifyApiClient::instance().search_tracks_advanced(q_tr, q_ar, q_al, 10, offset, &err);
        struct Result {
            SpotifyPanelState* st;
            std::vector<TrackMetadata> tracks;
            std::string err;
            bool append;
        };
        auto* res = new Result{st, std::move(tracks), std::move(err), append};
        g_idle_add([](gpointer data) -> gboolean {
            std::unique_ptr<Result> r(static_cast<Result*>(data));
            if (!r->st->alive) return G_SOURCE_REMOVE;
            if (!r->err.empty()) {
                set_status(r->st, "Error: " + r->err);
                return G_SOURCE_REMOVE;
            }
            for (const auto& t : r->tracks) {
                r->st->song_results.push_back(t);
                GtkTreeIter iter;
                gtk_list_store_append(r->st->song_store, &iter);
                gtk_list_store_set(r->st->song_store, &iter,
                                   0, t.title.c_str(),
                                   1, t.artist.c_str(),
                                   2, t.album.c_str(),
                                   3, format_duration(t.duration_ms).c_str(),
                                   -1);
            }
            r->st->song_offset += static_cast<int>(r->tracks.size());
            gtk_widget_set_sensitive(r->st->load_more_btn, r->tracks.size() == 10);
            set_status(r->st, "Loaded " + std::to_string(r->st->song_results.size()) + " tracks.");
            return G_SOURCE_REMOVE;
        }, res);
    }).detach();
}

void run_artist_search(SpotifyPanelState* st) {
    sync_spotify_credentials();
    if (!SpotifyApiClient::instance().has_credentials()) {
        set_status(st, "Set Spotify Client ID & Secret in Preferences -> Plugins -> Spotify Search.");
        return;
    }
    std::string q = gtk_entry_get_text(GTK_ENTRY(st->discog_artist_entry));
    if (q.empty()) return;

    set_status(st, "Searching artists...");
    gtk_list_store_clear(st->artist_store);
    gtk_list_store_clear(st->album_store);
    gtk_list_store_clear(st->album_track_store);
    st->artist_results.clear();
    st->album_results.clear();
    st->album_track_results.clear();

    std::thread([st, q]() {
        std::string err;
        auto artists = SpotifyApiClient::instance().search_artists(q, 10, &err);
        struct Result {
            SpotifyPanelState* st;
            std::vector<SpotifyArtist> artists;
            std::string err;
        };
        auto* res = new Result{st, std::move(artists), std::move(err)};
        g_idle_add([](gpointer data) -> gboolean {
            std::unique_ptr<Result> r(static_cast<Result*>(data));
            if (!r->st->alive) return G_SOURCE_REMOVE;
            if (!r->err.empty()) {
                set_status(r->st, "Error: " + r->err);
                return G_SOURCE_REMOVE;
            }
            r->st->artist_results = std::move(r->artists);
            for (const auto& a : r->st->artist_results) {
                GtkTreeIter iter;
                gtk_list_store_append(r->st->artist_store, &iter);
                gtk_list_store_set(r->st->artist_store, &iter,
                                   0, a.name.c_str(),
                                   1, a.genres.c_str(),
                                   -1);
            }
            set_status(r->st, "Found " + std::to_string(r->st->artist_results.size()) + " artists.");
            return G_SOURCE_REMOVE;
        }, res);
    }).detach();
}

void load_artist_albums(SpotifyPanelState* st, const SpotifyArtist& artist) {
    set_status(st, "Loading discography for " + artist.name + "...");
    gtk_list_store_clear(st->album_store);
    gtk_list_store_clear(st->album_track_store);
    st->album_results.clear();
    st->album_track_results.clear();

    std::thread([st, artist]() {
        std::string err;
        auto albums = SpotifyApiClient::instance().fetch_artist_albums(artist.id, &err);
        struct Result {
            SpotifyPanelState* st;
            std::vector<SpotifyAlbum> albums;
            std::string err;
        };
        auto* res = new Result{st, std::move(albums), std::move(err)};
        g_idle_add([](gpointer data) -> gboolean {
            std::unique_ptr<Result> r(static_cast<Result*>(data));
            if (!r->st->alive) return G_SOURCE_REMOVE;
            if (!r->err.empty()) {
                set_status(r->st, "Error: " + r->err);
                return G_SOURCE_REMOVE;
            }
            r->st->album_results = std::move(r->albums);
            for (const auto& al : r->st->album_results) {
                GtkTreeIter iter;
                gtk_list_store_append(r->st->album_store, &iter);
                gtk_list_store_set(r->st->album_store, &iter,
                                   0, al.release_date.c_str(),
                                   1, al.album_type.c_str(),
                                   2, al.name.c_str(),
                                   -1);
            }
            set_status(r->st, "Loaded " + std::to_string(r->st->album_results.size()) + " releases.");
            return G_SOURCE_REMOVE;
        }, res);
    }).detach();
}

void load_album_tracks(SpotifyPanelState* st, const SpotifyAlbum& album) {
    set_status(st, "Loading tracks for " + album.name + "...");
    gtk_list_store_clear(st->album_track_store);
    st->album_track_results.clear();

    std::thread([st, album]() {
        std::string err;
        auto tracks = SpotifyApiClient::instance().fetch_album_tracks(album, &err);
        struct Result {
            SpotifyPanelState* st;
            std::vector<TrackMetadata> tracks;
            std::string err;
        };
        auto* res = new Result{st, std::move(tracks), std::move(err)};
        g_idle_add([](gpointer data) -> gboolean {
            std::unique_ptr<Result> r(static_cast<Result*>(data));
            if (!r->st->alive) return G_SOURCE_REMOVE;
            if (!r->err.empty()) {
                set_status(r->st, "Error: " + r->err);
                return G_SOURCE_REMOVE;
            }
            r->st->album_track_results = std::move(r->tracks);
            for (const auto& t : r->st->album_track_results) {
                GtkTreeIter iter;
                gtk_list_store_append(r->st->album_track_store, &iter);
                gtk_list_store_set(r->st->album_track_store, &iter,
                                   0, std::to_string(t.track_num).c_str(),
                                   1, t.title.c_str(),
                                   2, format_duration(t.duration_ms).c_str(),
                                   -1);
            }
            set_status(r->st, "Loaded " + std::to_string(r->st->album_track_results.size()) + " tracks.");
            return G_SOURCE_REMOVE;
        }, res);
    }).detach();
}

std::vector<TrackMetadata> get_selected_tracks(GtkWidget* tree, const std::vector<TrackMetadata>& source) {
    std::vector<TrackMetadata> out;
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    GtkTreeModel* model = nullptr;
    GList* rows = gtk_tree_selection_get_selected_rows(sel, &model);
    for (GList* l = rows; l != nullptr; l = l->next) {
        auto* path = static_cast<GtkTreePath*>(l->data);
        int* indices = gtk_tree_path_get_indices(path);
        if (indices) {
            size_t idx = static_cast<size_t>(indices[0]);
            if (idx < source.size()) out.push_back(source[idx]);
        }
    }
    g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
    return out;
}

void add_text_column(GtkWidget* tree, const char* title, int col_id, bool expand = true) {
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(title, renderer, "text", col_id, nullptr);
    gtk_tree_view_column_set_resizable(col, TRUE);
    if (expand) gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
}

SpotifyPanelState* build_spotify_panel() {
    auto* st = new SpotifyPanelState();
    st->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(st->root), 6);

    g_signal_connect(st->root, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        auto* s = static_cast<SpotifyPanelState*>(data);
        s->alive = false;
        delete s;
    }), st);

    GtkWidget* notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(st->root), notebook, TRUE, TRUE, 0);

    // -------------------------------------------------------------------------
    // Tab 1: Song Search
    // -------------------------------------------------------------------------
    GtkWidget* song_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(song_box), 6);

    GtkWidget* filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    st->track_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->track_entry), "Track...");
    st->artist_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->artist_entry), "Artist...");
    st->album_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->album_entry), "Album...");
    GtkWidget* search_btn = gtk_button_new_with_label("Search");
    st->load_more_btn = gtk_button_new_with_label("Load More");
    gtk_widget_set_sensitive(st->load_more_btn, FALSE);

    gtk_box_pack_start(GTK_BOX(filter_box), st->track_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(filter_box), st->artist_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(filter_box), st->album_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(filter_box), search_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter_box), st->load_more_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(song_box), filter_box, FALSE, FALSE, 0);

    st->song_store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    st->song_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st->song_store));
    g_object_unref(st->song_store);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(st->song_tree)), GTK_SELECTION_MULTIPLE);
    add_text_column(st->song_tree, "Title", 0, true);
    add_text_column(st->song_tree, "Artist", 1, true);
    add_text_column(st->song_tree, "Album", 2, true);
    add_text_column(st->song_tree, "Duration", 3, false);

    GtkWidget* song_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(song_scroll), st->song_tree);
    gtk_box_pack_start(GTK_BOX(song_box), song_scroll, TRUE, TRUE, 0);

    GtkWidget* song_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* add_song_btn = gtk_button_new_with_label("Add Selected");
    GtkWidget* play_song_btn = gtk_button_new_with_label("Play Selected");
    gtk_box_pack_start(GTK_BOX(song_actions), add_song_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(song_actions), play_song_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(song_box), song_actions, FALSE, FALSE, 0);

    g_signal_connect(search_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        run_song_search(static_cast<SpotifyPanelState*>(d), false);
    }), st);
    g_signal_connect(st->track_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) {
        run_song_search(static_cast<SpotifyPanelState*>(d), false);
    }), st);
    g_signal_connect(st->artist_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) {
        run_song_search(static_cast<SpotifyPanelState*>(d), false);
    }), st);
    g_signal_connect(st->album_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) {
        run_song_search(static_cast<SpotifyPanelState*>(d), false);
    }), st);
    g_signal_connect(st->load_more_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        run_song_search(static_cast<SpotifyPanelState*>(d), true);
    }), st);
    g_signal_connect(add_song_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        add_tracks_to_playlist(get_selected_tracks(s->song_tree, s->song_results), false);
    }), st);
    g_signal_connect(play_song_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        add_tracks_to_playlist(get_selected_tracks(s->song_tree, s->song_results), true);
    }), st);
    g_signal_connect(st->song_tree, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        int* idx = gtk_tree_path_get_indices(path);
        if (idx && static_cast<size_t>(idx[0]) < s->song_results.size()) {
            add_tracks_to_playlist({s->song_results[idx[0]]}, true);
        }
    }), st);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), song_box, gtk_label_new("Song Search"));

    // -------------------------------------------------------------------------
    // Tab 2: Artist Discography
    // -------------------------------------------------------------------------
    GtkWidget* discog_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(discog_box), 6);

    GtkWidget* d_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    st->discog_artist_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(st->discog_artist_entry), "Artist name...");
    GtkWidget* d_search_btn = gtk_button_new_with_label("Search Artist");
    GtkWidget* add_album_btn = gtk_button_new_with_label("Add Full Album");
    gtk_box_pack_start(GTK_BOX(d_top), st->discog_artist_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(d_top), d_search_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(d_top), add_album_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(discog_box), d_top, FALSE, FALSE, 0);

    st->artist_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    st->artist_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st->artist_store));
    g_object_unref(st->artist_store);
    add_text_column(st->artist_tree, "Artist", 0, true);
    add_text_column(st->artist_tree, "Genres", 1, true);

    st->album_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    st->album_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st->album_store));
    g_object_unref(st->album_store);
    add_text_column(st->album_tree, "Date", 0, false);
    add_text_column(st->album_tree, "Type", 1, false);
    add_text_column(st->album_tree, "Album", 2, true);

    st->album_track_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    st->album_track_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st->album_track_store));
    g_object_unref(st->album_track_store);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(st->album_track_tree)), GTK_SELECTION_MULTIPLE);
    add_text_column(st->album_track_tree, "#", 0, false);
    add_text_column(st->album_track_tree, "Title", 1, true);
    add_text_column(st->album_track_tree, "Duration", 2, false);

    auto make_scroll = [](GtkWidget* w) {
        GtkWidget* sc = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_container_add(GTK_CONTAINER(sc), w);
        return sc;
    };

    GtkWidget* paned2 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned2), make_scroll(st->album_tree), TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned2), make_scroll(st->album_track_tree), TRUE, FALSE);

    GtkWidget* paned1 = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned1), make_scroll(st->artist_tree), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned1), paned2, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(discog_box), paned1, TRUE, TRUE, 0);

    g_signal_connect(d_search_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        run_artist_search(static_cast<SpotifyPanelState*>(d));
    }), st);
    g_signal_connect(st->discog_artist_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer d) {
        run_artist_search(static_cast<SpotifyPanelState*>(d));
    }), st);
    g_signal_connect(add_album_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        add_tracks_to_playlist(s->album_track_results, false);
    }), st);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(st->artist_tree)), "changed",
                     G_CALLBACK(+[](GtkTreeSelection* sel, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        GtkTreeModel* model = nullptr;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
            int* idx = gtk_tree_path_get_indices(path);
            if (idx && static_cast<size_t>(idx[0]) < s->artist_results.size()) {
                load_artist_albums(s, s->artist_results[idx[0]]);
            }
            gtk_tree_path_free(path);
        }
    }), st);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(st->album_tree)), "changed",
                     G_CALLBACK(+[](GtkTreeSelection* sel, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        GtkTreeModel* model = nullptr;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
            int* idx = gtk_tree_path_get_indices(path);
            if (idx && static_cast<size_t>(idx[0]) < s->album_results.size()) {
                load_album_tracks(s, s->album_results[idx[0]]);
            }
            gtk_tree_path_free(path);
        }
    }), st);
    g_signal_connect(st->album_track_tree, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer d) {
        auto* s = static_cast<SpotifyPanelState*>(d);
        int* idx = gtk_tree_path_get_indices(path);
        if (idx && static_cast<size_t>(idx[0]) < s->album_track_results.size()) {
            add_tracks_to_playlist({s->album_track_results[idx[0]]}, true);
        }
    }), st);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), discog_box, gtk_label_new("Artist Discography"));

    // Status bar
    st->status_label = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(st->status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(st->root), st->status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(st->root);
    return st;
}

struct SpotifyGtkWidget {
    ddb_gtkui_widget_t base;
    SpotifyPanelState* panel;
};

ddb_gtkui_widget_t* create_spotify_widget() {
    auto* w = new SpotifyGtkWidget();
    std::memset(&w->base, 0, sizeof(ddb_gtkui_widget_t));
    w->panel = build_spotify_panel();
    w->base.widget = w->panel->root;
    w->base.destroy = [](ddb_gtkui_widget_t* base_w) {
        delete reinterpret_cast<SpotifyGtkWidget*>(base_w);
    };
    if (gtkui) {
        gtkui->w_override_signals(w->base.widget, w);
    }
    return &w->base;
}

int open_spotify_window_action(DB_plugin_action_t* /*action*/, int /*ctx*/) {
    g_idle_add([](gpointer) -> gboolean {
        if (standalone_window) {
            gtk_window_present(GTK_WINDOW(standalone_window));
            return G_SOURCE_REMOVE;
        }
        standalone_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(standalone_window), "Spotify Search & Discography");
        gtk_window_set_default_size(GTK_WINDOW(standalone_window), 850, 520);
        g_signal_connect(standalone_window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
            standalone_window = nullptr;
        }), nullptr);

        auto* panel = build_spotify_panel();
        gtk_container_add(GTK_CONTAINER(standalone_window), panel->root);
        gtk_widget_show_all(standalone_window);
        return G_SOURCE_REMOVE;
    }, nullptr);
    return 0;
}

DB_plugin_action_t spotify_action = {
    .title = "View/Spotify Search...",
    .name = "spotify_search_open",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback = nullptr,
    .next = nullptr,
    .callback2 = open_spotify_window_action,
};

DB_plugin_action_t* spotify_get_actions(DB_playItem_t* it) {
    if (it != nullptr) return nullptr;
    return &spotify_action;
}

int gtkui_connect() {
    if (!deadbeef) return -1;
    gtkui = reinterpret_cast<ddb_gtkui_t*>(deadbeef->plug_get_for_id(DDB_GTKUI_PLUGIN_ID));
    if (gtkui && gtkui->gui.plugin.version_major == 2) {
        gtkui->w_reg_widget("Spotify Search & Discography", 0, create_spotify_widget, "spotify_search", nullptr);
    }
    sync_spotify_credentials();
    return 0;
}

int gtkui_disconnect() {
    if (gtkui && gtkui->gui.plugin.version_major == 2) {
        gtkui->w_unreg_widget("spotify_search");
    }
    gtkui = nullptr;
    return 0;
}

int gtkui_message(uint32_t id, uintptr_t /*ctx*/, uint32_t /*p1*/, uint32_t /*p2*/) {
    if (id == DB_EV_CONFIGCHANGED) {
        sync_spotify_credentials();
    }
    return 0;
}

const char gtkui_configdialog[] =
    "property \"Spotify Web API Client ID\" entry spotify_soloist.client_id \"\";\n"
    "property \"Spotify Web API Client Secret\" password spotify_soloist.client_secret \"\";\n";

DB_misc_t plugin = {
    .plugin = {
        .type = DB_PLUGIN_MISC,
        .api_vmajor = 1,
        .api_vminor = 10,
        .version_major = 0,
        .version_minor = 1,
        .flags = 0,
        .reserved1 = 0,
        .reserved2 = 0,
        .reserved3 = 0,
        .id = "spotify_search_ui",
        .name = "Spotify Search UI (GTK3)",
        .descr = "Spotify Web API Song Search & Artist Discography panel for DeaDBeeF.",
        .copyright = "MIT",
        .website = "https://github.com/Kleyguerth/deadbeef-spotify-soloist-plugin",
        .command = nullptr,
        .start = nullptr,
        .stop = nullptr,
        .connect = gtkui_connect,
        .disconnect = gtkui_disconnect,
        .exec_cmdline = nullptr,
        .get_actions = spotify_get_actions,
        .message = gtkui_message,
        .configdialog = gtkui_configdialog,
    },
};

} // namespace

extern "C" DB_plugin_t* ddb_spotify_gtkui_load(DB_functions_t* api) {
    deadbeef = api;
    return DB_PLUGIN(&plugin);
}
