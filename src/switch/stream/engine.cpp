#include "engine.hpp"

#include "../../core/http.hpp"
#include "json.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <mbedtls/aes.h>
#include <peer.h>
}

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <netdb.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

extern "C" void gnx_peer_log_set(void (*cb)(const char* line));

namespace {

gnx::stream::Engine* g_active_engine = nullptr;
bool g_peer_initialized = false;

void av_log_capture(void*, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING || !g_active_engine) return;
    char message[320]{};
    std::vsnprintf(message, sizeof(message), format, args);
    std::size_t length = std::strlen(message);
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
        message[--length] = '\0';
    }
    if (length) g_active_engine->log(std::string("ffmpeg| ") + message);
}

std::string uuid_v4() {
    std::array<unsigned char, 16> bytes{};
    randomGet(bytes.data(), bytes.size());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    char output[37]{};
    std::snprintf(output, sizeof(output),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return output;
}

std::string without_port(const std::string& host) {
    const auto colon = host.find(':');
    return colon == std::string::npos ? host : host.substr(0, colon);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool sequence_newer16(uint16_t value, uint16_t reference) {
    const uint16_t distance = static_cast<uint16_t>(value - reference);
    return distance != 0 && distance < 0x8000U;
}

bool annexb_video_info(const uint8_t* data, size_t size, bool* hasIdr) {
    if (hasIdr) *hasIdr = false;
    bool hasSlice = false;
    for (size_t at = 0; at + 4 < size; ++at) {
        size_t nal = 0;
        if (data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1) {
            nal = at + 3;
        } else if (data[at] == 0 && data[at + 1] == 0 &&
                   data[at + 2] == 0 && data[at + 3] == 1) {
            nal = at + 4;
        } else {
            continue;
        }
        if (nal >= size) continue;
        const uint8_t type = data[nal] & 0x1fU;
        if (type == 1 || type == 5) hasSlice = true;
        if (type == 5 && hasIdr) *hasIdr = true;
    }
    return hasSlice;
}

std::string query_session_id(const std::string& query) {
    const std::string lowered = lower_ascii(query);
    for (const char* name : {"sessionid=", "session="}) {
        const auto at = lowered.find(name);
        if (at == std::string::npos) continue;
        const auto start = at + std::strlen(name);
        const auto end = query.find('&', start);
        return query.substr(start, end == std::string::npos ? std::string::npos
                                                            : end - start);
    }
    return {};
}

std::string find_nested_string(const nlohmann::json& value,
                               std::initializer_list<const char*> keys,
                               int depth = 0) {
    if (depth > 10) return {};
    if (value.is_object()) {
        for (const char* key : keys) {
            const auto item = value.find(key);
            if (item != value.end() && item->is_string()) {
                return item->get<std::string>();
            }
        }
        for (auto item = value.begin(); item != value.end(); ++item) {
            const std::string found = find_nested_string(item.value(), keys, depth + 1);
            if (!found.empty()) return found;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            const std::string found = find_nested_string(item, keys, depth + 1);
            if (!found.empty()) return found;
        }
    }
    return {};
}

void collect_candidate_strings(const nlohmann::json& value,
                               std::vector<std::string>& output,
                               const std::string& key = {}, int depth = 0) {
    if (depth > 10) return;
    if (value.is_string() && lower_ascii(key) == "candidate") {
        std::string candidate = value.get<std::string>();
        if (candidate.rfind("a=", 0) == 0) candidate.erase(0, 2);
        if (candidate.rfind("candidate:", 0) == 0 &&
            std::find(output.begin(), output.end(), candidate) == output.end()) {
            output.push_back(std::move(candidate));
        }
        return;
    }
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            collect_candidate_strings(item.value(), output, item.key(), depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            collect_candidate_strings(item, output, key, depth + 1);
        }
    }
}

struct IceEntry {
    std::string url;
    std::string username;
    std::string credential;
};

void collect_ice_entries(const nlohmann::json& value,
                         std::vector<IceEntry>& entries, int depth = 0) {
    if (depth > 8) return;
    if (value.is_object()) {
        const auto urls = value.find("urls");
        if (urls != value.end()) {
            std::vector<std::string> values;
            if (urls->is_string()) values.push_back(urls->get<std::string>());
            if (urls->is_array()) {
                for (const auto& url : *urls) {
                    if (url.is_string()) values.push_back(url.get<std::string>());
                }
            }
            const std::string username = find_nested_string(value, {"username"});
            const std::string credential = find_nested_string(value, {"credential"});
            for (const std::string& url : values) {
                if (!url.empty() && entries.size() < 5) {
                    entries.push_back({url, username, credential});
                }
            }
            if (!values.empty()) return;
        }
        for (auto item = value.begin(); item != value.end(); ++item) {
            collect_ice_entries(item.value(), entries, depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) collect_ice_entries(item, entries, depth + 1);
    }
}

std::vector<std::string> candidates_from_sdp(const std::string& sdp) {
    std::vector<std::string> candidates;
    std::size_t at = 0;
    while ((at = sdp.find("a=candidate:", at)) != std::string::npos) {
        const std::size_t end = sdp.find_first_of("\r\n", at);
        candidates.push_back(sdp.substr(at + 2, end - at - 2));
        at = end == std::string::npos ? sdp.size() : end;
    }
    return candidates;
}

constexpr float kStickDeadzone = 3200.0f;
constexpr float kStickOuterRange = 30000.0f;
constexpr int kAxisChangeThreshold = 700;
constexpr uint64_t kAxisRefreshMs = 120;

std::pair<int, int> controller_stick(HidAnalogStickState stick) {
    // Apply a radial dead zone, preserve the original direction and expand the
    // useful outer range. Joy-Con sticks often stop short of 32767; mapping
    // ~30000 to full scale prevents games from interpreting a fully-held stick
    // as walking.
    const float x = static_cast<float>(stick.x);
    const float y = static_cast<float>(-stick.y);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= kStickDeadzone) return {0, 0};

    const float normalized = std::clamp(
        (magnitude - kStickDeadzone) /
            (kStickOuterRange - kStickDeadzone),
        0.0f, 1.0f);
    const float scale = normalized * 32767.0f / magnitude;
    return {
        std::clamp(static_cast<int>(std::lround(x * scale)), -32767, 32767),
        std::clamp(static_cast<int>(std::lround(y * scale)), -32767, 32767),
    };
}

bool axis_should_send(int before, int after, uint64_t lastSent, uint64_t now) {
    if (std::abs(before - after) >= kAxisChangeThreshold) return true;
    if ((after == 0) != (before == 0)) return true;
    if (std::abs(after) >= 32000 && before != after) return true;
    return after != 0 && now - lastSent >= kAxisRefreshMs;
}

uint16_t read_le16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) |
           (static_cast<uint16_t>(value[1]) << 8);
}

std::array<uint8_t, 512> g_gf_exp{};
std::array<uint8_t, 256> g_gf_log{};
std::once_flag g_gf_once;

void init_gf256() {
    uint16_t value = 1;
    for (int exponent = 0; exponent < 255; ++exponent) {
        g_gf_exp[exponent] = static_cast<uint8_t>(value);
        g_gf_log[value] = static_cast<uint8_t>(exponent);
        value <<= 1;
        if (value & 0x100U) value ^= 0x11dU;
    }
    for (int exponent = 255; exponent < 512; ++exponent) {
        g_gf_exp[exponent] = g_gf_exp[exponent - 255];
    }
}

uint8_t gf_multiply(uint8_t left, uint8_t right) {
    if (left == 0 || right == 0) return 0;
    return g_gf_exp[static_cast<unsigned int>(g_gf_log[left]) +
                    static_cast<unsigned int>(g_gf_log[right])];
}

uint8_t gf_inverse(uint8_t value) {
    if (value == 0) return 0;
    return g_gf_exp[255U - g_gf_log[value]];
}

bool invert_gf_matrix(std::vector<uint8_t>& matrix, std::size_t order,
                      std::vector<uint8_t>& inverse) {
    if (order == 0 || matrix.size() != order * order) return false;
    inverse.assign(order * order, 0);
    for (std::size_t row = 0; row < order; ++row) {
        inverse[row * order + row] = 1;
    }
    for (std::size_t column = 0; column < order; ++column) {
        std::size_t pivot = column;
        while (pivot < order && matrix[pivot * order + column] == 0) ++pivot;
        if (pivot == order) return false;
        if (pivot != column) {
            for (std::size_t at = 0; at < order; ++at) {
                std::swap(matrix[column * order + at],
                          matrix[pivot * order + at]);
                std::swap(inverse[column * order + at],
                          inverse[pivot * order + at]);
            }
        }
        const uint8_t scale = gf_inverse(matrix[column * order + column]);
        for (std::size_t at = 0; at < order; ++at) {
            matrix[column * order + at] =
                gf_multiply(matrix[column * order + at], scale);
            inverse[column * order + at] =
                gf_multiply(inverse[column * order + at], scale);
        }
        for (std::size_t row = 0; row < order; ++row) {
            if (row == column) continue;
            const uint8_t factor = matrix[row * order + column];
            if (factor == 0) continue;
            for (std::size_t at = 0; at < order; ++at) {
                matrix[row * order + at] ^=
                    gf_multiply(factor, matrix[column * order + at]);
                inverse[row * order + at] ^=
                    gf_multiply(factor, inverse[column * order + at]);
            }
        }
    }
    return true;
}

int json_int(const nlohmann::json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end()) return 0;
    if (value->is_number_integer() || value->is_number_unsigned()) {
        return value->get<int>();
    }
    if (value->is_string()) {
        try {
            return std::stoi(value->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

int open_udp_channel(const std::string& host, int port,
                     std::string& error, int& localPort) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), service.c_str(), &hints,
                                   &addresses);
    if (lookup != 0 || !addresses) {
        error = "No se pudo resolver el servidor UDP nativo.";
        return -1;
    }

    int fd = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        fd = ::socket(address->ai_family, address->ai_socktype,
                      address->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        error = "No se pudo abrir el canal UDP nativo.";
        return -1;
    }

    timeval timeout{};
    timeout.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int receiveBuffer = 512000;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receiveBuffer,
               sizeof(receiveBuffer));
    sockaddr_storage local{};
    socklen_t localSize = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localSize) == 0) {
        if (local.ss_family == AF_INET) {
            localPort = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
        } else if (local.ss_family == AF_INET6) {
            localPort = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
        }
    }
    const char ping[] = "ping";
    for (int attempt = 0; attempt < 10; ++attempt) {
        ::send(fd, ping, sizeof(ping) - 1, 0);
    }
    return fd;
}

}  // namespace

namespace gnx::stream {

Engine::Engine(ZERODROID::BoosteroidAPI& api, SDL_Renderer* renderer)
    : api_(api), renderer_(renderer) {
    if (!g_peer_initialized) {
        peer_init();
        g_peer_initialized = true;
    }
}

Engine::~Engine() { stop(); }

void Engine::global_shutdown() {
    if (!g_peer_initialized) return;
    g_peer_initialized = false;
    peer_deinit();
}

void Engine::start(int appId, int streamWidth, int streamHeight,
                   const std::string& preferredGateway) {
    stop();
    appId_ = appId;
    streamWidth_ = std::clamp(streamWidth, 640, 2560);
    streamHeight_ = std::clamp(streamHeight, 360, 1440);
    preferredGateway_ = preferredGateway;
    sessionId_.clear();
    gatewayHost_.clear();
    gatewayApiBase_.clear();
    peerId_.clear();
    error_.clear();
    quit_ = false;
    gotFrame_ = false;
    gotVideoPacket_ = false;
    gotAudioPacket_ = false;
    gotAccessUnit_ = false;
    presentedFirstFrame_ = false;
    channelAssociationReady_ = false;
    peerState_ = PEER_CONNECTION_NEW;
    controllerId_ = -1;
    inputCommand_ = 0;
    guidePressed_ = false;
    pingMs_ = -1;
    lastMediaTicks_ = 0;
    sessionStartedTicks_ = SDL_GetTicks64();
    mouseMoveCount_ = 0;
    mouseClickCount_ = 0;
    keyboardEventCount_ = 0;
    nativeMediaStarted_ = false;
    nativeStartedTicks_ = 0;
    dataChannelOpened_ = false;
    padInitialized_ = false;
    inputLogged_ = false;
    mouseInputLogged_ = false;
    keyboardInputLogged_ = false;
    previousGuide_ = false;
    previousLeft_ = {};
    previousRight_ = {};
    previousButtons_ = 0;
    const int initialAxes[6] = {0, 0, -32767, 0, 0, -32767};
    for (int axis = 0; axis < 6; ++axis) {
        lastSentAxes_[axis] = initialAxes[axis];
        lastAxisSentTicks_[axis] = 0;
    }
    lastInputDiagnosticTicks_ = 0;
    remoteCandidates_.clear();
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        nativeUdpHost_.clear();
        nativeKeyHex_.clear();
        nativeVideoPort_ = 0;
        nativeAudioPort_ = 0;
        nativeGroups_.clear();
        nativeSequenceStarted_ = false;
        nativeAnyQueued_ = false;
        nativeNextGroup_ = 0;
    }
    nativeWaitingKeyframe_ = true;
    nativeDroppedGroups_ = 0;
    nativeRecoveredGroups_ = 0;
    nativeRecovering_ = false;
    nativeRecoveryStartedTicks_ = 0;
    nativeLastKeyframeRequestTicks_ = 0;
    nativeRecoveryCount_ = 0;
    decoderResyncRequested_ = false;
    decoderFlushOnKeyframe_ = false;
    jitter_.reset();
    nextPresentCounter_ = 0;
    presentedSequence_ = 0;

    std::remove("sdmc:/switch/ZERODROID/stream-prev.log");
    std::rename("sdmc:/switch/ZERODROID/stream.log",
                "sdmc:/switch/ZERODROID/stream-prev.log");
    logFile_ = std::fopen("sdmc:/switch/ZERODROID/stream.log", "w");
    if (logFile_) std::setvbuf(logFile_, nullptr, _IOLBF, 0);
    g_active_engine = this;
    gnx_peer_log_set([](const char* line) {
        if (g_active_engine) g_active_engine->log(std::string("peer| ") + line);
    });
    av_log_set_callback(&av_log_capture);

    video_.init(renderer_);
    audio_.init();
    sharedFrame_ = av_frame_alloc();
    presentFrame_ = av_frame_alloc();
    sharedFrameValid_ = false;

    state_ = EngineState::StartingSession;
    set_status("Solicitando una maquina a Boosteroid...");
    workerThread_ = std::thread(&Engine::worker, this);
    decodeThread_ = std::thread(&Engine::decode_loop, this);
}

void Engine::stop() { shutdown(false); }

void Engine::disconnect_for_reconnect() { shutdown(true); }

void Engine::shutdown(bool preserveRemoteSession) {
    quit_ = true;
    videoCv_.notify_all();
    if (control_.connected()) {
        const int id = controllerId_.load();
        if (id > 0) {
            control_.send_text(nlohmann::json({
                {"type", "controller"}, {"action", "disconnected"},
                {"id", id}}).dump());
        }
        // "terminating", gateway hangup and API dequeue explicitly end the VM.
        // Skip all three during a reconnect so Boosteroid can keep the desktop
        // and game alive while the Switch creates a fresh transport.
        if (!preserveRemoteSession) {
            control_.send_text(nlohmann::json({
                {"type", "settings"}, {"action", "terminating"}}).dump());
        }
    }
    control_.close();
    stop_native_udp();
    if (workerThread_.joinable()) workerThread_.join();
    if (decodeThread_.joinable()) decodeThread_.join();
    destroy_peer();
    dkVideo_.shutdown();
    audio_.shutdown();
    video_.shutdown();
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        videoQueue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (sharedFrame_) av_frame_free(&sharedFrame_);
        if (presentFrame_) av_frame_free(&presentFrame_);
        sharedFrameValid_ = false;
    }
    if (!preserveRemoteSession && !gatewayApiBase_.empty() &&
        !peerId_.empty() && !sessionId_.empty()) {
        try {
            gnx::Http http;
            http.get(gatewayApiBase_ + "/api/hangup?peerid=" +
                     gnx::Http::urlencode(peerId_) + "&sessionId=" +
                     gnx::Http::urlencode(sessionId_));
        } catch (...) {
        }
    }
    if (!preserveRemoteSession && !sessionId_.empty()) {
        api_.stopStreamingSession(sessionId_);
    }
    if (preserveRemoteSession && !sessionId_.empty()) {
        log("local transport closed; preserving remote session " + sessionId_);
    }
    sessionId_.clear();
    if (g_active_engine == this) {
        gnx_peer_log_set(nullptr);
        g_active_engine = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (logFile_) std::fclose(logFile_);
        logFile_ = nullptr;
    }
    if (state_ != EngineState::Failed && state_ != EngineState::Idle) {
        state_ = EngineState::Stopped;
    }
}

void Engine::worker() {
    ZERODROID::StreamSessionConfig config;
    log("launch appId=" + std::to_string(appId_) + " resolution=" +
        std::to_string(streamWidth_) + "x" + std::to_string(streamHeight_));
    if (!api_.startStreamingSession(
            appId_, config, &quit_, [this](const std::string& message) {
                log("api| " + message);
                set_status(message);
            })) {
        if (quit_) return;
        fail(api_.lastError().empty() ? "No se pudo iniciar el juego."
                                     : api_.lastError());
        return;
    }
    if (quit_) return;
    sessionId_ = config.sessionId;
    log("sessionId=" + sessionId_ + " assignedGateway=" +
        (config.assignedGateway.empty() ? "no" : "yes") +
        " signedQueryBytes=" + std::to_string(config.signedQuery.size()) +
        " legacyQueries=" +
        std::to_string(config.sessionQueries.size()) + " gateways=" +
        std::to_string(config.gateways.size()));
    if (!connect_gateway(config)) return;

    bool peerRequested = false;
    bool announcedController = false;
    uint64_t lastControllerAnnouncement = 0;
    uint64_t lastPingRequest = 0;
    uint64_t connectedAt = SDL_GetTicks64();
    uint64_t lastConsent = connectedAt;
    uint64_t lastFeedback = connectedAt;
    nextCandidatePoll_ = connectedAt;

    while (!quit_ && control_.connected()) {
        std::string message;
        std::string socketError;
        if (control_.read_text(message, 2, socketError)) {
            handle_control_message(message);
            const auto parsed = nlohmann::json::parse(message, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object() &&
                parsed.value("type", std::string()) == "settings" &&
                parsed.value("action", std::string()) == "webrtc") {
                peerRequested = true;
            }
        } else if (!socketError.empty() && !quit_) {
            fail(socketError);
            return;
        }

        const uint64_t controlNow = SDL_GetTicks64();
        if (controlNow - lastPingRequest >= 2000) {
            lastPingRequest = controlNow;
            control_.send_ping();
        }
        const int measuredPing = control_.last_ping_ms();
        if (measuredPing >= 0) pingMs_ = measuredPing;
        if (controllerId_.load() <= 0 &&
            (!announcedController ||
             controlNow - lastControllerAnnouncement >= 2000)) {
            announcedController = true;
            lastControllerAnnouncement = controlNow;
            // The Android TV client uses String.valueOf(localDeviceId). The
            // gateway returns this name together with the assigned remote id.
            send_control_json(nlohmann::json({
                {"type", "controller"}, {"action", "connected"},
                {"name", "1"}}).dump());
            log("controller registration requested name=1");
        }
        if (state_ == EngineState::Failed) return;

        if (nativeMediaStarted_) {
            const uint64_t now = SDL_GetTicks64();
            if (nativeRecovering_.load()) {
                request_native_keyframe("recovery watchdog");
            }
            if (state_ == EngineState::WaitingForVideo && !gotFrame_ &&
                nativeStartedTicks_ > 0 && now - nativeStartedTicks_ > 30000) {
                fail(gotVideoPacket_
                         ? "Llegan paquetes UDP, pero no se pudo reconstruir el video."
                         : "El gateway nativo conecto, pero no envio video UDP.");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (peerRequested && !peer_) {
            if (!setup_peer()) return;
        }

        if (peer_) {
            bool drained = false;
            {
                std::lock_guard<std::mutex> lock(peerMutex_);
                for (int index = 0; peer_ && index < 64; ++index) {
                    if (peer_connection_loop(peer_) > 0) drained = true;
                    else break;
                }
                if (channelAssociationReady_ && !dataChannelOpened_) {
                    dataChannelOpened_ =
                        peer_connection_create_datachannel_sid(
                            peer_, DATA_CHANNEL_RELIABLE, 0, 0,
                            const_cast<char*>("ClientDataChannel"),
                            const_cast<char*>(""), 0) == 0;
                    log(dataChannelOpened_ ? "ClientDataChannel opened"
                                           : "ClientDataChannel open deferred");
                }
            }

            const uint64_t now = SDL_GetTicks64();
            const PeerConnectionState peerState = peerState_.load();
            if ((peerState == PEER_CONNECTION_CONNECTED ||
                 peerState == PEER_CONNECTION_COMPLETED) &&
                state_ == EngineState::Negotiating) {
                state_ = EngineState::WaitingForVideo;
                set_status("Conectado. Esperando el primer fotograma...");
                send_control_json(nlohmann::json({
                    {"type", "settings"}, {"action", "ready"}}).dump());
                send_control_json(nlohmann::json({
                    {"type", "stream"}, {"action", "page"},
                    {"is_visible", true}}).dump());
            }
            if (peerState == PEER_CONNECTION_FAILED ||
                peerState == PEER_CONNECTION_DISCONNECTED) {
                fail("La conexion WebRTC con el gateway fallo.");
                return;
            }
            if (now - lastConsent > 2000) {
                lastConsent = now;
                std::lock_guard<std::mutex> lock(peerMutex_);
                if (peer_) peer_connection_send_consent(peer_);
            }
            if (now - lastFeedback > 1000) {
                lastFeedback = now;
                uint8_t fraction = 0;
                uint32_t lost = 0, highest = 0;
                std::lock_guard<std::mutex> lock(peerMutex_);
                if (peer_ && jitter_.report_stats(&fraction, &lost, &highest)) {
                    peer_connection_send_receiver_report(peer_, fraction, lost,
                                                         highest, 0);
                    peer_connection_send_remb(peer_, 20000000);
                }
            }
            if (state_ == EngineState::WaitingForVideo &&
                now - connectedAt > 30000 && !gotFrame_) {
                fail("WebRTC conecto, pero Boosteroid no envio video.");
                return;
            }
            if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            if (SDL_GetTicks64() - connectedAt > 20000 && !peerRequested) {
                fail("El gateway no solicito iniciar WebRTC.");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (!quit_ && state_ != EngineState::Failed) {
        state_ = EngineState::Stopped;
        set_status("La sesion termino.");
    }
}

bool Engine::connect_gateway(const ZERODROID::StreamSessionConfig& config) {
    state_ = EngineState::ConnectingGateway;
    set_status("Conectando con el gateway de streaming...");
    // The details endpoint signs a particular VM gateway.  It must take
    // priority over any generic/preferred list entry.
    if (!config.assignedGateway.empty()) gatewayHost_ = config.assignedGateway;
    if (gatewayHost_.empty() && !preferredGateway_.empty()) {
        gatewayHost_ = preferredGateway_;
    }
    if (gatewayHost_.empty() && !config.gateways.empty()) {
        gatewayHost_ = config.gateways.front().host;
    }
    if (gatewayHost_.rfind("wss://", 0) == 0) gatewayHost_.erase(0, 6);
    if (gatewayHost_.rfind("https://", 0) == 0) gatewayHost_.erase(0, 8);
    const auto slash = gatewayHost_.find('/');
    if (slash != std::string::npos) gatewayHost_.erase(slash);
    if (gatewayHost_.empty()) {
        fail("Boosteroid no devolvio un gateway de streaming.");
        return false;
    }

    std::string query = config.signedQuery;
    if (!query.empty() && query.front() == '?') query.erase(0, 1);
    if (query.empty()) {
        for (const std::string& candidate : config.sessionQueries) {
            std::string normalized = candidate;
            if (!normalized.empty() && normalized.front() == '?') {
                normalized.erase(0, 1);
            }
            if (query_session_id(normalized) == sessionId_ &&
                normalized.find('&') != std::string::npos) {
                query = std::move(normalized);
                break;
            }
        }
    }
    if (query.empty()) {
        fail("Falta la firma para conectarse al gateway.");
        return false;
    }

    gatewayApiBase_ = "https://" + without_port(gatewayHost_) + "/webrtc";
    // Exact Android TV control route and device identity. The requested video
    // dimensions come from the user's display profile: 720p, 1080p, or the
    // experimental 1440p supersampling mode.
    const std::string path = "/native?" + query +
        "&x=" + std::to_string(streamWidth_) +
        "&y=" + std::to_string(streamHeight_) +
        "&lang=es&tv=1&clientType=native&devType=tv&os=atv";
    std::string error;
    log("gateway=" + gatewayHost_ + " queryLength=" +
        std::to_string(query.size()) + " controlPath=/native device=tv video=" +
        std::to_string(streamWidth_) + "x" + std::to_string(streamHeight_));
    if (!control_.connect(gatewayHost_, path, "romfs:/cacert.pem", error)) {
        fail(error.empty() ? "No se pudo abrir el canal de control." : error);
        return false;
    }
    return true;
}

bool Engine::start_native_udp(const std::string& host, int videoPort,
                              int audioPort) {
    if (nativeMediaStarted_) return true;
    if (host.empty() || videoPort <= 0 || audioPort <= 0) {
        fail("El gateway devolvio una configuracion UDP incompleta.");
        return false;
    }

    std::string error;
    int videoLocalPort = 0;
    int audioLocalPort = 0;
    const int videoSocket = open_udp_channel(
        host, videoPort, error, videoLocalPort);
    if (videoSocket < 0) {
        fail(error);
        return false;
    }
    const int audioSocket = open_udp_channel(
        host, audioPort, error, audioLocalPort);
    if (audioSocket < 0) {
        ::close(videoSocket);
        fail(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        nativeUdpHost_ = host;
        nativeVideoPort_ = videoPort;
        nativeAudioPort_ = audioPort;
        nativeVideoSocket_ = videoSocket;
        nativeAudioSocket_ = audioSocket;
    }
    nativeStartedTicks_ = SDL_GetTicks64();
    nativeMediaStarted_ = true;
    state_ = EngineState::WaitingForVideo;
    set_status("Canal nativo conectado. Esperando el primer fotograma...");
    log("native UDP ready videoLocalPort=" +
        std::to_string(videoLocalPort) + " audioLocalPort=" +
        std::to_string(audioLocalPort));
    nativeVideoThread_ = std::thread(&Engine::native_video_loop, this);
    nativeAudioThread_ = std::thread(&Engine::native_audio_loop, this);
    return true;
}

void Engine::stop_native_udp() {
    int videoSocket = -1;
    int audioSocket = -1;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        videoSocket = nativeVideoSocket_;
        audioSocket = nativeAudioSocket_;
        nativeVideoSocket_ = -1;
        nativeAudioSocket_ = -1;
    }
    if (videoSocket >= 0) {
        ::shutdown(videoSocket, SHUT_RDWR);
        ::close(videoSocket);
    }
    if (audioSocket >= 0) {
        ::shutdown(audioSocket, SHUT_RDWR);
        ::close(audioSocket);
    }
    if (nativeVideoThread_.joinable()) nativeVideoThread_.join();
    if (nativeAudioThread_.joinable()) nativeAudioThread_.join();
    nativeMediaStarted_ = false;
}

void Engine::native_video_loop() {
    int socket = -1;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        socket = nativeVideoSocket_;
    }
    std::array<uint8_t, 2048> packet{};
    uint64_t lastKeepalive = SDL_GetTicks64();
    while (!quit_ && socket >= 0) {
        const ssize_t received = ::recv(socket, packet.data(), packet.size(), 0);
        if (received > 0) {
            handle_native_video_packet(packet.data(),
                                       static_cast<size_t>(received));
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR && !quit_) {
            log("native video UDP receive error=" + std::to_string(errno));
            break;
        }
        const uint64_t now = SDL_GetTicks64();
        if (now - lastKeepalive >= 10000) {
            static constexpr char keepalive[] = "{\"type\":\"keepalive\"}";
            ::send(socket, keepalive, sizeof(keepalive) - 1, 0);
            lastKeepalive = now;
        }
    }
}

void Engine::native_audio_loop() {
    int socket = -1;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        socket = nativeAudioSocket_;
    }
    std::array<uint8_t, 2048> packet{};
    uint64_t lastKeepalive = SDL_GetTicks64();
    while (!quit_ && socket >= 0) {
        const ssize_t received = ::recv(socket, packet.data(), packet.size(), 0);
        if (received >= 12 && (packet[0] & 0xc0U) == 0x80U &&
            (packet[1] & 0x7fU) == 97U) {
            on_audio(packet.data(), static_cast<size_t>(received), this);
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR && !quit_) {
            log("native audio UDP receive error=" + std::to_string(errno));
            break;
        }
        const uint64_t now = SDL_GetTicks64();
        if (now - lastKeepalive >= 10000) {
            static constexpr char keepalive[] = "{\"type\":\"keepalive\"}";
            ::send(socket, keepalive, sizeof(keepalive) - 1, 0);
            lastKeepalive = now;
        }
    }
}

bool Engine::decrypt_native_chunk(uint16_t group, uint16_t index,
                                  const uint8_t* encrypted, size_t size,
                                  std::vector<uint8_t>& plaintext) {
    if (!encrypted || size == 0 || size % 16 != 0) return false;
    std::string keyHex;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        keyHex = nativeKeyHex_;
    }
    if (keyHex.size() % 2 != 0) return false;
    std::vector<uint8_t> key(keyHex.size() / 2);
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t at = 0; at < key.size(); ++at) {
        const int high = nibble(keyHex[at * 2]);
        const int low = nibble(keyHex[at * 2 + 1]);
        if (high < 0 || low < 0) return false;
        key[at] = static_cast<uint8_t>((high << 4) | low);
    }
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;

    std::array<uint8_t, 16> iv{};
    iv[6] = static_cast<uint8_t>(group & 0xffU);
    iv[7] = static_cast<uint8_t>(group >> 8);
    iv[14] = static_cast<uint8_t>(index & 0xffU);
    iv[15] = static_cast<uint8_t>(index >> 8);
    plaintext.resize(size);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const int keyResult = mbedtls_aes_setkey_dec(
        &aes, key.data(), static_cast<unsigned int>(key.size() * 8));
    const int decryptResult = keyResult == 0
        ? mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, size, iv.data(),
                                encrypted, plaintext.data())
        : keyResult;
    mbedtls_aes_free(&aes);
    return decryptResult == 0;
}

bool Engine::recover_native_group(NativeVideoGroup& group) {
    const std::size_t dataCount = group.dataPackets;
    const std::size_t totalCount = group.totalPackets;
    if (dataCount == 0 || totalCount <= dataCount || totalCount > 255 ||
        group.receivedCount < dataCount || group.chunks.size() != totalCount ||
        group.received.size() != totalCount) return false;

    std::vector<std::size_t> available;
    available.reserve(dataCount);
    for (std::size_t index = 0;
         index < totalCount && available.size() < dataCount; ++index) {
        if (group.received[index] && group.chunks[index].size() == 1410) {
            available.push_back(index);
        }
    }
    if (available.size() != dataCount) return false;

    std::call_once(g_gf_once, init_gf256);
    const std::size_t parityCount = totalCount - dataCount;
    std::vector<uint8_t> decodeMatrix(dataCount * dataCount, 0);
    for (std::size_t row = 0; row < dataCount; ++row) {
        const std::size_t shard = available[row];
        if (shard < dataCount) {
            decodeMatrix[row * dataCount + shard] = 1;
            continue;
        }
        const std::size_t parityRow = shard - dataCount;
        for (std::size_t column = 0; column < dataCount; ++column) {
            const uint8_t denominator = static_cast<uint8_t>(
                (parityCount + column) ^ parityRow);
            if (denominator == 0) return false;
            decodeMatrix[row * dataCount + column] =
                gf_inverse(denominator);
        }
    }
    std::vector<uint8_t> inverse;
    if (!invert_gf_matrix(decodeMatrix, dataCount, inverse)) return false;

    std::size_t recovered = 0;
    for (std::size_t missing = 0; missing < dataCount; ++missing) {
        if (group.received[missing]) continue;
        std::vector<uint8_t> shard(1410, 0);
        for (std::size_t source = 0; source < dataCount; ++source) {
            const uint8_t coefficient =
                inverse[missing * dataCount + source];
            if (coefficient == 0) continue;
            const auto& input = group.chunks[available[source]];
            if (coefficient == 1) {
                for (std::size_t at = 0; at < shard.size(); ++at) {
                    shard[at] ^= input[at];
                }
            } else {
                for (std::size_t at = 0; at < shard.size(); ++at) {
                    shard[at] ^= gf_multiply(coefficient, input[at]);
                }
            }
        }
        group.chunks[missing] = std::move(shard);
        group.received[missing] = true;
        ++recovered;
    }
    if (recovered > 0) {
        log("native Reed-Solomon recovered packets=" +
            std::to_string(recovered));
    }
    return std::all_of(group.received.begin(),
                       group.received.begin() + dataCount,
                       [](bool value) { return value; });
}

void Engine::process_native_video_group(NativeVideoGroup complete) {
    const auto failGroup = [&](const std::string& reason) {
        begin_native_recovery(reason.c_str(), true);
        ++nativeDroppedGroups_;
        log("native frame dropped: " + reason + " group=" +
            std::to_string(complete.id));
    };
    const bool allOriginal = std::all_of(
        complete.received.begin(),
        complete.received.begin() + complete.dataPackets,
        [](bool value) { return value; });
    if (!allOriginal) {
        if (!recover_native_group(complete)) {
            failGroup("Reed-Solomon recovery failed");
            return;
        }
        ++nativeRecoveredGroups_;
    }

    std::vector<uint8_t> accessUnit;
    accessUnit.reserve(static_cast<size_t>(complete.dataPackets) * 1408);
    for (uint16_t index = 0; index < complete.dataPackets; ++index) {
        const auto& chunk = complete.chunks[index];
        if (chunk.size() != 1410) {
            failGroup("invalid shard size");
            return;
        }
        const uint16_t payloadBytes = read_le16(chunk.data());
        if (payloadBytes > 1408) {
            failGroup("invalid plaintext length");
            return;
        }
        std::vector<uint8_t> plaintext;
        if (!decrypt_native_chunk(complete.id, index, chunk.data() + 2,
                                  1408, plaintext)) {
            failGroup("AES decrypt failed");
            return;
        }
        if (payloadBytes > plaintext.size()) {
            failGroup("decrypted shard too short");
            return;
        }
        accessUnit.insert(accessUnit.end(), plaintext.begin(),
                          plaintext.begin() + payloadBytes);
    }
    bool hasIdr = false;
    if (accessUnit.empty() ||
        !annexb_video_info(accessUnit.data(), accessUnit.size(), &hasIdr)) {
        failGroup("invalid Annex-B");
        return;
    }

    bool resetDecoder = nativeWaitingKeyframe_.load();
    if (resetDecoder && !hasIdr) {
        // Ask the native gateway for a fresh intra frame, but never remain in
        // a permanent frozen state if it ignores the request. After a short
        // guard period, probe the decoder with a complete access unit while
        // retaining its old reference surfaces. If that probe is invalid the
        // decoder error path re-enters hard recovery and tries again.
        request_native_keyframe("waiting for IDR");
        const uint64_t now = SDL_GetTicks64();
        uint64_t started = nativeRecoveryStartedTicks_.load();
        if (started == 0) {
            nativeRecoveryStartedTicks_ = now;
            started = now;
        }
        constexpr uint64_t kIdrFallbackMs = 2500;
        if (now - started < kIdrFallbackMs) return;
        nativeWaitingKeyframe_ = false;
        resetDecoder = false;
        nativeRecoveryStartedTicks_ = now;
        log("native IDR timeout: probing decoder with a complete access unit");
    }
    if (hasIdr) {
        const bool wasWaiting = nativeWaitingKeyframe_.exchange(false);
        const bool wasRecovering = nativeRecovering_.load();
        resetDecoder = resetDecoder || wasWaiting || wasRecovering;
        if (wasRecovering) {
            log("native clean IDR received; decoder recovery can resume");
        }
    }
    if (!gotAccessUnit_.exchange(true)) {
        log("first complete native H264 access unit bytes=" +
            std::to_string(accessUnit.size()));
    }
    bool queueOverflow = false;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        if (videoQueue_.size() >= 24) {
            videoQueue_.clear();
            ++nativeDroppedGroups_;
            queueOverflow = true;
        } else {
            VideoAccessUnit unit;
            unit.data = std::move(accessUnit);
            unit.timestamp = complete.id;
            unit.native = true;
            unit.resetDecoder = resetDecoder;
            unit.recoveryProbe = nativeRecovering_.load();
            videoQueue_.push_back(std::move(unit));
        }
    }
    if (queueOverflow) {
        begin_native_recovery("video queue overflow", true);
        return;
    }
    videoCv_.notify_one();
}

void Engine::handle_native_video_packet(const uint8_t* data, size_t size) {
    // Android TV's native transport uses a 10-byte Reed-Solomon header plus
    // a 2-byte plaintext length and one 1408-byte AES-CBC block.
    if (!data || size != 1420) return;
    const uint64_t now = SDL_GetTicks64();
    lastMediaTicks_ = now;
    if (!gotVideoPacket_.exchange(true)) {
        log("first native video UDP packet bytes=" + std::to_string(size));
    }
    const uint16_t groupId = read_le16(data + 2);
    const uint16_t packetIndex = read_le16(data + 4);
    const uint16_t dataPackets = read_le16(data + 6);
    const uint16_t totalPackets = read_le16(data + 8);
    if (dataPackets == 0 || totalPackets < dataPackets ||
        dataPackets > 1024 || totalPackets > 1024 ||
        packetIndex >= totalPackets) return;

    // 1080p produces substantially more UDP shards than the old 720p-only
    // path. Eight frames / 140 ms was too aggressive and generated false loss
    // during normal Wi-Fi reordering. Keep a wider but still bounded window.
    constexpr uint64_t kNativeHoldMs = 350;
    constexpr uint16_t kMaximumReorderGroups = 24;
    constexpr size_t kMaximumPendingGroups = 48;
    std::vector<NativeVideoGroup> ready;
    uint32_t droppedGaps = 0;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (!nativeSequenceStarted_) {
            nativeSequenceStarted_ = true;
            nativeNextGroup_ = groupId;
        } else if (!nativeAnyQueued_ && sequence_newer16(nativeNextGroup_, groupId) &&
                   static_cast<uint16_t>(nativeNextGroup_ - groupId) <= 4) {
            // The first datagram can belong to frame N+1. Before anything has
            // been emitted, allow a small backwards correction.
            nativeNextGroup_ = groupId;
        } else if (groupId != nativeNextGroup_ &&
                   !sequence_newer16(groupId, nativeNextGroup_)) {
            return;  // late datagram for a frame already emitted/dropped
        }

        auto& group = nativeGroups_[groupId];
        if (group.dataPackets != dataPackets ||
            group.totalPackets != totalPackets) {
            group = NativeVideoGroup();
            group.id = groupId;
            group.dataPackets = dataPackets;
            group.totalPackets = totalPackets;
            group.firstSeenMs = now;
            group.chunks.resize(totalPackets);
            group.received.assign(totalPackets, false);
        }
        if (!group.received[packetIndex]) {
            group.chunks[packetIndex].assign(data + 10, data + size);
            group.received[packetIndex] = true;
            ++group.receivedCount;
        }

        for (int guard = 0; guard < 96; ++guard) {
            auto expected = nativeGroups_.find(nativeNextGroup_);
            if (expected != nativeGroups_.end()) {
                const bool originals = std::all_of(
                    expected->second.received.begin(),
                    expected->second.received.begin() +
                        expected->second.dataPackets,
                    [](bool value) { return value; });
                const bool recoverable =
                    expected->second.receivedCount >=
                        expected->second.dataPackets;
                if ((originals || recoverable) && !nativeKeyHex_.empty()) {
                    ready.push_back(std::move(expected->second));
                    nativeGroups_.erase(expected);
                    nativeAnyQueued_ = true;
                    nativeNextGroup_ = static_cast<uint16_t>(nativeNextGroup_ + 1);
                    continue;
                }
            }

            uint16_t farthest = 0;
            uint16_t nearestDelta = 0xffff;
            uint16_t nearestNewer = nativeNextGroup_;
            uint64_t oldestNewer = now;
            bool haveNewer = false;
            for (const auto& [id, pending] : nativeGroups_) {
                if (!sequence_newer16(id, nativeNextGroup_)) continue;
                haveNewer = true;
                const uint16_t delta =
                    static_cast<uint16_t>(id - nativeNextGroup_);
                farthest = std::max(farthest, delta);
                if (delta < nearestDelta) {
                    nearestDelta = delta;
                    nearestNewer = id;
                }
                oldestNewer = std::min(oldestNewer, pending.firstSeenMs);
            }
            const bool timedOut = expected != nativeGroups_.end()
                ? now - expected->second.firstSeenMs >= kNativeHoldMs
                : haveNewer && now - oldestNewer >= kNativeHoldMs;
            const bool windowExceeded =
                farthest >= kMaximumReorderGroups ||
                nativeGroups_.size() >= kMaximumPendingGroups;
            if (timedOut || windowExceeded) {
                if (expected != nativeGroups_.end()) {
                    nativeGroups_.erase(expected);
                    nativeNextGroup_ =
                        static_cast<uint16_t>(nativeNextGroup_ + 1);
                } else if (haveNewer) {
                    // Jump directly to the nearest observed group instead of
                    // walking through every absent id and logging one gap for
                    // every packet. This is what caused the log storm in 0.8.4.
                    nativeNextGroup_ = nearestNewer;
                } else {
                    break;
                }
                nativeAnyQueued_ = true;
                ++droppedGaps;
                continue;
            }
            break;
        }
    }

    if (droppedGaps > 0) {
        nativeDroppedGroups_.fetch_add(droppedGaps);
        // Do not enqueue a mixed batch spanning the discontinuity. Existing
        // decoder-queue units remain valid; newly completed groups will be
        // marked as recovery probes on the next packet.
        ready.clear();
        // Soft recovery: do not flush or gate on IDR merely because one native
        // group was late. Feed the next complete units to FFmpeg and let its
        // error concealment preserve the reference chain. An actual decode
        // error escalates to hard IDR recovery in decode_loop().
        begin_native_recovery("native sequence gap", false);
    }
    for (auto& group : ready) process_native_video_group(std::move(group));
}

bool Engine::setup_peer() {
    state_ = EngineState::Negotiating;
    set_status("Negociando video y audio WebRTC...");
    peerId_ = uuid_v4();

    std::vector<IceEntry> iceEntries;
    try {
        gnx::Http http;
        const auto response = http.get(
            gatewayApiBase_ + "/api/getIceServers?sessionId=" +
            gnx::Http::urlencode(sessionId_), {"Accept: application/json"});
        log("ICE servers HTTP " + std::to_string(response.status));
        if (response.ok()) {
            const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
            if (!parsed.is_discarded()) collect_ice_entries(parsed, iceEntries);
        }
    } catch (const std::exception& error) {
        log(std::string("ICE server lookup: ") + error.what());
    }
    if (iceEntries.empty()) {
        iceEntries.push_back({"stun:stun.l.google.com:19302", {}, {}});
    }

    PeerConfiguration peerConfig{};
    for (std::size_t i = 0; i < iceEntries.size() && i < 5; ++i) {
        peerConfig.ice_servers[i].urls = iceEntries[i].url.c_str();
        peerConfig.ice_servers[i].username = iceEntries[i].username.empty()
            ? nullptr : iceEntries[i].username.c_str();
        peerConfig.ice_servers[i].credential = iceEntries[i].credential.empty()
            ? nullptr : iceEntries[i].credential.c_str();
    }
    peerConfig.audio_codec = CODEC_OPUS;
    peerConfig.video_codec = CODEC_H264;
    peerConfig.datachannel = DATA_CHANNEL_STRING;
    peerConfig.onaudiotrack = &Engine::on_audio;
    peerConfig.onvideotrack = &Engine::on_video;
    peerConfig.user_data = this;

    std::string offer;
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        peer_ = peer_connection_create(&peerConfig);
        if (!peer_) {
            fail("No se pudo crear PeerConnection.");
            return false;
        }
        peer_connection_oniceconnectionstatechange(peer_, &Engine::on_peer_state);
        peer_connection_ondatachannel(peer_, &Engine::on_channel_message,
                                      &Engine::on_channel_open, nullptr);
        const char* generated = peer_connection_create_offer(peer_);
        if (!generated) {
            fail("No se pudo crear la oferta WebRTC.");
            return false;
        }
        offer = generated;
    }
    log("offer bytes=" + std::to_string(offer.size()));

    try {
        gnx::Http http;
        const std::string url = gatewayApiBase_ + "/api/call?peerid=" +
            gnx::Http::urlencode(peerId_) + "&sessionId=" +
            gnx::Http::urlencode(sessionId_);
        const auto response = http.post(
            url, nlohmann::json({{"type", "offer"}, {"sdp", offer}}).dump(),
            {"Accept: application/json", "Content-Type: application/json"});
        log("WebRTC offer HTTP " + std::to_string(response.status));
        if (!response.ok()) {
            fail("El gateway rechazo la oferta WebRTC (HTTP " +
                 std::to_string(response.status) + ").");
            return false;
        }
        const auto answerJson = nlohmann::json::parse(response.body, nullptr, false);
        const std::string answer = answerJson.is_discarded()
            ? std::string()
            : find_nested_string(answerJson, {"sdp"});
        if (answer.empty()) {
            fail("El gateway devolvio una respuesta WebRTC no valida.");
            return false;
        }

        // libpeer builds ICE pairs only when the remote description is first
        // installed. Boosteroid trickles its candidates through a REST poll,
        // so gather at least the first route before setRemoteDescription.
        const bool answerContainsCandidates =
            !candidates_from_sdp(answer).empty();
        std::vector<std::string> answerCandidates;
        const auto candidateDeadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(8);
        while (!answerContainsCandidates && answerCandidates.empty() &&
               std::chrono::steady_clock::now() < candidateDeadline && !quit_) {
            try {
                const auto candidateResponse = http.get(
                    gatewayApiBase_ + "/api/getIceCandidate?peerid=" +
                    gnx::Http::urlencode(peerId_) + "&sessionId=" +
                    gnx::Http::urlencode(sessionId_),
                    {"Accept: application/json"});
                if (candidateResponse.ok()) {
                    const auto candidateJson = nlohmann::json::parse(
                        candidateResponse.body, nullptr, false);
                    if (!candidateJson.is_discarded()) {
                        collect_candidate_strings(candidateJson, answerCandidates);
                    }
                }
            } catch (...) {
            }
            if (answerCandidates.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
        {
            std::lock_guard<std::mutex> lock(peerMutex_);
            if (!answerContainsCandidates) {
                for (const std::string& candidate : answerCandidates) {
                    if (remoteCandidates_.insert(candidate).second) {
                        peer_connection_add_ice_candidate(
                            peer_, const_cast<char*>(candidate.c_str()));
                    }
                }
            }
            peer_connection_set_remote_description(
                peer_, answer.c_str(), SDP_TYPE_ANSWER);
        }
        log("answer bytes=" + std::to_string(answer.size()) +
            " remoteCandidates=" +
            std::to_string(answerContainsCandidates
                               ? candidates_from_sdp(answer).size()
                               : answerCandidates.size()));

        // The offer already contains gathered candidates. Also trickle them to
        // the endpoint used by the official web client for gateway variants
        // that do not consume candidates embedded in SDP.
        int mediaIndex = 0;
        for (const std::string& candidate : candidates_from_sdp(offer)) {
            const nlohmann::json body = {
                {"candidate", candidate}, {"sdpMid", "0"},
                {"sdpMLineIndex", mediaIndex}};
            try {
                http.post(gatewayApiBase_ + "/api/addIceCandidate?peerid=" +
                              gnx::Http::urlencode(peerId_) + "&sessionId=" +
                              gnx::Http::urlencode(sessionId_),
                          body.dump(), {"Content-Type: application/json"});
            } catch (...) {
            }
        }
    } catch (const std::exception& error) {
        fail(std::string("Fallo de señalizacion WebRTC: ") + error.what());
        return false;
    }
    return true;
}

void Engine::poll_remote_candidates() {
    if (!peer_) return;
    try {
        gnx::Http http;
        const auto response = http.get(
            gatewayApiBase_ + "/api/getIceCandidate?peerid=" +
            gnx::Http::urlencode(peerId_) + "&sessionId=" +
            gnx::Http::urlencode(sessionId_), {"Accept: application/json"});
        if (!response.ok()) return;
        const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) return;
        std::vector<std::string> candidates;
        collect_candidate_strings(parsed, candidates);
        std::lock_guard<std::mutex> lock(peerMutex_);
        for (const std::string& candidate : candidates) {
            if (!remoteCandidates_.insert(candidate).second) continue;
            peer_connection_add_ice_candidate(
                peer_, const_cast<char*>(candidate.c_str()));
            log("remote ICE " + candidate.substr(0, 120));
        }
    } catch (...) {
    }
}

void Engine::handle_control_message(const std::string& raw) {
    const auto message = nlohmann::json::parse(raw, nullptr, false);
    if (message.is_discarded() || !message.is_object()) return;
    const std::string type = message.value("type", std::string());
    const std::string action = message.value("action", std::string());
    log("control type=" + (type.empty() ? "?" : type) + " action=" +
        (action.empty() ? "?" : action) + " bytes=" +
        std::to_string(raw.size()));

    if (type == "settings" && action == "udpforward") {
        const std::string host = message.value("ip", std::string());
        const int videoPort = json_int(message, "videoport");
        const int audioPort = json_int(message, "audioport");
        log("native UDP assignment host=" +
            (host.empty() ? std::string("missing") : host) +
            " videoPort=" + std::to_string(videoPort) +
            " audioPort=" + std::to_string(audioPort));
        start_native_udp(host, videoPort, audioPort);
        return;
    }
    if (type == "settings" && action == "streamIds") {
        log("native stream dimensions=" +
            std::to_string(json_int(message, "width")) + "x" +
            std::to_string(json_int(message, "height")));
        return;
    }
    if (type == "stream" && action == "key") {
        const auto value = message.find("value");
        if (value != message.end() && value->is_string()) {
            {
                std::lock_guard<std::mutex> lock(nativeMutex_);
                nativeKeyHex_ = value->get<std::string>();
            }
            log("native stream key received bytes=" +
                std::to_string(value->get_ref<const std::string&>().size() / 2));
            send_control_json(nlohmann::json({
                {"type", "stream"}, {"action", "key"}, {"value", "ok"}}).dump());
        }
        return;
    }

    if (type == "stream" && action == "getstatus") {
        send_control_json(nlohmann::json({
            {"type", "stream"}, {"action", "status"}, {"value", "ok"},
            {"params", {
                {"type", "androidTV"}, {"ver", "v.2.5.10.tv"},
                {"gpu", "Tegra X1"}, {"proto", 2}, {"codec", "h264"},
                {"framerate_max", 60}, {"bitrate_max", 0},
                {"hdr", false}}}}).dump());
        return;
    }
    if (type == "controller" && action == "connected") {
        const int id = json_int(message, "id");
        if (id > 0) {
            controllerId_ = id;
            {
                std::lock_guard<std::mutex> lock(inputMutex_);
                padInitialized_ = false;
                inputLogged_ = false;
                previousLeft_ = {};
                previousRight_ = {};
                previousButtons_ = 0;
                const int initialAxes[6] = {0, 0, -32767, 0, 0, -32767};
                for (int axis = 0; axis < 6; ++axis) {
                    lastSentAxes_[axis] = initialAxes[axis];
                    lastAxisSentTicks_[axis] = 0;
                }
                lastInputDiagnosticTicks_ = 0;
            }
            log("controller registered name=" +
                message.value("name", std::string("?")) +
                " id=" + std::to_string(id));
        } else {
            log("controller connected response without usable id");
        }
        return;
    }
    if (type == "controller" && action == "rumble") {
        std::lock_guard<std::mutex> lock(rumbleMutex_);
        rumble_.low = static_cast<uint16_t>(std::clamp(
            message.value("left", 0), 0, 65535));
        rumble_.high = static_cast<uint16_t>(std::clamp(
            message.value("right", 0), 0, 65535));
        rumble_.durationMs = 400;
        rumblePending_ = true;
    }
}

void Engine::send_control_json(const std::string& payload) {
    if (!control_.send_text(payload)) return;
    log("send control " + payload.substr(0, 200));
}

void Engine::send_input_json(nlohmann::json payload) {
    const uint32_t sequence = inputCommand_.fetch_add(1);
    if ((sequence + 1) % 20 == 0) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        payload["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now).count();
    }

    const bool native = nativeMediaStarted_.load();
    const std::string inputType = payload.value("type", std::string());
    const bool desktopInput = inputType == "mouse" || inputType == "keyboard";

    // Android-TV controller packets are accepted as plain JSON. Boosteroid's
    // browser mouse/keyboard path, however, includes id_cmd/from_udp even when
    // carried by the control WebSocket. v0.8.4.2 sent desktop input as native
    // controller-shaped JSON, which the gateway silently ignored.
    nlohmann::json controlPayload = payload;
    if (!native || desktopInput) {
        controlPayload["id_cmd"] = sequence;
        controlPayload["from_udp"] = false;
    }
    const bool sent = control_.send_text(controlPayload.dump());
    if (sent && !inputLogged_) {
        inputLogged_ = true;
        log("first input sent type=" + inputType + " action=" +
            payload.value("action", std::string("?")) +
            " native=" + (native ? std::string("yes") : std::string("no")));
    }
    if (sent && inputType == "mouse" && !mouseInputLogged_) {
        mouseInputLogged_ = true;
        log("mouse input enabled with browser envelope on control WSS");
    }
    if (sent && inputType == "keyboard" && !keyboardInputLogged_) {
        keyboardInputLogged_ = true;
        log("keyboard input enabled with browser envelope on control WSS");
    }

    // WebRTC mode keeps the original reliable data-channel duplicate. Native
    // /native sessions have no ClientDataChannel, so the WSS packet above is
    // the authoritative desktop-input path.
    if (native) return;
    payload["id_cmd"] = sequence;
    payload["from_udp"] = true;
    const std::string channelPayload = payload.dump();
    std::lock_guard<std::mutex> lock(peerMutex_);
    if (peer_ && dataChannelOpened_ &&
        (peerState_ == PEER_CONNECTION_CONNECTED ||
         peerState_ == PEER_CONNECTION_COMPLETED)) {
        peer_connection_datachannel_send_text_sid(
            peer_, const_cast<char*>(channelPayload.data()),
            channelPayload.size(), 0);
    }
}

void Engine::send_controller_button(int button, bool pressed) {
    const int id = controllerId_.load();
    if (id <= 0 || !control_.connected()) return;
    std::lock_guard<std::mutex> lock(inputMutex_);
    send_input_json({{"type", "controller"}, {"action", "button"},
                     {"id", id}, {"button", button},
                     {"value", pressed ? 1 : 0}});
}

void Engine::send_mouse_position(float x, float y, bool visible) {
    if (!control_.connected()) return;
    ++mouseMoveCount_;
    std::lock_guard<std::mutex> lock(inputMutex_);
    send_input_json({{"type", "mouse"}, {"action", "move"},
                     {"X", std::clamp(x, 0.0f, 1.0f)},
                     {"Y", std::clamp(y, 0.0f, 1.0f)},
                     {"offsetX", 0}, {"offsetY", 0},
                     {"isVisible", visible}});
}

void Engine::send_mouse_button(int button, bool pressed) {
    if (!control_.connected()) return;
    if (pressed) ++mouseClickCount_;
    std::lock_guard<std::mutex> lock(inputMutex_);
    send_input_json({{"type", "mouse"}, {"action", "button"},
                     {"btn", button}, {"isPressed", pressed}});
}

void Engine::send_keyboard_button(int keyCode, bool pressed) {
    if (!control_.connected()) return;
    ++keyboardEventCount_;
    std::lock_guard<std::mutex> lock(inputMutex_);
    send_input_json({{"type", "keyboard"}, {"action", "button"},
                     {"isPressed", pressed}, {"code", keyCode}});
}

void Engine::send_alt_tab() {
    // Match the browser client sequence: hold Alt, tap Tab, then release Alt.
    // Small gaps prevent Windows from coalescing the four state transitions.
    log("sending ALT+TAB");
    send_keyboard_button(18, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    send_keyboard_button(9, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    send_keyboard_button(9, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    send_keyboard_button(18, false);
}

void Engine::send_steam_overlay() {
    // Steam's default in-game overlay shortcut is Shift+Tab. Keep the same
    // staggered transitions used by ALT+TAB because remote desktop input can
    // otherwise coalesce press/release events into a no-op.
    log("sending STEAM overlay shortcut SHIFT+TAB");
    send_keyboard_button(16, true);  // VK_SHIFT
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    send_keyboard_button(9, true);   // VK_TAB
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    send_keyboard_button(9, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    send_keyboard_button(16, false);
}

void Engine::send_gamepad(HidAnalogStickState left,
                          HidAnalogStickState right, u64 buttons) {
    const int id = controllerId_.load();
    if (id <= 0 || !control_.connected()) return;
    std::lock_guard<std::mutex> lock(inputMutex_);

    const bool stickComboGuide =
        (buttons & HidNpadButton_StickL) &&
        (buttons & HidNpadButton_StickR);
    const bool guide = guidePressed_.load() || stickComboGuide;
    u64 effectiveButtons = buttons;
    if (stickComboGuide) {
        effectiveButtons &= ~(HidNpadButton_StickL | HidNpadButton_StickR);
    }

    auto sendButton = [&](u64 mask, int index) {
        const bool before = (previousButtons_ & mask) != 0;
        const bool after = (effectiveButtons & mask) != 0;
        if (!padInitialized_ || before != after) {
            send_input_json({{"type", "controller"}, {"action", "button"},
                             {"id", id}, {"button", index},
                             {"value", after ? 1 : 0}});
        }
    };
    if (xboxFaceLayout_.load()) {
        // Match the physical Xbox diamond: bottom=A, right=B, left=X, top=Y.
        sendButton(HidNpadButton_B, 0);
        sendButton(HidNpadButton_A, 1);
        sendButton(HidNpadButton_Y, 2);
        sendButton(HidNpadButton_X, 3);
    } else {
        // Nintendo labels are preserved: Switch A sends A, B sends B, etc.
        sendButton(HidNpadButton_A, 0);
        sendButton(HidNpadButton_B, 1);
        sendButton(HidNpadButton_X, 2);
        sendButton(HidNpadButton_Y, 3);
    }
    sendButton(HidNpadButton_L | HidNpadButton_LeftSL |
                   HidNpadButton_RightSL, 4);
    sendButton(HidNpadButton_R | HidNpadButton_LeftSR |
                   HidNpadButton_RightSR, 5);
    sendButton(HidNpadButton_Minus, 6);
    sendButton(HidNpadButton_Plus, 7);
    sendButton(HidNpadButton_StickL, 8);
    sendButton(HidNpadButton_StickR, 9);
    // Android can report digital trigger KeyEvents in addition to axes.
    sendButton(HidNpadButton_ZL, 10);
    sendButton(HidNpadButton_ZR, 11);
    if (!padInitialized_ || guide != previousGuide_) {
        send_input_json({{"type", "controller"}, {"action", "button"},
                         {"id", id}, {"button", 16},
                         {"value", guide ? 1 : 0}});
    }

    const uint64_t now = SDL_GetTicks64();
    auto sendAxis = [&](int index, int after) {
        const int before = lastSentAxes_[index];
        if (!padInitialized_ || axis_should_send(
                before, after, lastAxisSentTicks_[index], now)) {
            send_input_json({{"type", "controller"}, {"action", "axes"},
                             {"id", id}, {"axes", index}, {"value", after}});
            lastSentAxes_[index] = after;
            lastAxisSentTicks_[index] = now;
        }
    };

    const auto leftAxes = controller_stick(left);
    const auto rightAxes = controller_stick(right);
    sendAxis(0, leftAxes.first);
    sendAxis(1, leftAxes.second);
    sendAxis(3, rightAxes.first);
    sendAxis(4, rightAxes.second);
    const int newZl = (effectiveButtons & HidNpadButton_ZL) ? 32767 : -32767;
    const int newZr = (effectiveButtons & HidNpadButton_ZR) ? 32767 : -32767;
    sendAxis(2, newZl);
    sendAxis(5, newZr);

    // Throttled diagnostic makes it possible to confirm that a physically
    // full stick reaches full-scale at the gateway without flooding logs.
    if (now - lastInputDiagnosticTicks_ >= 2000 &&
        (leftAxes.first != 0 || leftAxes.second != 0 ||
         rightAxes.first != 0 || rightAxes.second != 0)) {
        log("gamepad TX LX=" + std::to_string(leftAxes.first) +
            " LY=" + std::to_string(leftAxes.second) +
            " RX=" + std::to_string(rightAxes.first) +
            " RY=" + std::to_string(rightAxes.second));
        lastInputDiagnosticTicks_ = now;
    }

    int hat = 0;
    const bool up = effectiveButtons & HidNpadButton_Up;
    const bool down = effectiveButtons & HidNpadButton_Down;
    const bool leftPressed = effectiveButtons & HidNpadButton_Left;
    const bool rightPressed = effectiveButtons & HidNpadButton_Right;
    if (up && leftPressed) hat = 9;
    else if (up && rightPressed) hat = 3;
    else if (down && leftPressed) hat = 12;
    else if (down && rightPressed) hat = 6;
    else if (up) hat = 1;
    else if (rightPressed) hat = 2;
    else if (down) hat = 4;
    else if (leftPressed) hat = 8;

    int oldHat = 0;
    const bool oldUp = previousButtons_ & HidNpadButton_Up;
    const bool oldDown = previousButtons_ & HidNpadButton_Down;
    const bool oldLeft = previousButtons_ & HidNpadButton_Left;
    const bool oldRight = previousButtons_ & HidNpadButton_Right;
    if (oldUp && oldLeft) oldHat = 9;
    else if (oldUp && oldRight) oldHat = 3;
    else if (oldDown && oldLeft) oldHat = 12;
    else if (oldDown && oldRight) oldHat = 6;
    else if (oldUp) oldHat = 1;
    else if (oldRight) oldHat = 2;
    else if (oldDown) oldHat = 4;
    else if (oldLeft) oldHat = 8;
    if (!padInitialized_ || hat != oldHat) {
        send_input_json({{"type", "controller"}, {"action", "pad"},
                         {"id", id}, {"hat", hat}});
    }

    previousLeft_ = left;
    previousRight_ = right;
    previousButtons_ = effectiveButtons;
    previousGuide_ = guide;
    padInitialized_ = true;
}

void Engine::on_video(uint8_t* data, size_t size, void* user) {
    auto* self = static_cast<Engine*>(user);
    self->lastMediaTicks_ = SDL_GetTicks64();
    if (!self->gotVideoPacket_.exchange(true)) {
        self->log("first video RTP packet bytes=" + std::to_string(size));
    }
    if (self->decoderResyncRequested_.exchange(false)) {
        self->jitter_.resync();
    }
    bool wantKeyframe = false;
    self->jitter_.receive(
        data, size, SDL_GetTicks64(),
        [self, &wantKeyframe](const uint8_t* accessUnit, size_t bytes,
                              uint32_t timestamp) {
            if (!self->gotAccessUnit_.exchange(true)) {
                self->log("first complete H264 access unit bytes=" +
                          std::to_string(bytes));
            }
            {
                std::lock_guard<std::mutex> lock(self->videoMutex_);
                if (self->videoQueue_.size() >= 16) {
                    self->videoQueue_.clear();
                    self->decoderResyncRequested_ = true;
                    self->decoderFlushOnKeyframe_ = true;
                    wantKeyframe = true;
                    return;
                }
                VideoAccessUnit unit;
                unit.data.assign(accessUnit, accessUnit + bytes);
                unit.timestamp = timestamp;
                unit.native = false;
                unit.resetDecoder =
                    self->decoderFlushOnKeyframe_.exchange(false);
                self->videoQueue_.push_back(std::move(unit));
            }
            self->videoCv_.notify_one();
        },
        [self](uint16_t pid, uint16_t blp) {
            if (self->peer_) peer_connection_send_nack(self->peer_, pid, blp);
        },
        &wantKeyframe);
    if (wantKeyframe) self->request_keyframe_locked();
}

void Engine::on_audio(uint8_t* data, size_t size, void* user) {
    auto* self = static_cast<Engine*>(user);
    self->lastMediaTicks_ = SDL_GetTicks64();
    if (!self->gotAudioPacket_.exchange(true)) {
        self->log("first audio RTP packet bytes=" + std::to_string(size));
    }
    if (size < 12) return;
    const uint8_t csrcCount = data[0] & 0x0fU;
    const bool extension = (data[0] & 0x10U) != 0;
    const bool padding = (data[0] & 0x20U) != 0;
    const uint16_t sequence =
        (static_cast<uint16_t>(data[2]) << 8) | data[3];
    size_t offset = 12 + static_cast<size_t>(csrcCount) * 4;
    if (extension) {
        if (offset + 4 > size) return;
        const uint16_t words =
            (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4 + static_cast<size_t>(words) * 4;
    }
    size_t end = size;
    if (padding && end > offset) {
        const uint8_t count = data[end - 1];
        if (count <= end - offset) end -= count;
    }
    if (offset <= end) self->audio_.submit(sequence, data + offset, end - offset);
}

void Engine::on_peer_state(PeerConnectionState state, void* user) {
    auto* self = static_cast<Engine*>(user);
    self->peerState_ = state;
    self->log(std::string("peer state=") +
              peer_connection_state_to_string(state));
}

void Engine::on_channel_open(void* user) {
    static_cast<Engine*>(user)->channelAssociationReady_ = true;
}

void Engine::on_channel_message(char* data, size_t size, void* user,
                                uint16_t) {
    static_cast<Engine*>(user)->handle_control_message(
        std::string(data, size));
}

void Engine::decode_loop() {
    while (!quit_) {
        VideoAccessUnit unit;
        {
            std::unique_lock<std::mutex> lock(videoMutex_);
            videoCv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return quit_.load() || !videoQueue_.empty();
            });
            if (quit_) break;
            if (videoQueue_.empty()) continue;
            unit = std::move(videoQueue_.front());
            videoQueue_.pop_front();
        }
        if (unit.resetDecoder) video_.flush();
        const bool decoded = video_.decode(unit.data.data(), unit.data.size());
        const bool decoderError = video_.take_error();
        if (decoderError) {
            {
                std::lock_guard<std::mutex> lock(videoMutex_);
                videoQueue_.clear();
            }
            if (unit.native) {
                ++nativeDroppedGroups_;
                begin_native_recovery("native decoder corruption", true);
            } else {
                decoderResyncRequested_ = true;
                decoderFlushOnKeyframe_ = true;
                std::lock_guard<std::mutex> lock(peerMutex_);
                request_keyframe_locked();
            }
            continue;
        }
        if (!decoded) {
            continue;
        }
        if (unit.native && unit.recoveryProbe &&
            nativeRecovering_.exchange(false)) {
            const uint64_t started = nativeRecoveryStartedTicks_.exchange(0);
            const uint64_t now = SDL_GetTicks64();
            log("native video recovered after " +
                std::to_string(started > 0 && now >= started ? now - started : 0) +
                " ms");
        }
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            av_frame_unref(sharedFrame_);
            if (av_frame_ref(sharedFrame_, video_.current_frame()) == 0) {
                sharedFrameValid_ = true;
                ++sharedFrameSequence_;
            }
        }
        if (!gotFrame_.exchange(true)) {
            state_ = EngineState::Streaming;
            set_status("Streaming activo");
            log("first decoded video frame");
        }
    }
}

bool Engine::begin_deko_output() {
    dkVideo_.set_logger([this](const char* value) {
        log(std::string("deko| ") + value);
    });
    const bool ready = dkVideo_.init();
    log(ready ? "deko output initialized" : "deko output initialization failed");
    return ready;
}

void Engine::end_deko_output() { dkVideo_.shutdown(); }

void Engine::pump_video() {
    if (!dkVideo_.initialized() || !gotFrame_) return;
    constexpr double kDisplayHz = 59.94;
    const double interval =
        static_cast<double>(SDL_GetPerformanceFrequency()) / kDisplayHz;
    const double now = static_cast<double>(SDL_GetPerformanceCounter());
    if (now < nextPresentCounter_) return;

    AVFrame* frame = nullptr;
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (sharedFrameValid_) {
            av_frame_unref(presentFrame_);
            if (av_frame_ref(presentFrame_, sharedFrame_) == 0) {
                frame = presentFrame_;
                sequence = sharedFrameSequence_;
            }
        }
    }
    if (frame) {
        dkVideo_.set_ping_ms(pingMs_.load());
        if (dkVideo_.render(frame)) {
            presentedSequence_ = sequence;
            if (!presentedFirstFrame_.exchange(true)) {
                log("first video frame presented");
            }
        }
    }
    nextPresentCounter_ = now + interval;
}

void Engine::request_native_keyframe(const char* reason) {
    const uint64_t now = SDL_GetTicks64();
    uint64_t previous = nativeLastKeyframeRequestTicks_.load();
    constexpr uint64_t kMinimumRequestIntervalMs = 1000;
    if (previous != 0 && now - previous < kMinimumRequestIntervalMs) return;
    if (!nativeLastKeyframeRequestTicks_.compare_exchange_strong(previous, now)) {
        return;
    }

    // The WebRTC PLI is unavailable on /native and this project has no
    // documented native PLI action. Reassert the already-observed page-visible
    // control message as a best-effort encoder refresh. Soft concealment and
    // the timed decoder probe remain the real protection against a permanent
    // freeze, so recovery does not depend on an undocumented command.
    send_control_json(nlohmann::json({
        {"type", "stream"}, {"action", "page"},
        {"is_visible", true}}).dump());
    log(std::string("native recovery refresh requested: ") +
        (reason ? reason : "recovery"));
}

void Engine::begin_native_recovery(const char* reason, bool hardWait) {
    const uint64_t now = SDL_GetTicks64();
    const bool first = !nativeRecovering_.exchange(true);
    const bool escalating = hardWait && !nativeWaitingKeyframe_.exchange(true);
    if (first || escalating || nativeRecoveryStartedTicks_.load() == 0) {
        nativeRecoveryStartedTicks_ = now;
        ++nativeRecoveryCount_;
    }
    if (hardWait) {
        std::lock_guard<std::mutex> lock(videoMutex_);
        videoQueue_.clear();
    }
    if (first || escalating) {
        log(std::string("native video recovery started: ") +
            (reason ? reason : "unknown") +
            (hardWait ? " (hard IDR wait)" : " (soft concealment)"));
    }
    request_native_keyframe(reason);
}

void Engine::request_keyframe_locked() {
    if (peer_) peer_connection_request_keyframe(peer_);
}

void Engine::destroy_peer() {
    std::lock_guard<std::mutex> lock(peerMutex_);
    if (!peer_) return;
    peer_connection_close(peer_);
    peer_connection_destroy(peer_);
    peer_ = nullptr;
}

std::string Engine::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

std::string Engine::error() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return error_;
}

std::string Engine::gateway() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return gatewayHost_;
}

void Engine::set_status(const std::string& value) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = value;
}

void Engine::fail(const std::string& value) {
    log("FAIL " + value);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        error_ = value;
        status_ = value;
    }
    state_ = EngineState::Failed;
}

bool Engine::take_rumble(RumbleCommand& out) {
    std::lock_guard<std::mutex> lock(rumbleMutex_);
    if (!rumblePending_) return false;
    out = rumble_;
    rumblePending_ = false;
    return true;
}

void Engine::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex_);
    if (!logFile_) return;
    std::fprintf(logFile_, "[%8llu] %s\n",
                 static_cast<unsigned long long>(SDL_GetTicks64()),
                 line.c_str());
}

}  // namespace gnx::stream
