#pragma once

#include "core/bridge_export.hpp"
#include "core/simple_ws_client.hpp"
#include "core/soloist_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace spotify_bridge {

/// Thread-safe, pure C++17 WebSocket client for communicating with a local Spotify Soloist daemon.
class SPOTIFY_BRIDGE_API SoloistClient {
public:
    static SoloistClient& instance();

    using ConnectionCallback = std::function<void(bool connected)>;
    using StateCallback = std::function<void(const SoloistPlaybackState& state)>;

    void set_callbacks(ConnectionCallback on_conn, StateCallback on_state);

    void configure(const std::string& ws_url);
    void configure_launch(int ws_port, const std::string& exe_path,
                          const std::string& api_key, const std::string& device_name,
                          bool auto_spawn);
    std::string ws_url() const;
    bool is_connected() const;
    bool has_fatal_error() const;
    std::string last_error() const;

    void connect_to_server();
    void disconnect_from_server();
    void shutdown_spawned_process();

    // Thread-safe playback commands
    void play_uri(const std::string& spotify_uri);
    void resume();
    void pause();
    void seek(int64_t position_ms);
    void set_volume(int volume_percent);

    SoloistPlaybackState state() const;

private:
    SoloistClient();
    ~SoloistClient();

    void send_json(const std::string& json_payload);
    void try_spawn_soloist();
    void do_reconnect();
    void do_disconnect();
    void on_connected();
    void on_disconnected();
    void on_socket_error(bool connection_refused, const std::string& message);
    void on_text_message(const std::string& msg);

    mutable std::mutex mutex_;
    static constexpr int kDefaultWsPort = 9099;
    std::string ws_url_ = "ws://127.0.0.1:9099";
    std::string ws_host_ = "127.0.0.1";
    int ws_port_ = kDefaultWsPort;
    std::string exe_path_;
    std::string api_key_;
    std::string device_name_;
    bool auto_spawn_ = true;
    std::chrono::steady_clock::time_point last_spawn_attempt_;

    SoloistPlaybackState state_;
    bool connected_ = false;
    bool fatal_error_ = false;
    std::string last_error_;

    SimpleWsClient socket_;
    int64_t spawned_pid_ = 0;
    std::vector<std::string> pending_commands_;

    ConnectionCallback on_connection_changed_;
    StateCallback on_state_updated_;
};

} // namespace spotify_bridge
