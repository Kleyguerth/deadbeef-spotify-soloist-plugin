#pragma once

#include "core/bridge_export.hpp"
#include "core/track_metadata.hpp"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace spotify_bridge {

class SPOTIFY_BRIDGE_API MetadataCache {
public:
    static MetadataCache& instance();

    void put(const TrackMetadata& meta);
    std::optional<TrackMetadata> get(const std::string& uri) const;
    bool contains(const std::string& uri) const;
    void clear();

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, TrackMetadata> entries_;
};

} // namespace spotify_bridge
