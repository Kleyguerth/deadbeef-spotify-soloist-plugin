#pragma once

#include "core/bridge_export.hpp"
#include "core/track_metadata.hpp"

#include <optional>
#include <string>
#include <vector>

namespace spotify_bridge {

struct XspfPlaylist {
    std::string title;
    std::vector<TrackMetadata> tracks;
};

/// Parses an XSPF XML string into an XspfPlaylist. Returns std::nullopt on XML parse failure.
SPOTIFY_BRIDGE_API std::optional<XspfPlaylist> parse_xspf(const std::string& xml_content);

/// Serializes an XspfPlaylist into an XSPF 1.0 XML string.
SPOTIFY_BRIDGE_API std::string serialize_xspf(const XspfPlaylist& playlist);

} // namespace spotify_bridge
