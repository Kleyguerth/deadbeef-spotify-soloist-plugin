#include "core/spotify_api_client.hpp"
#include "core/metadata_cache.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winhttp.h>
#else
  #include <curl/curl.h>
#endif

namespace spotify_bridge {

using json = nlohmann::json;

namespace {

std::string url_escape(const std::string& s) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

#ifdef _WIN32
std::string base64_encode_str(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        uint32_t b = static_cast<uint8_t>(input[i]) << 16;
        if (i + 1 < input.size()) b |= static_cast<uint8_t>(input[i + 1]) << 8;
        if (i + 2 < input.size()) b |= static_cast<uint8_t>(input[i + 2]);
        out.push_back(kTable[(b >> 18) & 0x3F]);
        out.push_back(kTable[(b >> 12) & 0x3F]);
        out.push_back((i + 1 < input.size()) ? kTable[(b >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < input.size()) ? kTable[b & 0x3F] : '=');
    }
    return out;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool winhttp_request(const std::wstring& method,
                     const std::string& url,
                     const std::wstring& extra_headers,
                     const std::string& body,
                     long* http_code,
                     std::vector<char>* out_bytes) {
    if (http_code) *http_code = 0;
    std::wstring wurl = to_wide(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        return false;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"AudaciousSpotifySoloist/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    LPCWSTR hdrs = extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str();
    DWORD hdrs_len = extra_headers.empty() ? 0 : static_cast<DWORD>(extra_headers.size());
    LPVOID body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    DWORD body_len = static_cast<DWORD>(body.size());

    BOOL ok = WinHttpSendRequest(request, hdrs, hdrs_len, body_ptr, body_len, body_len, 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);

    if (ok) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            if (http_code) *http_code = static_cast<long>(status);
        }

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
            size_t old_size = out_bytes->size();
            out_bytes->resize(old_size + avail);
            DWORD read_bytes = 0;
            if (!WinHttpReadData(request, out_bytes->data() + old_size, avail, &read_bytes)) {
                ok = FALSE;
                break;
            }
            out_bytes->resize(old_size + read_bytes);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok == TRUE;
}
#else
size_t write_string_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

size_t write_bytes_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* vec = static_cast<std::vector<char>*>(userp);
    const char* ptr = static_cast<const char*>(contents);
    vec->insert(vec->end(), ptr, ptr + total);
    return total;
}
#endif

std::string join_artists(const json& obj) {
    if (!obj.contains("artists") || !obj["artists"].is_array()) return {};
    std::string artists;
    for (size_t i = 0; i < obj["artists"].size(); ++i) {
        if (i > 0) artists += ", ";
        artists += obj["artists"][i].value("name", "");
    }
    return artists;
}

std::string extract_api_error(const std::string& prefix, long http_code, const std::string& body) {
    std::string detail;
    if (!body.empty()) {
        json err_j = json::parse(body, nullptr, false);
        if (err_j.is_object() && err_j.contains("error") && err_j["error"].contains("message")) {
            detail = ": " + err_j["error"]["message"].get<std::string>();
        }
    }
    return prefix + " (HTTP " + std::to_string(http_code) + ")" + detail;
}

TrackMetadata parse_track_json(const json& t) {
    TrackMetadata meta;
    meta.location = t.value("uri", "");
    meta.title = t.value("name", "");
    meta.duration_ms = t.value("duration_ms", int64_t{0});
    meta.track_num = t.value("track_number", 0);
    meta.artist = join_artists(t);

    if (t.contains("album") && t["album"].is_object()) {
        const auto& album = t["album"];
        meta.album = album.value("name", "");
        if (album.contains("images") && album["images"].is_array() && !album["images"].empty()) {
            meta.image_url = album["images"][0].value("url", "");
        }
    }

    return meta;
}

} // namespace

SpotifyApiClient& SpotifyApiClient::instance() {
    static SpotifyApiClient client;
    return client;
}

void SpotifyApiClient::set_credentials(const std::string& client_id, const std::string& client_secret) {
    std::lock_guard lock(mutex_);
    if (client_id_ != client_id || client_secret_ != client_secret) {
        client_id_ = client_id;
        client_secret_ = client_secret;
        access_token_.clear();
        token_expiry_ = {};
    }
}

bool SpotifyApiClient::has_credentials() const {
    std::lock_guard lock(mutex_);
    return !client_id_.empty() && !client_secret_.empty();
}

bool SpotifyApiClient::ensure_access_token(std::string* error_out) {
    auto now = std::chrono::steady_clock::now();
    if (!access_token_.empty() && now < token_expiry_) {
        return true;
    }

    if (client_id_.empty() || client_secret_.empty()) {
        if (error_out) *error_out = "Spotify Client ID and Client Secret are not configured.";
        return false;
    }

    std::string response;
    long http_code = 0;
    const std::string body = "grant_type=client_credentials";

#ifdef _WIN32
    std::string auth = base64_encode_str(client_id_ + ":" + client_secret_);
    std::wstring hdrs = L"Content-Type: application/x-www-form-urlencoded\r\n"
                        L"Authorization: Basic " + to_wide(auth) + L"\r\n";
    std::vector<char> bytes;
    bool ok = winhttp_request(L"POST", "https://accounts.spotify.com/api/token",
                              hdrs, body, &http_code, &bytes);
    if (ok) response.assign(bytes.begin(), bytes.end());
    if (!ok || http_code != 200) {
        if (error_out) {
            *error_out = "Spotify token request failed (HTTP " + std::to_string(http_code) + ").";
        }
        return false;
    }
#else
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error_out) *error_out = "Failed to initialize libcurl.";
        return false;
    }

    std::string userpwd = client_id_ + ":" + client_secret_;
    curl_easy_setopt(curl, CURLOPT_URL, "https://accounts.spotify.com/api/token");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        if (error_out) {
            *error_out = "Spotify token request failed (HTTP " + std::to_string(http_code) + ").";
        }
        return false;
    }
#endif

    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.contains("access_token")) {
        if (error_out) *error_out = "Invalid token JSON from Spotify.";
        return false;
    }

    access_token_ = j["access_token"].get<std::string>();
    int expires_in = j.value("expires_in", 3600);
    token_expiry_ = now + std::chrono::seconds(std::max(30, expires_in - 30));
    return true;
}

std::string SpotifyApiClient::http_get(const std::string& url, const std::string& bearer_token, long* http_code) {
#ifdef _WIN32
    std::wstring hdrs = L"Authorization: Bearer " + to_wide(bearer_token) + L"\r\n";
    std::vector<char> bytes;
    if (!winhttp_request(L"GET", url, hdrs, {}, http_code, &bytes)) {
        return {};
    }
    return std::string(bytes.begin(), bytes.end());
#else
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + bearer_token;
    headers = curl_slist_append(headers, auth_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code) *http_code = code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return response;
#endif
}

std::vector<TrackMetadata> SpotifyApiClient::search_tracks(const std::string& query, int limit,
                                                           std::string* error_out) {
    return search_tracks_advanced(query, "", "", limit, 0, error_out);
}

std::vector<TrackMetadata> SpotifyApiClient::search_tracks_advanced(
    const std::string& track, const std::string& artist, const std::string& album,
    int limit, int offset, std::string* error_out) {
    std::string q;
    auto append_filter = [&](const char* prefix, const std::string& val) {
        if (val.empty()) return;
        if (!q.empty()) q += " ";
        if (prefix[0] != '\0' && val.find(':') == std::string::npos) {
            q += prefix;
        }
        q += val;
    };

    if (!track.empty() && artist.empty() && album.empty()) {
        q = track;
    } else {
        append_filter("track:", track);
        append_filter("artist:", artist);
        append_filter("album:", album);
    }

    if (q.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        if (!ensure_access_token(error_out)) {
            return {};
        }
        token = access_token_;
    }

    int clamped_limit = std::clamp(limit, 1, 10);
    std::string url = "https://api.spotify.com/v1/search?type=track&limit=" +
                      std::to_string(clamped_limit) +
                      "&offset=" + std::to_string(std::max(0, offset)) +
                      "&q=" + url_escape(q);

    long http_code = 0;
    std::string body = http_get(url, token, &http_code);
    if (http_code != 200 || body.empty()) {
        if (error_out) {
            *error_out = extract_api_error("Spotify search failed", http_code, body);
        }
        return {};
    }

    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("tracks") || !j["tracks"].contains("items")) {
        if (error_out) *error_out = "Unexpected search response format.";
        return {};
    }

    std::vector<TrackMetadata> results;
    for (const auto& item : j["tracks"]["items"]) {
        TrackMetadata meta = parse_track_json(item);
        if (!meta.location.empty()) {
            MetadataCache::instance().put(meta);
            results.push_back(std::move(meta));
        }
    }

    // Spotify's strict field filters (track:/artist:/album:) disable fuzzy/typo matching.
    // If a filtered search on page 0 returns nothing, fall back to a combined free-text query.
    bool used_filters = !((!track.empty() && artist.empty() && album.empty()));
    if (results.empty() && offset == 0 && used_filters) {
        std::string plain_q;
        for (const auto& part : {track, artist, album}) {
            if (!part.empty()) {
                if (!plain_q.empty()) plain_q += " ";
                plain_q += part;
            }
        }
        return search_tracks_advanced(plain_q, "", "", limit, 0, error_out);
    }

    return results;
}

std::vector<SpotifyArtist> SpotifyApiClient::search_artists(const std::string& query, int limit,
                                                            std::string* error_out) {
    if (query.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        if (!ensure_access_token(error_out)) {
            return {};
        }
        token = access_token_;
    }

    int clamped_limit = std::clamp(limit, 1, 10);
    std::string url = "https://api.spotify.com/v1/search?type=artist&limit=" +
                      std::to_string(clamped_limit) + "&q=" + url_escape(query);

    long http_code = 0;
    std::string body = http_get(url, token, &http_code);
    if (http_code != 200 || body.empty()) {
        if (error_out) {
            *error_out = extract_api_error("Artist search failed", http_code, body);
        }
        return {};
    }

    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("artists") || !j["artists"].contains("items")) {
        if (error_out) *error_out = "Unexpected artist search response format.";
        return {};
    }

    std::vector<SpotifyArtist> results;
    for (const auto& item : j["artists"]["items"]) {
        SpotifyArtist artist;
        artist.id = item.value("id", "");
        artist.name = item.value("name", "");
        if (item.contains("genres") && item["genres"].is_array()) {
            for (size_t i = 0; i < std::min<size_t>(item["genres"].size(), 3); ++i) {
                if (i > 0) artist.genres += ", ";
                artist.genres += item["genres"][i].get<std::string>();
            }
        }
        if (!artist.id.empty()) {
            results.push_back(std::move(artist));
        }
    }
    return results;
}

std::vector<SpotifyAlbum> SpotifyApiClient::fetch_artist_albums(const std::string& artist_id,
                                                                std::string* error_out) {
    if (artist_id.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        if (!ensure_access_token(error_out)) {
            return {};
        }
        token = access_token_;
    }

    std::vector<SpotifyAlbum> albums;
    int offset = 0;
    constexpr int kPageLimit = 10;
    constexpr int kMaxPages = 5;

    for (int page = 0; page < kMaxPages; ++page) {
        std::string url = "https://api.spotify.com/v1/artists/" + url_escape(artist_id) +
                          "/albums?include_groups=album,single&limit=" +
                          std::to_string(kPageLimit) + "&offset=" + std::to_string(offset);

        long http_code = 0;
        std::string body = http_get(url, token, &http_code);
        if (http_code != 200 || body.empty()) {
            if (error_out && albums.empty()) {
                *error_out = extract_api_error("Failed to fetch artist albums", http_code, body);
            }
            break;
        }

        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("items") || !j["items"].is_array()) break;

        for (const auto& item : j["items"]) {
            SpotifyAlbum alb;
            alb.id = item.value("id", "");
            alb.name = item.value("name", "");
            alb.release_date = item.value("release_date", "");
            alb.album_type = item.value("album_type", "");
            alb.total_tracks = item.value("total_tracks", 0);
            alb.artist_name = join_artists(item);
            if (item.contains("images") && item["images"].is_array() && !item["images"].empty()) {
                alb.image_url = item["images"][0].value("url", "");
            }
            if (!alb.id.empty()) {
                albums.push_back(std::move(alb));
            }
        }

        if (!j.contains("next") || j["next"].is_null()) break;
        offset += kPageLimit;
    }

    std::sort(albums.begin(), albums.end(), [](const SpotifyAlbum& a, const SpotifyAlbum& b) {
        if (a.release_date != b.release_date) return a.release_date < b.release_date;
        return a.name < b.name;
    });

    return albums;
}

std::vector<TrackMetadata> SpotifyApiClient::fetch_album_tracks(const SpotifyAlbum& album,
                                                                std::string* error_out) {
    if (album.id.empty()) return {};

    std::string token;
    {
        std::lock_guard lock(mutex_);
        if (!ensure_access_token(error_out)) {
            return {};
        }
        token = access_token_;
    }

    std::vector<TrackMetadata> tracks;
    int offset = 0;
    constexpr int kPageLimit = 10;
    constexpr int kMaxPages = 10;

    for (int page = 0; page < kMaxPages; ++page) {
        std::string url = "https://api.spotify.com/v1/albums/" + url_escape(album.id) +
                          "/tracks?limit=" + std::to_string(kPageLimit) +
                          "&offset=" + std::to_string(offset);

        long http_code = 0;
        std::string body = http_get(url, token, &http_code);
        if (http_code != 200 || body.empty()) {
            if (error_out && tracks.empty()) {
                *error_out = extract_api_error("Failed to fetch album tracks", http_code, body);
            }
            break;
        }

        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("items") || !j["items"].is_array()) break;

        for (const auto& item : j["items"]) {
            TrackMetadata meta;
            meta.location = item.value("uri", "");
            meta.title = item.value("name", "");
            meta.duration_ms = item.value("duration_ms", int64_t{0});
            meta.track_num = item.value("track_number", 0);
            meta.artist = join_artists(item);
            if (meta.artist.empty()) meta.artist = album.artist_name;
            meta.album = album.name;
            meta.image_url = album.image_url;

            if (!meta.location.empty()) {
                MetadataCache::instance().put(meta);
                tracks.push_back(std::move(meta));
            }
        }

        if (!j.contains("next") || j["next"].is_null()) break;
        offset += kPageLimit;
    }

    return tracks;
}

std::optional<TrackMetadata> SpotifyApiClient::fetch_track(const std::string& track_id,
                                                           std::string* error_out) {
    std::string token;
    {
        std::lock_guard lock(mutex_);
        if (!ensure_access_token(error_out)) {
            return std::nullopt;
        }
        token = access_token_;
    }

    std::string url = "https://api.spotify.com/v1/tracks/" + url_escape(track_id);
    long http_code = 0;
    std::string body = http_get(url, token, &http_code);
    if (http_code != 200 || body.empty()) {
        if (error_out) {
            *error_out = "Spotify track fetch failed (HTTP " + std::to_string(http_code) + ").";
        }
        return std::nullopt;
    }

    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("uri")) {
        return std::nullopt;
    }

    TrackMetadata meta = parse_track_json(j);
    MetadataCache::instance().put(meta);
    return meta;
}

std::vector<char> SpotifyApiClient::fetch_bytes(const std::string& url) {
    if (url.empty()) return {};
#ifdef _WIN32
    long code = 0;
    std::vector<char> data;
    if (!winhttp_request(L"GET", url, {}, {}, &code, &data) || code != 200) {
        return {};
    }
    return data;
#else
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::vector<char> data;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code != 200) return {};
    return data;
#endif
}

} // namespace spotify_bridge
