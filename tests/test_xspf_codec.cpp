#include "core/xspf_codec.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace spotify_bridge;

void run_xspf_tests() {
    // 1. Round-trip test with a mixed local + Spotify playlist
    XspfPlaylist pl;
    pl.title = "Test Mix";

    TrackMetadata local;
    local.location = "Avantasia - Journey to Arcadia.mp3";
    local.title = "Journey to Arcadia";
    local.artist = "Avantasia";
    local.album = "Angel of Babylon";
    local.duration_ms = 437000;
    local.track_num = 11;

    TrackMetadata sp;
    sp.location = "spotify:track:09HdqqBx6wlfJ3QAlOonAt";
    sp.title = "Spectres";
    sp.artist = "Avantasia";
    sp.album = "The Mystery of Time";
    sp.duration_ms = 369786;
    sp.track_num = 1;
    sp.image_url = "https://i.scdn.co/image/ab67616d0000b2730f6f6e7ea0dad5d3e99da987";

    pl.tracks.push_back(local);
    pl.tracks.push_back(sp);

    std::string xml = serialize_xspf(pl);
    assert(xml.find("<location>spotify%3Atrack%3A09HdqqBx6wlfJ3QAlOonAt</location>") != std::string::npos);
    assert(xml.find("<image>https%3A//i.scdn.co/image/ab67616d0000b2730f6f6e7ea0dad5d3e99da987</image>") != std::string::npos);

    auto parsed = parse_xspf(xml);
    assert(parsed.has_value());
    assert(parsed->title == "Test Mix");
    assert(parsed->tracks.size() == 2);
    assert(!parsed->tracks[0].is_spotify());
    assert(parsed->tracks[1].is_spotify());
    assert(parsed->tracks[1].location == "spotify:track:09HdqqBx6wlfJ3QAlOonAt");
    assert(parsed->tracks[1].image_url == "https://i.scdn.co/image/ab67616d0000b2730f6f6e7ea0dad5d3e99da987");
    assert(parsed->tracks[1].duration_ms == 369786);

    // 2. Parse the real example file in examples/Album order strawberry.xspf
#ifdef EXAMPLES_DIR
    std::string example_path = std::string(EXAMPLES_DIR) + "/Album order strawberry.xspf";
    std::ifstream ifs(example_path, std::ios::binary);
    if (ifs) {
        std::ostringstream ss;
        ss << ifs.rdbuf();
        auto ex = parse_xspf(ss.str());
        assert(ex.has_value());
        assert(!ex->tracks.empty());

        bool found_spectres = false;
        size_t spotify_count = 0;
        for (const auto& t : ex->tracks) {
            if (t.is_spotify()) {
                ++spotify_count;
                if (t.location == "spotify:track:09HdqqBx6wlfJ3QAlOonAt") {
                    found_spectres = true;
                    assert(t.title == "Spectres");
                    assert(t.artist == "Avantasia");
                    assert(t.album == "The Mystery of Time");
                    assert(t.duration_ms == 369786);
                    assert(t.track_num == 1);
                    assert(t.image_url == "https://i.scdn.co/image/ab67616d0000b2730f6f6e7ea0dad5d3e99da987");
                }
            }
        }
        assert(spotify_count > 0);
        assert(found_spectres);
        std::cout << "[PASS] Parsed example XSPF with " << spotify_count << " Spotify tracks.\n";
    }
#endif
}
