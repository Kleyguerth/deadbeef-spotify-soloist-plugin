#include "core/simple_ws_client.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
  inline void close_socket(SocketHandle s) { closesocket(s); }
  inline int poll_socket(pollfd* fds, unsigned long nfds, int timeout_ms) {
      return WSAPoll(fds, nfds, timeout_ms);
  }
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <netdb.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketHandle = int;
  constexpr SocketHandle kInvalidSocket = -1;
  inline void close_socket(SocketHandle s) { ::close(s); }
  inline int poll_socket(pollfd* fds, nfds_t nfds, int timeout_ms) {
      return ::poll(fds, nfds, timeout_ms);
  }
#endif

namespace spotify_bridge {

namespace {

#ifdef _WIN32
struct WsaInit {
    WsaInit() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit g_wsa_init;
#endif

std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(b >> 18) & 0x3F]);
        out.push_back(kTable[(b >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTable[(b >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTable[b & 0x3F] : '=');
    }
    return out;
}

bool send_all(SocketHandle sock, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(sock, reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact(SocketHandle sock, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = ::recv(sock, reinterpret_cast<char*>(buf + got),
                       static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool send_ws_frame(SocketHandle sock, uint8_t opcode, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(14 + payload.size());
    frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F))); // FIN + opcode

    uint64_t len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(0x80 | len)); // MASK bit set
    } else if (len <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    std::random_device rd;
    std::array<uint8_t, 4> mask = {
        static_cast<uint8_t>(rd()), static_cast<uint8_t>(rd()),
        static_cast<uint8_t>(rd()), static_cast<uint8_t>(rd())
    };
    frame.insert(frame.end(), mask.begin(), mask.end());

    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
    }
    return send_all(sock, frame.data(), frame.size());
}

} // namespace

SimpleWsClient::SimpleWsClient() = default;

SimpleWsClient::~SimpleWsClient() {
    close();
}

void SimpleWsClient::set_callbacks(OnConnected on_connected,
                                   OnDisconnected on_disconnected,
                                   OnError on_error,
                                   OnTextMessage on_text) {
    std::lock_guard lock(mutex_);
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
    on_error_ = std::move(on_error);
    on_text_ = std::move(on_text);
}

void SimpleWsClient::connect_async(const std::string& host, int port) {
    {
        std::lock_guard lock(mutex_);
        if (active_.load() && active_host_ == host && active_port_ == port) {
            return;
        }
    }
    close();
    {
        std::lock_guard lock(mutex_);
        active_host_ = host;
        active_port_ = port;
    }
    stop_requested_ = false;
    active_ = true;
    uint64_t gen = ++generation_;
    std::fprintf(stderr, "[spotify-soloist] Connecting WebSocket to %s:%d...\n", host.c_str(), port);
    worker_ = std::thread(&SimpleWsClient::run_loop, this, host, port, gen);
}

void SimpleWsClient::close() {
    stop_requested_ = true;
    ++generation_;
    if (worker_.joinable()) {
        worker_.join();
    }
    connected_ = false;
    std::lock_guard lock(mutex_);
    std::queue<std::string> empty;
    outbox_.swap(empty);
}

bool SimpleWsClient::is_connected() const {
    return connected_;
}

void SimpleWsClient::send_text(const std::string& text) {
    std::lock_guard lock(mutex_);
    outbox_.push(text);
}

void SimpleWsClient::run_loop(std::string host, int port, uint64_t generation) {
    struct ActiveGuard {
        std::atomic<bool>& flag;
        ~ActiveGuard() { flag = false; }
    } active_guard{active_};

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        if (on_error_) on_error_(false, "Failed to resolve host " + host);
        return;
    }

    SocketHandle sock = kInvalidSocket;
    bool conn_refused = false;
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == kInvalidSocket) continue;
        if (::connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            break;
        }
#ifdef _WIN32
        conn_refused = (WSAGetLastError() == WSAECONNREFUSED);
#else
        conn_refused = (errno == ECONNREFUSED);
#endif
        close_socket(sock);
        sock = kInvalidSocket;
    }
    freeaddrinfo(res);

    if (sock == kInvalidSocket) {
        if (on_error_) {
            on_error_(conn_refused, conn_refused ? "Connection refused" : "Failed to connect");
        }
        return;
    }

    std::fprintf(stderr, "[spotify-soloist] TCP connected, sending WS handshake...\n");

    // Perform HTTP/1.1 WebSocket Upgrade handshake
    std::random_device rd;
    std::array<uint8_t, 16> nonce{};
    for (auto& b : nonce) b = static_cast<uint8_t>(rd());
    std::string key = base64_encode(nonce.data(), nonce.size());

    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: " + host + ":" + port_str + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    if (!send_all(sock, reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
        close_socket(sock);
        if (on_error_) on_error_(false, "Handshake send failed");
        return;
    }

    std::string resp;
    char ch = 0;
    while (resp.size() < 8192) {
        int n = ::recv(sock, &ch, 1, 0);
        if (n <= 0) break;
        resp.push_back(ch);
        if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }

    if (resp.find("101") == std::string::npos) {
        std::fprintf(stderr, "[spotify-soloist] WS handshake rejected: '%s'\n", resp.c_str());
        close_socket(sock);
        if (on_error_) on_error_(false, "WebSocket handshake rejected");
        return;
    }

    std::fprintf(stderr, "[spotify-soloist] WebSocket connected!\n");
    connected_ = true;
    if (on_connected_) on_connected_();

    std::string fragmented_msg;
    while (!stop_requested_ && generation == generation_.load()) {
        // Flush outbox
        for (;;) {
            std::string msg;
            {
                std::lock_guard lock(mutex_);
                if (outbox_.empty()) break;
                msg = std::move(outbox_.front());
                outbox_.pop();
            }
            std::fprintf(stderr, "[spotify-soloist] WS TX: %s\n", msg.c_str());
            if (!send_ws_frame(sock, 0x01, msg)) {
                stop_requested_ = true;
                break;
            }
        }
        if (stop_requested_) break;

        pollfd pfd{};
        pfd.fd = sock;
        pfd.events = POLLIN;
        int pr = poll_socket(&pfd, 1, 50);
        if (pr < 0) break;
        if (pr == 0) continue;

        uint8_t hdr[2]{};
        if (!recv_exact(sock, hdr, 2)) break;

        bool fin = (hdr[0] & 0x80) != 0;
        uint8_t opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t payload_len = hdr[1] & 0x7F;

        if (payload_len == 126) {
            uint8_t ext[2]{};
            if (!recv_exact(sock, ext, 2)) break;
            payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8]{};
            if (!recv_exact(sock, ext, 8)) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | ext[i];
            }
        }

        uint8_t mask_key[4]{};
        if (masked && !recv_exact(sock, mask_key, 4)) break;

        if (payload_len > 16 * 1024 * 1024) break; // Sanity cap
        std::string payload(static_cast<size_t>(payload_len), '\0');
        if (payload_len > 0 &&
            !recv_exact(sock, reinterpret_cast<uint8_t*>(payload.data()),
                        static_cast<size_t>(payload_len))) {
            break;
        }

        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask_key[i % 4]);
            }
        }

        if (opcode == 0x08) { // Close
            break;
        } else if (opcode == 0x09) { // Ping -> Pong
            send_ws_frame(sock, 0x0A, payload);
        } else if (opcode == 0x01 || opcode == 0x00) { // Text or continuation
            fragmented_msg += payload;
            if (fin) {
                if (on_text_) on_text_(fragmented_msg);
                fragmented_msg.clear();
            }
        }
    }

    std::fprintf(stderr, "[spotify-soloist] WebSocket disconnected.\n");
    close_socket(sock);
    connected_ = false;
    if (on_disconnected_) on_disconnected_();
}

} // namespace spotify_bridge
