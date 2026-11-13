#include "core/soloist_protocol.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace spotify_bridge {

using json = nlohmann::json;

namespace {

SoloistStatus parse_status(const std::string& s) {
    if (s == "playing") return SoloistStatus::Playing;
    if (s == "paused") return SoloistStatus::Paused;
    if (s == "stopped" || s == "idle") return SoloistStatus::Stopped;
    return SoloistStatus::Unknown;
}

void parse_item(const json& item, SoloistPlaybackState& state) {
    if (!item.is_object()) return;

    if (item.contains("uri") && item["uri"].is_string()) {
        state.current_uri = item["uri"].get<std::string>();
    }
    if (item.contains("decorations") && item["decorations"].is_object()) {
        const auto& dec = item["decorations"];
        if (dec.contains("identity") && dec["identity"].contains("name") &&
            dec["identity"]["name"].is_string()) {
            state.track_name = dec["identity"]["name"].get<std::string>();
        }
        if (dec.contains("playback") && dec["playback"].contains("duration_ms") &&
            dec["playback"]["duration_ms"].is_number_integer()) {
            state.duration_ms = dec["playback"]["duration_ms"].get<int64_t>();
        }
        if (dec.contains("visual_identity") && dec["visual_identity"].contains("cover") &&
            dec["visual_identity"]["cover"].is_array() && !dec["visual_identity"]["cover"].empty()) {
            const auto& first = dec["visual_identity"]["cover"][0];
            if (first.contains("url") && first["url"].is_string()) {
                state.cover_url = first["url"].get<std::string>();
            }
        }
    }
}

} // namespace

std::string build_play_command(const std::string& uri) {
    json j = {
        {"type", "command"},
        {"command", "play"}
    };
    if (!uri.empty()) {
        j["uri"] = uri;
    }
    return j.dump();
}

std::string build_pause_command() {
    return json{
        {"type", "command"},
        {"command", "pause"}
    }.dump();
}

std::string build_seek_command(int64_t position_ms) {
    return json{
        {"type", "command"},
        {"command", "seek"},
        {"position_ms", std::max<int64_t>(0, position_ms)}
    }.dump();
}

std::string build_set_volume_command(int volume) {
    return json{
        {"type", "command"},
        {"command", "set_volume"},
        {"volume", std::clamp(volume, 0, 100)}
    }.dump();
}

std::string build_skip_next_command() {
    return json{
        {"type", "command"},
        {"command", "skip_next"}
    }.dump();
}

std::string build_skip_prev_command() {
    return json{
        {"type", "command"},
        {"command", "skip_prev"}
    }.dump();
}

SoloistEventType parse_soloist_event(const std::string& json_text, SoloistPlaybackState& state) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return SoloistEventType::Unknown;
    }

    const std::string type = j.value("type", "");

    if (j.contains("status") && j["status"].is_string()) {
        state.status = parse_status(j["status"].get<std::string>());
    }
    if (j.contains("is_active") && j["is_active"].is_boolean()) {
        state.is_active = j["is_active"].get<bool>();
    }
    if (j.contains("logged_in") && j["logged_in"].is_boolean()) {
        state.logged_in = j["logged_in"].get<bool>();
    }
    if (j.contains("position_ms") && j["position_ms"].is_number_integer()) {
        state.position_ms = j["position_ms"].get<int64_t>();
    }
    if (j.contains("volume") && j["volume"].is_number_integer()) {
        state.volume = j["volume"].get<int>();
    }
    if (j.contains("item")) {
        parse_item(j["item"], state);
    }

    if (type == "auth_state") return SoloistEventType::AuthState;
    if (type == "playback_state") return SoloistEventType::PlaybackState;
    if (type == "track_changed") return SoloistEventType::TrackChanged;
    if (type == "playback_changed") return SoloistEventType::PlaybackChanged;
    if (type == "position_sync") return SoloistEventType::PositionSync;
    if (type == "command_result") return SoloistEventType::CommandResult;

    return SoloistEventType::Unknown;
}

} // namespace spotify_bridge
