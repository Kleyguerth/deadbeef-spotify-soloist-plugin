#pragma once

#include "core/bridge_export.hpp"
#include "core/track_metadata.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace spotify_bridge {

struct SPOTIFY_BRIDGE_API SpotifyArtist {
    std::string id;
    std::string name;
    std::string genres;
};

struct SPOTIFY_BRIDGE_API SpotifyAlbum {
    std::string id;
    std::string name;
    std::string release_date; // YYYY-MM-DD or YYYY
    std::string album_type;   // "album", "single", etc.
    std::string artist_name;
    std::string image_url;
    int total_tracks = 0;
};

class SPOTIFY_BRIDGE_API SpotifyApiClient {
public:
    static SpotifyApiClient& instance();

    void set_credentials(const std::string& client_id, const std::string& client_secret);
    bool has_credentials() const;

    /// Searches Spotify Web API for tracks matching `query` (limit capped at 10 for Dev Mode).
    std::vector<TrackMetadata> search_tracks(const std::string& query, int limit = 10,
                                             std::string* error_out = nullptr);

    /// Fine-grained track search with optional track, artist, and album filters + pagination offset.
    std::vector<TrackMetadata> search_tracks_advanced(const std::string& track,
                                                      const std::string& artist,
                                                      const std::string& album,
                                                      int limit = 10,
                                                      int offset = 0,
                                                      std::string* error_out = nullptr);

    /// Searches Spotify Web API for artists matching `query`.
    std::vector<SpotifyArtist> search_artists(const std::string& query, int limit = 10,
                                              std::string* error_out = nullptr);

    /// Fetches an artist's albums/singles ordered by release_date (paginated).
    std::vector<SpotifyAlbum> fetch_artist_albums(const std::string& artist_id,
                                                  std::string* error_out = nullptr);

    /// Fetches all tracks for a given album and caches their metadata.
    std::vector<TrackMetadata> fetch_album_tracks(const SpotifyAlbum& album,
                                                  std::string* error_out = nullptr);

    /// Fetches metadata for a single Spotify track ID (e.g. "09HdqqBx6wlfJ3QAlOonAt").
    std::optional<TrackMetadata> fetch_track(const std::string& track_id,
                                             std::string* error_out = nullptr);

    /// Downloads raw bytes from an HTTPS URL (used for album cover art).
    std::vector<char> fetch_bytes(const std::string& url);

private:
    SpotifyApiClient() = default;

    bool ensure_access_token(std::string* error_out);
    std::string http_get(const std::string& url, const std::string& bearer_token, long* http_code);

    mutable std::mutex mutex_;
    std::string client_id_;
    std::string client_secret_;
    std::string access_token_;
    std::chrono::steady_clock::time_point token_expiry_;
};

} // namespace spotify_bridge
