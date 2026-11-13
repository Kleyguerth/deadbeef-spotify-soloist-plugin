#pragma once

#include <cstdint>
#include <string>

namespace spotify_bridge {

enum class SoloistStatus {
    Unknown,
    Stopped,
    Paused,
    Playing
};

struct SoloistPlaybackState {
    SoloistStatus status = SoloistStatus::Unknown;
    bool is_active = false;
    bool logged_in = false;
    std::string current_uri;
    std::string track_name;
    std::string cover_url;
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
    int volume = 100;
};

enum class SoloistEventType {
    Unknown,
    AuthState,
    PlaybackState,
    TrackChanged,
    PlaybackChanged,
    PositionSync,
    CommandResult
};

// JSON command builders for Spotify Soloist WebSocket API
std::string build_play_command(const std::string& uri = "");
std::string build_pause_command();
std::string build_seek_command(int64_t position_ms);
std::string build_set_volume_command(int volume);
std::string build_skip_next_command();
std::string build_skip_prev_command();

// Parses an incoming Soloist WebSocket JSON message and updates `state`.
SoloistEventType parse_soloist_event(const std::string& json_text, SoloistPlaybackState& state);

} // namespace spotify_bridge
