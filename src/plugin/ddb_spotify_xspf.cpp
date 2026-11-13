#include "core/track_metadata.hpp"
#include "core/xspf_codec.hpp"

#define DDB_API_LEVEL 10
#include <deadbeef/deadbeef.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace spotify_bridge;

namespace {

DB_functions_t* deadbeef = nullptr;

std::string read_vfs_file(const char* fname) {
    DB_FILE* fp = deadbeef->fopen(fname);
    if (!fp) {
        std::fprintf(stderr, "[deadbeef-spotify-xspf] fopen failed for '%s'\n", fname);
        return {};
    }
    int64_t len = deadbeef->fgetlength(fp);
    std::string content;
    if (len > 0 && len < 64 * 1024 * 1024) {
        content.resize(static_cast<size_t>(len));
        size_t got = deadbeef->fread(content.data(), 1, static_cast<size_t>(len), fp);
        content.resize(got);
    } else {
        char buf[8192];
        size_t got = 0;
        while ((got = deadbeef->fread(buf, 1, sizeof(buf), fp)) > 0) {
            content.append(buf, got);
        }
    }
    deadbeef->fclose(fp);
    return content;
}

DB_playItem_t* insert_xspf_item(ddb_playlist_t* plt, DB_playItem_t* after,
                               const TrackMetadata& t, const std::string& uri,
                               const char* decoder_id,
                               int (*cb)(DB_playItem_t* it, void* data), void* user_data,
                               bool& aborted) {
    DB_playItem_t* it = deadbeef->pl_item_alloc_init(uri.c_str(), decoder_id);
    if (!it) return after;

    if (is_spotify_uri(uri)) {
        deadbeef->pl_add_meta(it, ":FILETYPE", "Spotify");
    }
    if (!t.title.empty()) deadbeef->pl_add_meta(it, "title", t.title.c_str());
    if (!t.artist.empty()) deadbeef->pl_add_meta(it, "artist", t.artist.c_str());
    if (!t.album.empty()) deadbeef->pl_add_meta(it, "album", t.album.c_str());
    if (t.track_num > 0) {
        deadbeef->pl_add_meta(it, "track", std::to_string(t.track_num).c_str());
    }
    if (!t.image_url.empty()) {
        deadbeef->pl_add_meta(it, ":SPOTIFY_IMAGE", t.image_url.c_str());
    }
    if (t.duration_ms > 0) {
        deadbeef->plt_set_item_duration(plt, it, static_cast<float>(t.duration_ms) / 1000.0f);
    }

    after = deadbeef->plt_insert_item(plt, after, it);
    if (cb && cb(it, user_data) < 0) {
        aborted = true;
    }
    deadbeef->pl_item_unref(it);
    return after;
}

DB_playItem_t* xspf_load(ddb_playlist_t* plt, DB_playItem_t* after, const char* fname,
                         int* pabort, int (*cb)(DB_playItem_t* it, void* data), void* user_data) {
    if (!fname || !deadbeef) return after;

    std::string xml = read_vfs_file(fname);
    if (xml.empty()) return after;

    auto parsed = parse_xspf(xml);
    if (!parsed.has_value()) {
        std::fprintf(stderr, "[deadbeef-spotify-xspf] parse_xspf failed for '%s'\n", fname);
        return after;
    }

    std::fprintf(stderr, "[deadbeef-spotify-xspf] Loaded %zu tracks from '%s'\n",
                 parsed->tracks.size(), fname);

    for (const auto& t : parsed->tracks) {
        if (pabort && *pabort) break;

        bool aborted = false;
        if (is_spotify_uri(t.location)) {
            std::string uri = to_audacious_spotify_uri(t.location);
            after = insert_xspf_item(plt, after, t, uri, "spotify_soloist", cb, user_data, aborted);
            if (aborted) break;
        } else if (!t.location.empty()) {
            std::string path = t.location;
#ifdef _WIN32
            if (path.rfind("file:///", 0) == 0 && path.size() > 10 && path[9] == ':') {
                path = path.substr(8); // file:///D:/foo -> D:/foo
            } else if (path.rfind("file://", 0) == 0) {
                path = path.substr(7);
            }
#else
            if (path.rfind("file://", 0) == 0) {
                path = path.substr(7);
            }
#endif
            // Resolve relative paths against the XSPF file's directory
            if (path.find("://") == std::string::npos) {
                std::filesystem::path p = std::filesystem::u8path(path);
                if (p.is_relative()) {
                    std::filesystem::path base_dir = std::filesystem::u8path(fname).parent_path();
                    if (!base_dir.empty()) {
                        auto resolved = (base_dir / p).lexically_normal().u8string();
                        path.assign(resolved.begin(), resolved.end());
                    }
                }
            }
            DB_playItem_t* inserted = deadbeef->plt_insert_file2(0, plt, after, path.c_str(), pabort, cb, user_data);
            if (inserted) {
                after = inserted;
            } else {
                // Keep entry with XSPF metadata if local file isn't present on this machine/OS
                after = insert_xspf_item(plt, after, t, path, "", cb, user_data, aborted);
                if (aborted) break;
            }
        }
    }
    return after;
}

int xspf_save(ddb_playlist_t* /*plt*/, const char* fname, DB_playItem_t* first, DB_playItem_t* last) {
    if (!fname || !deadbeef) return -1;

    XspfPlaylist playlist;
    deadbeef->pl_lock();
    for (DB_playItem_t* it = first; it != nullptr; ) {
        TrackMetadata t;
        const char* uri = deadbeef->pl_find_meta(it, ":URI");
        const char* title = deadbeef->pl_find_meta(it, "title");
        const char* artist = deadbeef->pl_find_meta(it, "artist");
        const char* album = deadbeef->pl_find_meta(it, "album");
        const char* track_str = deadbeef->pl_find_meta(it, "track");
        const char* image = deadbeef->pl_find_meta(it, ":SPOTIFY_IMAGE");

        t.location = uri ? uri : "";
        t.title = title ? title : "";
        t.artist = artist ? artist : "";
        t.album = album ? album : "";
        t.track_num = track_str ? std::atoi(track_str) : 0;
        t.image_url = image ? image : "";
        float dur = deadbeef->pl_get_item_duration(it);
        t.duration_ms = (dur > 0.0f) ? static_cast<int64_t>(dur * 1000.0f) : 0;

        if (!t.location.empty()) {
            playlist.tracks.push_back(std::move(t));
        }

        if (it == last) break;
        DB_playItem_t* next = deadbeef->pl_get_next(it, PL_MAIN);
        if (next) deadbeef->pl_item_unref(next);
        it = next;
    }
    deadbeef->pl_unlock();

    std::ofstream ofs(std::filesystem::u8path(fname), std::ios::binary);
    if (!ofs) return -1;
    ofs << serialize_xspf(playlist);
    return ofs.good() ? 0 : -1;
}

const char* xspf_exts[] = { "xspf", nullptr };

DB_playlist_t plugin = {
    .plugin = {
        .type = DB_PLUGIN_PLAYLIST,
        .api_vmajor = 1,
        .api_vminor = 10,
        .version_major = 0,
        .version_minor = 1,
        .flags = 0,
        .reserved1 = 0,
        .reserved2 = 0,
        .reserved3 = 0,
        .id = "spotify_xspf",
        .name = "Spotify XSPF Playlist",
        .descr = "Loads and saves XSPF playlists with spotify%3Atrack%3A... URIs.",
        .copyright = "MIT",
        .website = "https://github.com/Kleyguerth/deadbeef-spotify-soloist-plugin",
        .command = nullptr,
        .start = nullptr,
        .stop = nullptr,
        .connect = nullptr,
        .disconnect = nullptr,
        .exec_cmdline = nullptr,
        .get_actions = nullptr,
        .message = nullptr,
        .configdialog = nullptr,
    },
    .load = xspf_load,
    .save = xspf_save,
    .extensions = xspf_exts,
    .load2 = nullptr,
};

} // namespace

extern "C" DB_plugin_t* ddb_spotify_xspf_load(DB_functions_t* api) {
    deadbeef = api;
    return DB_PLUGIN(&plugin);
}
