#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace spotify_bridge {

/// Minimal cross-platform RFC 6455 WebSocket client for local ws://127.0.0.1:<port> connections.
class SimpleWsClient {
public:
    using OnConnected = std::function<void()>;
    using OnDisconnected = std::function<void()>;
    using OnError = std::function<void(bool connection_refused, const std::string& message)>;
    using OnTextMessage = std::function<void(const std::string& text)>;

    SimpleWsClient();
    ~SimpleWsClient();

    void set_callbacks(OnConnected on_connected,
                       OnDisconnected on_disconnected,
                       OnError on_error,
                       OnTextMessage on_text);

    void connect_async(const std::string& host, int port);
    void close();
    bool is_connected() const;
    void send_text(const std::string& text);

private:
    void run_loop(std::string host, int port, uint64_t generation);

    mutable std::mutex mutex_;
    std::queue<std::string> outbox_;
    std::string active_host_;
    int active_port_ = 0;
    std::atomic<bool> active_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> generation_{0};
    std::thread worker_;

    OnConnected on_connected_;
    OnDisconnected on_disconnected_;
    OnError on_error_;
    OnTextMessage on_text_;
};

} // namespace spotify_bridge
