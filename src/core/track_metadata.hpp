#pragma once

#include "core/bridge_export.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace spotify_bridge {

struct SPOTIFY_BRIDGE_API TrackMetadata {
    std::string location;    // Decoded URI or path (e.g. "spotify:track:09HdqqBx6wlfJ3QAlOonAt")
    std::string title;
    std::string artist;      // XSPF <creator>
    std::string album;
    int64_t duration_ms = 0; // XSPF <duration>
    int track_num = 0;       // XSPF <trackNum>
    std::string image_url;   // Decoded cover art URL (e.g. "https://i.scdn.co/image/...")

    bool is_spotify() const;
};

/// Returns true if the string is a Spotify URI ("spotify:track:..." or "spotify://track/...").
SPOTIFY_BRIDGE_API bool is_spotify_uri(const std::string& uri);

/// Converts "spotify://track/<id>" to canonical "spotify:track:<id>" (or returns unchanged).
SPOTIFY_BRIDGE_API std::string to_canonical_spotify_uri(const std::string& uri);

/// Converts "spotify:track:<id>" to Audacious-compatible "spotify://track/<id>".
SPOTIFY_BRIDGE_API std::string to_audacious_spotify_uri(const std::string& uri);

/// Extracts the track ID from "spotify:track:<id>" or "spotify://track/<id>".
SPOTIFY_BRIDGE_API std::optional<std::string> extract_spotify_track_id(const std::string& uri);

/// Decodes percent-encoded characters (e.g., "%3A" -> ":").
SPOTIFY_BRIDGE_API std::string percent_decode_uri(const std::string& encoded);

/// Percent-encodes colons (":" -> "%3A") as used in XSPF <location> and <image> tags.
SPOTIFY_BRIDGE_API std::string percent_encode_xspf_uri(const std::string& uri);

} // namespace spotify_bridge
