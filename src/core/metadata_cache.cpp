#include "core/metadata_cache.hpp"

#include <mutex>

namespace spotify_bridge {

MetadataCache& MetadataCache::instance() {
    static MetadataCache cache;
    return cache;
}

void MetadataCache::put(const TrackMetadata& meta) {
    if (meta.location.empty()) return;
    std::string key = to_canonical_spotify_uri(meta.location);
    std::unique_lock lock(mutex_);
    entries_[key] = meta;
}

std::optional<TrackMetadata> MetadataCache::get(const std::string& uri) const {
    std::string key = to_canonical_spotify_uri(uri);
    std::shared_lock lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

bool MetadataCache::contains(const std::string& uri) const {
    std::string key = to_canonical_spotify_uri(uri);
    std::shared_lock lock(mutex_);
    return entries_.find(key) != entries_.end();
}

void MetadataCache::clear() {
    std::unique_lock lock(mutex_);
    entries_.clear();
}

} // namespace spotify_bridge
