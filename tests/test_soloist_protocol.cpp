#include "core/soloist_protocol.hpp"
#include "core/track_metadata.hpp"

#include <cassert>
#include <iostream>

using namespace spotify_bridge;

void run_soloist_protocol_tests() {
    auto id = extract_spotify_track_id("spotify:track:2JRo0gjbX4GrCqBYdRohoo");
    assert(id.has_value() && *id == "2JRo0gjbX4GrCqBYdRohoo");
    assert(!extract_spotify_track_id("file:///tmp/song.mp3").has_value());

    std::string play_cmd = build_play_command("spotify:track:2JRo0gjbX4GrCqBYdRohoo");
    assert(play_cmd.find("\"command\":\"play\"") != std::string::npos);
    assert(play_cmd.find("\"uri\":\"spotify:track:2JRo0gjbX4GrCqBYdRohoo\"") != std::string::npos);

    std::string seek_cmd = build_seek_command(30000);
    assert(seek_cmd.find("\"command\":\"seek\"") != std::string::npos);
    assert(seek_cmd.find("\"position_ms\":30000") != std::string::npos);

    const std::string sample_event = R"({
        "type": "playback_state",
        "status": "playing",
        "is_active": true,
        "position_ms": 45000,
        "volume": 80,
        "item": {
            "uri": "spotify:track:2JRo0gjbX4GrCqBYdRohoo",
            "decorations": {
                "identity": { "name": "My Song" },
                "playback": { "duration_ms": 204000 },
                "visual_identity": {
                    "cover": [{ "url": "https://i.scdn.co/image/abc" }]
                }
            }
        }
    })";

    SoloistPlaybackState state;
    auto ev = parse_soloist_event(sample_event, state);
    assert(ev == SoloistEventType::PlaybackState);
    assert(state.status == SoloistStatus::Playing);
    assert(state.is_active);
    assert(state.position_ms == 45000);
    assert(state.duration_ms == 204000);
    assert(state.volume == 80);
    assert(state.current_uri == "spotify:track:2JRo0gjbX4GrCqBYdRohoo");
    assert(state.track_name == "My Song");
    assert(state.cover_url == "https://i.scdn.co/image/abc");

    std::cout << "[PASS] Soloist protocol tests passed.\n";
}
