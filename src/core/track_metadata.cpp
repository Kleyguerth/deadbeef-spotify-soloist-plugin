#include "core/track_metadata.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace spotify_bridge {

namespace {

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool TrackMetadata::is_spotify() const {
    return is_spotify_uri(location);
}

bool is_spotify_uri(const std::string& uri) {
    return uri.rfind("spotify://", 0) == 0 ||
           (uri.rfind("spotify:", 0) == 0 && uri.size() > 8);
}

std::string to_canonical_spotify_uri(const std::string& uri) {
    constexpr const char* kAudPrefix = "spotify://track/";
    constexpr size_t kAudLen = 16;
    if (uri.rfind(kAudPrefix, 0) == 0 && uri.size() > kAudLen) {
        return "spotify:track:" + uri.substr(kAudLen);
    }
    return uri;
}

std::string to_audacious_spotify_uri(const std::string& uri) {
    constexpr const char* kCanonPrefix = "spotify:track:";
    constexpr size_t kCanonLen = 14;
    if (uri.rfind(kCanonPrefix, 0) == 0 && uri.size() > kCanonLen) {
        return "spotify://track/" + uri.substr(kCanonLen);
    }
    return uri;
}

std::optional<std::string> extract_spotify_track_id(const std::string& uri) {
    std::string canon = to_canonical_spotify_uri(uri);
    constexpr const char* kTrackPrefix = "spotify:track:";
    constexpr size_t kPrefixLen = 14;
    if (canon.rfind(kTrackPrefix, 0) != 0 || canon.size() <= kPrefixLen) {
        return std::nullopt;
    }
    return canon.substr(kPrefixLen);
}

std::string percent_decode_uri(const std::string& encoded) {
    std::string out;
    out.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int hi = hex_val(encoded[i + 1]);
            int lo = hex_val(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(encoded[i]);
    }
    return out;
}

std::string percent_encode_xspf_uri(const std::string& uri) {
    std::ostringstream out;
    out << std::uppercase << std::hex;

    for (unsigned char c : uri) {
        // Preserve standard unreserved + URI path/query characters, encode ':' as %3A
        // matching Strawberry / Audacious XSPF output.
        if (c == ':') {
            out << "%3A";
        } else if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
                   c == '/' || c == '?' || c == '&' || c == '=' || c == '+') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

} // namespace spotify_bridge
