#include "core/soloist_client.hpp"
#include "core/track_metadata.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace spotify_bridge {

namespace {

std::string default_device_name() {
    char buf[256]{};
#ifdef _WIN32
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len) && len > 0) {
        return std::string("DeaDBeeF-") + buf;
    }
#else
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        return std::string("DeaDBeeF-") + buf;
    }
#endif
    return "DeaDBeeF";
}

void parse_ws_url(const std::string& url, std::string& host_out, int& port_out) {
    std::string s = url;
    const std::string prefix = "ws://";
    if (s.rfind(prefix, 0) == 0) {
        s = s.substr(prefix.size());
    }
    auto slash = s.find('/');
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        std::string h = s.substr(0, colon);
        int p = std::atoi(s.substr(colon + 1).c_str());
        if (!h.empty()) host_out = h;
        if (p > 0 && p <= 65535) port_out = p;
    } else if (!s.empty()) {
        host_out = s;
    }
}

std::string find_soloist_in_path() {
#ifdef _WIN32
    char path_buf[MAX_PATH]{};
    DWORD n = SearchPathA(nullptr, "soloist.exe", nullptr, MAX_PATH, path_buf, nullptr);
    if (n > 0 && n < MAX_PATH) {
        return std::string(path_buf);
    }
    return {};
#else
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::istringstream iss(path_env);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        if (dir.empty()) continue;
        std::string candidate = dir + "/soloist";
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return {};
#endif
}

#ifdef _WIN32
std::string quote_win_arg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}
#endif

} // namespace

SoloistClient& SoloistClient::instance() {
    static SoloistClient client;
    return client;
}

SoloistClient::SoloistClient() {
    socket_.set_callbacks(
        [this]() { on_connected(); },
        [this]() { on_disconnected(); },
        [this](bool refused, const std::string& msg) { on_socket_error(refused, msg); },
        [this](const std::string& text) { on_text_message(text); }
    );
}

SoloistClient::~SoloistClient() {
    socket_.close();
}

void SoloistClient::set_callbacks(ConnectionCallback on_conn, StateCallback on_state) {
    std::lock_guard lock(mutex_);
    on_connection_changed_ = std::move(on_conn);
    on_state_updated_ = std::move(on_state);
}

void SoloistClient::configure_launch(int ws_port, const std::string& exe_path,
                                     const std::string& api_key,
                                     const std::string& device_name, bool auto_spawn) {
    int clamped_port = (ws_port > 0 && ws_port <= 65535) ? ws_port : 9090;
    std::string new_url = "ws://127.0.0.1:" + std::to_string(clamped_port);
    {
        std::lock_guard lock(mutex_);
        ws_port_ = clamped_port;
        exe_path_ = exe_path;
        api_key_ = api_key;
        device_name_ = device_name.empty() ? default_device_name() : device_name;
        auto_spawn_ = auto_spawn;
    }
    configure(new_url);
}

void SoloistClient::configure(const std::string& ws_url) {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        if (ws_url_ != ws_url && !ws_url.empty()) {
            ws_url_ = ws_url;
            parse_ws_url(ws_url, ws_host_, ws_port_);
            changed = true;
        }
    }
    if (changed) {
        do_reconnect();
    }
}

void SoloistClient::do_reconnect() {
    socket_.close();
    connect_to_server();
}

void SoloistClient::do_disconnect() {
    socket_.close();
}

std::string SoloistClient::ws_url() const {
    std::lock_guard lock(mutex_);
    return ws_url_;
}

bool SoloistClient::is_connected() const {
    std::lock_guard lock(mutex_);
    return connected_;
}

bool SoloistClient::has_fatal_error() const {
    std::lock_guard lock(mutex_);
    return fatal_error_;
}

std::string SoloistClient::last_error() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

void SoloistClient::connect_to_server() {
    std::string host;
    int port = 9090;
    {
        std::lock_guard lock(mutex_);
        if (connected_) return;
        fatal_error_ = false;
        last_error_.clear();
        host = ws_host_;
        port = ws_port_;
    }
    socket_.connect_async(host, port);
}

void SoloistClient::disconnect_from_server() {
    do_disconnect();
}

void SoloistClient::shutdown_spawned_process() {
    int64_t pid = 0;
    {
        std::lock_guard lock(mutex_);
        pid = spawned_pid_;
        spawned_pid_ = 0;
    }
    if (pid > 0) {
#ifdef _WIN32
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
#else
        ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
    }
}

void SoloistClient::play_uri(const std::string& spotify_uri) {
    send_json(build_play_command(to_canonical_spotify_uri(spotify_uri)));
}

void SoloistClient::resume() {
    send_json(build_play_command());
}

void SoloistClient::pause() {
    send_json(build_pause_command());
}

void SoloistClient::seek(int64_t position_ms) {
    send_json(build_seek_command(position_ms));
}

void SoloistClient::set_volume(int volume_percent) {
    send_json(build_set_volume_command(volume_percent));
}

SoloistPlaybackState SoloistClient::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void SoloistClient::send_json(const std::string& json_payload) {
    bool is_conn = socket_.is_connected();
    bool ready = false;
    {
        std::lock_guard lock(mutex_);
        ready = is_conn && state_.logged_in;
        if (!ready) {
            pending_commands_.push_back(json_payload);
        }
    }
    if (ready) {
        socket_.send_text(json_payload);
    } else if (!is_conn) {
        connect_to_server();
    }
}

void SoloistClient::try_spawn_soloist() {
    std::string exe_copy;
    std::string key_copy;
    std::string dev_copy;
    int port_copy = 9090;
    {
        std::lock_guard lock(mutex_);
        if (!auto_spawn_) {
            fatal_error_ = true;
            last_error_ = "Soloist is not running and auto-start is disabled.";
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (last_spawn_attempt_.time_since_epoch().count() != 0 &&
            (now - last_spawn_attempt_) < std::chrono::seconds(10)) {
            return;
        }
        last_spawn_attempt_ = now;
        exe_copy = exe_path_;
        key_copy = api_key_;
        dev_copy = device_name_.empty() ? default_device_name() : device_name_;
        port_copy = ws_port_;

        if (spawned_pid_ > 0) {
#ifdef _WIN32
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(spawned_pid_));
            if (h) {
                DWORD wait_rc = WaitForSingleObject(h, 0);
                CloseHandle(h);
                if (wait_rc == WAIT_TIMEOUT) return;
            }
#else
            int status = 0;
            if (::waitpid(static_cast<pid_t>(spawned_pid_), &status, WNOHANG) == 0) {
                return;
            }
#endif
            spawned_pid_ = 0;
        }
    }

    std::string program = exe_copy.empty() ? find_soloist_in_path() : exe_copy;
    if (program.empty()) {
        std::fprintf(stderr, "[deadbeef-spotify] ERROR: Soloist executable not found.\n");
        std::lock_guard lock(mutex_);
        fatal_error_ = true;
        last_error_ = "Soloist executable not found in PATH or settings.";
        return;
    }

    std::string ws_addr = "127.0.0.1:" + std::to_string(port_copy);
    std::fprintf(stderr, "[deadbeef-spotify] Spawning: '%s' -n '%s' -w %s (api_key=%s)\n",
                 program.c_str(), dev_copy.c_str(), ws_addr.c_str(),
                 key_copy.empty() ? "EMPTY" : "SET");

#ifdef _WIN32
    if (!key_copy.empty()) {
        SetEnvironmentVariableA("SPOTIFY_SOLOIST_API_KEY", key_copy.c_str());
    }
    std::string cmd = quote_win_arg(program) + " -n " + quote_win_arg(dev_copy) +
                      " -w " + ws_addr;
    if (!key_copy.empty()) {
        cmd += " -k " + quote_win_arg(key_copy);
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');
    if (CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::lock_guard lock(mutex_);
        spawned_pid_ = static_cast<int64_t>(pi.dwProcessId);
    } else {
        std::lock_guard lock(mutex_);
        fatal_error_ = true;
        last_error_ = "Failed to launch Soloist process.";
    }
#else
    pid_t pid = ::fork();
    if (pid == 0) {
        if (!key_copy.empty()) {
            ::setenv("SPOTIFY_SOLOIST_API_KEY", key_copy.c_str(), 1);
        }
        std::vector<const char*> argv;
        argv.push_back(program.c_str());
        argv.push_back("-n");
        argv.push_back(dev_copy.c_str());
        argv.push_back("-w");
        argv.push_back(ws_addr.c_str());
        if (!key_copy.empty()) {
            argv.push_back("-k");
            argv.push_back(key_copy.c_str());
        }
        argv.push_back(nullptr);
        ::execvp(program.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    } else if (pid > 0) {
        std::lock_guard lock(mutex_);
        spawned_pid_ = static_cast<int64_t>(pid);
    } else {
        std::lock_guard lock(mutex_);
        fatal_error_ = true;
        last_error_ = "Failed to fork Soloist process.";
    }
#endif
}

void SoloistClient::on_connected() {
    std::vector<std::string> to_send;
    ConnectionCallback cb;
    {
        std::lock_guard lock(mutex_);
        connected_ = true;
        if (state_.logged_in) {
            to_send.swap(pending_commands_);
        }
        cb = on_connection_changed_;
    }
    for (const auto& cmd : to_send) {
        socket_.send_text(cmd);
    }
    if (cb) cb(true);
}

void SoloistClient::on_disconnected() {
    bool was_connected = false;
    ConnectionCallback cb;
    {
        std::lock_guard lock(mutex_);
        was_connected = connected_;
        connected_ = false;
        state_.logged_in = false;
        cb = on_connection_changed_;
    }
    if (was_connected && cb) {
        cb(false);
    }
}

void SoloistClient::on_socket_error(bool connection_refused, const std::string& message) {
    std::fprintf(stderr, "[deadbeef-spotify] WebSocket error: %s\n", message.c_str());
    if (connection_refused) {
        try_spawn_soloist();
    }
}

void SoloistClient::on_text_message(const std::string& msg) {
    SoloistPlaybackState snapshot;
    std::vector<std::string> to_send;
    StateCallback cb;
    {
        std::lock_guard lock(mutex_);
        SoloistEventType ev = parse_soloist_event(msg, state_);
        if (ev == SoloistEventType::PlaybackState ||
            ev == SoloistEventType::PlaybackChanged ||
            ev == SoloistEventType::TrackChanged) {
            state_.logged_in = true;
        }
        if (state_.logged_in && !pending_commands_.empty()) {
            to_send.swap(pending_commands_);
        }
        snapshot = state_;
        cb = on_state_updated_;
    }
    for (const auto& cmd : to_send) {
        socket_.send_text(cmd);
    }
    if (cb) cb(snapshot);
}

} // namespace spotify_bridge
