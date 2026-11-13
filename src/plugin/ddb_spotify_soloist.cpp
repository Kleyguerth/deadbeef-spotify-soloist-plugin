#include "core/soloist_client.hpp"
#include "core/track_metadata.hpp"

#define DDB_API_LEVEL 10
#include <deadbeef/deadbeef.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace spotify_bridge;

namespace {

DB_functions_t* deadbeef = nullptr;

struct SpotifyFileInfo {
    DB_fileinfo_t info;
    std::string uri;
    std::string canon_uri;
    int64_t duration_ms = 0;
    int64_t elapsed_ms = 0;
    bool saw_playing = false;
    bool aborted = false;
    std::chrono::steady_clock::time_point last_tick;
};

std::string get_conf_str(const char* key, const char* def) {
    char buf[1024]{};
    deadbeef->conf_get_str(key, def, buf, sizeof(buf));
    return std::string(buf);
}

void apply_soloist_config() {
    if (!deadbeef) return;
    int port = deadbeef->conf_get_int("spotify_soloist.ws_port", 9090);
    bool auto_spawn = deadbeef->conf_get_int("spotify_soloist.auto_spawn", 1) != 0;
    std::string path = get_conf_str("spotify_soloist.soloist_path", "");
    std::string dev = get_conf_str("spotify_soloist.device_name", "");
    std::string key = get_conf_str("spotify_soloist.api_key", "");
    SoloistClient::instance().configure_launch(port, path, key, dev, auto_spawn);
}

void sync_soloist_volume() {
    if (!deadbeef) return;
    int pct = static_cast<int>(std::lround(deadbeef->volume_get_amp() * 100.0f));
    SoloistClient::instance().set_volume(std::clamp(pct, 0, 100));
}

bool is_item_spotify(DB_playItem_t* it) {
    if (!it || !deadbeef) return false;
    deadbeef->pl_lock();
    const char* raw_uri = deadbeef->pl_find_meta(it, ":URI");
    bool result = raw_uri && is_spotify_uri(raw_uri);
    deadbeef->pl_unlock();
    return result;
}

extern DB_decoder_t plugin;

DB_fileinfo_t* spotify_open(uint32_t /*hints*/) {
    auto* fi = new SpotifyFileInfo();
    std::memset(&fi->info, 0, sizeof(DB_fileinfo_t));
    fi->info.plugin = &plugin;
    fi->info.fmt.bps = 16;
    fi->info.fmt.channels = 2;
    fi->info.fmt.samplerate = 44100;
    fi->info.fmt.channelmask = DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT;
    fi->info.fmt.is_float = 0;
    return &fi->info;
}

int spotify_init(DB_fileinfo_t* base_info, DB_playItem_t* it) {
    auto* fi = reinterpret_cast<SpotifyFileInfo*>(base_info);
    if (!fi || !it || !deadbeef) return -1;

    deadbeef->pl_lock();
    const char* raw_uri = deadbeef->pl_find_meta(it, ":URI");
    std::string uri = raw_uri ? raw_uri : "";
    float dur_sec = deadbeef->pl_get_item_duration(it);
    if (is_spotify_uri(uri) && !deadbeef->pl_find_meta(it, ":FILETYPE")) {
        deadbeef->pl_add_meta(it, ":FILETYPE", "Spotify");
    }
    deadbeef->pl_unlock();

    if (!is_spotify_uri(uri)) {
        return -1;
    }

    fi->uri = uri;
    fi->canon_uri = to_canonical_spotify_uri(uri);
    fi->duration_ms = (dur_sec > 0.0f) ? static_cast<int64_t>(dur_sec * 1000.0f) : 0;
    fi->elapsed_ms = 0;
    fi->saw_playing = false;
    fi->aborted = false;
    fi->info.readpos = 0.0f;

    apply_soloist_config();
    auto& soloist = SoloistClient::instance();
    soloist.connect_to_server();
    sync_soloist_volume();
    soloist.play_uri(uri);

    auto start_wait = std::chrono::steady_clock::now();
    while (!soloist.is_connected()) {
        if (soloist.has_fatal_error()) {
            std::fprintf(stderr, "[deadbeef-spotify] init() aborting: %s\n",
                         soloist.last_error().c_str());
            return -1;
        }
        if (std::chrono::steady_clock::now() - start_wait > std::chrono::seconds(10)) {
            std::fprintf(stderr, "[deadbeef-spotify] init() timed out waiting for Soloist.\n");
            return -1;
        }
        soloist.connect_to_server();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    fi->last_tick = std::chrono::steady_clock::now();
    return 0;
}

void spotify_free(DB_fileinfo_t* base_info) {
    delete reinterpret_cast<SpotifyFileInfo*>(base_info);
}

int spotify_read(DB_fileinfo_t* base_info, char* buffer, int nbytes) {
    auto* fi = reinterpret_cast<SpotifyFileInfo*>(base_info);
    if (!fi || fi->aborted || nbytes <= 0) return 0;

    auto& soloist = SoloistClient::instance();
    SoloistPlaybackState st = soloist.state();
    if (st.duration_ms > 0 && fi->duration_ms <= 0) {
        fi->duration_ms = st.duration_ms;
    }
    if (st.current_uri == fi->canon_uri && st.status == SoloistStatus::Playing) {
        fi->saw_playing = true;
    }

    if (fi->saw_playing && st.status == SoloistStatus::Stopped) {
        return 0;
    }
    if (fi->duration_ms > 0 && fi->elapsed_ms >= fi->duration_ms + 1000) {
        return 0;
    }

    // Pace silent PCM output to real-time (44100 Hz * 2 ch * 2 bytes = 176400 bytes/sec)
    constexpr int kBytesPerSec = 44100 * 2 * 2;
    int chunk_ms = (nbytes * 1000) / kBytesPerSec;
    if (chunk_ms > 0) {
        auto target = fi->last_tick + std::chrono::milliseconds(chunk_ms);
        auto now = std::chrono::steady_clock::now();
        if (target > now) {
            std::this_thread::sleep_for(target - now);
        }
        fi->last_tick = std::chrono::steady_clock::now();
    }

    std::memset(buffer, 0, static_cast<size_t>(nbytes));
    fi->elapsed_ms += chunk_ms;
    fi->info.readpos = static_cast<float>(fi->elapsed_ms) / 1000.0f;
    return nbytes;
}

int spotify_seek_sample(DB_fileinfo_t* base_info, int sample) {
    auto* fi = reinterpret_cast<SpotifyFileInfo*>(base_info);
    if (!fi) return -1;
    int64_t pos_ms = (static_cast<int64_t>(sample) * 1000) / 44100;
    SoloistClient::instance().seek(pos_ms);
    fi->elapsed_ms = pos_ms;
    fi->info.readpos = static_cast<float>(pos_ms) / 1000.0f;
    fi->last_tick = std::chrono::steady_clock::now();
    return 0;
}

int spotify_seek(DB_fileinfo_t* base_info, float seconds) {
    return spotify_seek_sample(base_info, static_cast<int>(seconds * 44100.0f));
}

DB_playItem_t* spotify_insert(ddb_playlist_t* plt, DB_playItem_t* after, const char* fname) {
    if (!fname || !is_spotify_uri(fname) || !deadbeef) return nullptr;
    DB_playItem_t* it = deadbeef->pl_item_alloc_init(fname, "spotify_soloist");
    if (!it) return nullptr;
    deadbeef->pl_add_meta(it, ":FILETYPE", "Spotify");
    after = deadbeef->plt_insert_item(plt, after, it);
    deadbeef->pl_item_unref(it);
    return after;
}

int spotify_start() {
    apply_soloist_config();
    return 0;
}

int spotify_stop() {
    SoloistClient::instance().disconnect_from_server();
    SoloistClient::instance().shutdown_spawned_process();
    return 0;
}

int spotify_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t /*p2*/) {
    switch (id) {
        case DB_EV_PAUSED:
            if (p1) {
                SoloistClient::instance().pause();
            } else {
                SoloistClient::instance().resume();
            }
            break;
        case DB_EV_STOP:
            SoloistClient::instance().pause();
            break;
        case DB_EV_SONGCHANGED: {
            auto* tc = reinterpret_cast<ddb_event_trackchange_t*>(ctx);
            if (tc && is_item_spotify(tc->from) && !is_item_spotify(tc->to)) {
                SoloistClient::instance().pause();
            }
            break;
        }
        case DB_EV_VOLUMECHANGED:
            sync_soloist_volume();
            break;
        case DB_EV_CONFIGCHANGED:
            apply_soloist_config();
            break;
        default:
            break;
    }
    return 0;
}

const char* spotify_exts[] = { nullptr };
const char* spotify_prefixes[] = { "spotify:", nullptr };

const char spotify_configdialog[] =
    "property \"Soloist WebSocket Port\" spinbtn[1024,65535,1] spotify_soloist.ws_port 9090;\n"
    "property \"Auto-start Soloist if not running\" checkbox spotify_soloist.auto_spawn 1;\n"
    "property \"Soloist Device Name (blank = DeaDBeeF-<host>)\" entry spotify_soloist.device_name \"\";\n"
    "property \"Soloist Executable Path (blank = PATH)\" file spotify_soloist.soloist_path \"\";\n"
    "property \"Soloist API Key\" password spotify_soloist.api_key \"\";\n";

DB_decoder_t plugin = {
    .plugin = {
        .type = DB_PLUGIN_DECODER,
        .api_vmajor = 1,
        .api_vminor = 10,
        .version_major = 0,
        .version_minor = 1,
        .flags = 0,
        .reserved1 = 0,
        .reserved2 = 0,
        .reserved3 = 0,
        .id = "spotify_soloist",
        .name = "Spotify Soloist Playback",
        .descr = "Plays spotify://track/... URIs through Spotify Soloist WebSocket daemon.",
        .copyright = "MIT",
        .website = "https://github.com/Kleyguerth/deadbeef-spotify-soloist-plugin",
        .command = nullptr,
        .start = spotify_start,
        .stop = spotify_stop,
        .connect = nullptr,
        .disconnect = nullptr,
        .exec_cmdline = nullptr,
        .get_actions = nullptr,
        .message = spotify_message,
        .configdialog = spotify_configdialog,
    },
    .open = spotify_open,
    .init = spotify_init,
    .free = spotify_free,
    .read = spotify_read,
    .seek = spotify_seek,
    .seek_sample = spotify_seek_sample,
    .insert = spotify_insert,
    .numvoices = nullptr,
    .mutevoice = nullptr,
    .read_metadata = nullptr,
    .write_metadata = nullptr,
    .exts = spotify_exts,
    .prefixes = spotify_prefixes,
    .open2 = nullptr,
};

} // namespace

extern "C" DB_plugin_t* ddb_spotify_soloist_load(DB_functions_t* api) {
    deadbeef = api;
    return DB_PLUGIN(&plugin);
}
