#include "core/xspf_codec.hpp"

#include <pugixml.hpp>
#include <sstream>

namespace spotify_bridge {

namespace {

std::string child_text(const pugi::xml_node& parent, const char* name) {
    auto node = parent.child(name);
    return node ? std::string(node.text().as_string()) : std::string();
}

void append_text_child(pugi::xml_node& parent, const char* name, const std::string& value) {
    if (value.empty()) return;
    parent.append_child(name).text().set(value.c_str());
}

} // namespace

std::optional<XspfPlaylist> parse_xspf(const std::string& xml_content) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(xml_content.data(), xml_content.size());
    if (!result) {
        return std::nullopt;
    }

    auto root = doc.child("playlist");
    if (!root) {
        return std::nullopt;
    }

    XspfPlaylist playlist;
    playlist.title = child_text(root, "title");

    auto track_list = root.child("trackList");
    if (!track_list) {
        return playlist;
    }

    for (auto track_node : track_list.children("track")) {
        TrackMetadata meta;
        meta.location = percent_decode_uri(child_text(track_node, "location"));
        meta.title = child_text(track_node, "title");
        meta.artist = child_text(track_node, "creator");
        meta.album = child_text(track_node, "album");

        if (auto dur = track_node.child("duration")) {
            meta.duration_ms = dur.text().as_llong(0);
        }
        if (auto num = track_node.child("trackNum")) {
            meta.track_num = num.text().as_int(0);
        }

        std::string raw_image = child_text(track_node, "image");
        if (!raw_image.empty()) {
            meta.image_url = percent_decode_uri(raw_image);
        }

        if (!meta.location.empty()) {
            playlist.tracks.push_back(std::move(meta));
        }
    }

    return playlist;
}

std::string serialize_xspf(const XspfPlaylist& playlist) {
    pugi::xml_document doc;

    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto root = doc.append_child("playlist");
    root.append_attribute("version") = "1";
    root.append_attribute("xmlns") = "http://xspf.org/ns/0/";

    if (!playlist.title.empty()) {
        append_text_child(root, "title", playlist.title);
    }

    auto track_list = root.append_child("trackList");
    for (const auto& track : playlist.tracks) {
        auto t = track_list.append_child("track");

        // Encode colons/special chars in <location> and <image> to match Strawberry/Audacious XSPF
        std::string canon_loc = to_canonical_spotify_uri(track.location);
        append_text_child(t, "location", percent_encode_xspf_uri(canon_loc));
        append_text_child(t, "title", track.title);
        append_text_child(t, "creator", track.artist);
        append_text_child(t, "album", track.album);

        if (track.duration_ms > 0) {
            t.append_child("duration").text().set(static_cast<long long>(track.duration_ms));
        }
        if (track.track_num > 0) {
            t.append_child("trackNum").text().set(track.track_num);
        }
        if (!track.image_url.empty()) {
            append_text_child(t, "image", percent_encode_xspf_uri(track.image_url));
        }
    }

    std::ostringstream oss;
    doc.save(oss, "  ");
    return oss.str();
}

} // namespace spotify_bridge
