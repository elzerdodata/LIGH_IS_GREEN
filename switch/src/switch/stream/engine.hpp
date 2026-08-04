#pragma once

#include <SDL2/SDL.h>
#include <switch.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../core/boosteroid_api.hpp"
#include "json.hpp"
#include "audio_player.hpp"
#include "dk_video_renderer.hpp"
#include "video_decoder.hpp"
#include "video_jitter.hpp"
#include "websocket.hpp"

extern "C" {
#include <peer_connection.h>
}

namespace gnx::stream {

enum class EngineState {
    Idle,
    StartingSession,
    ConnectingGateway,
    Negotiating,
    WaitingForVideo,
    Streaming,
    Failed,
    Stopped,
};

class Engine {
public:
    Engine(ZERODROID::BoosteroidAPI& api, SDL_Renderer* renderer);
    ~Engine();

    static void global_shutdown();

    void start(int appId, int streamWidth = 1280, int streamHeight = 720,
               const std::string& preferredGateway = {});
    void stop();
    // Close only the local transport and preserve the remote Boosteroid VM so
    // the same app can be attached again immediately.
    void disconnect_for_reconnect();

    EngineState state() const { return state_.load(); }
    std::string status() const;
    std::string error() const;
    std::string gateway() const;

    // libnx state -> Boosteroid's Android TV controller protocol.
    void send_gamepad(HidAnalogStickState left, HidAnalogStickState right,
                      u64 buttons);
    // Direct input helpers used by the delayed Minus modifier and the virtual
    // mouse. Mouse coordinates are normalized to the streamed picture (0..1).
    void send_controller_button(int button, bool pressed);
    void send_mouse_position(float x, float y, bool visible = true);
    void send_mouse_button(int button, bool pressed);
    void send_keyboard_button(int keyCode, bool pressed);
    void send_alt_tab();
    void set_xbox_face_layout(bool enabled) { xboxFaceLayout_ = enabled; }
    void set_guide_button_pressed(bool pressed) {
        guidePressed_ = pressed;
        dkVideo_.set_guide_pressed(pressed);
    }
    void set_quick_menu_state(const QuickMenuState& state) {
        dkVideo_.set_quick_menu_state(state);
    }
    int ping_ms() const { return pingMs_.load(); }
    uint32_t dropped_groups() const { return nativeDroppedGroups_.load(); }
    uint32_t recovered_groups() const { return nativeRecoveredGroups_.load(); }
    uint32_t recovery_requests() const { return nativeRecoveryCount_.load(); }
    uint64_t mouse_moves() const { return mouseMoveCount_.load(); }
    uint64_t mouse_clicks() const { return mouseClickCount_.load(); }
    uint64_t keyboard_events() const { return keyboardEventCount_.load(); }
    uint64_t session_seconds() const {
        const uint64_t started = sessionStartedTicks_.load();
        const uint64_t now = SDL_GetTicks64();
        return started == 0 || now < started ? 0 : (now - started) / 1000;
    }

    struct RumbleCommand {
        uint16_t low{0};
        uint16_t high{0};
        uint32_t durationMs{400};
    };
    bool take_rumble(RumbleCommand& out);

    bool begin_deko_output();
    void end_deko_output();
    void pump_video();

    // Public so the process-wide libpeer/FFmpeg log redirects can write to the
    // active session log without reaching through private state.
    void log(const std::string& line);

private:
    struct VideoAccessUnit {
        std::vector<uint8_t> data;
        uint32_t timestamp{0};
        bool native{false};
        bool resetDecoder{false};
        bool recoveryProbe{false};
    };

    struct NativeVideoGroup {
        uint16_t id{0};
        uint16_t dataPackets{0};
        uint16_t totalPackets{0};
        uint64_t firstSeenMs{0};
        std::size_t receivedCount{0};
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<bool> received;
    };

    void worker();
    void decode_loop();
    bool connect_gateway(const ZERODROID::StreamSessionConfig& config);
    bool setup_peer();
    bool start_native_udp(const std::string& host, int videoPort, int audioPort);
    void stop_native_udp();
    void native_video_loop();
    void native_audio_loop();
    void handle_native_video_packet(const uint8_t* data, size_t size);
    void process_native_video_group(NativeVideoGroup group);
    bool recover_native_group(NativeVideoGroup& group);
    bool decrypt_native_chunk(uint16_t group, uint16_t index,
                              const uint8_t* encrypted, size_t size,
                              std::vector<uint8_t>& plaintext);
    void destroy_peer();
    void poll_remote_candidates();
    void handle_control_message(const std::string& raw);
    void send_control_json(const std::string& payload);
    void send_input_json(nlohmann::json payload);
    void set_status(const std::string& value);
    void fail(const std::string& value);
    void request_keyframe_locked();
    void request_native_keyframe(const char* reason);
    void begin_native_recovery(const char* reason, bool hardWait);
    void shutdown(bool preserveRemoteSession);

    static void on_video(uint8_t* data, size_t size, void* user);
    static void on_audio(uint8_t* data, size_t size, void* user);
    static void on_peer_state(PeerConnectionState state, void* user);
    static void on_channel_open(void* user);
    static void on_channel_message(char* data, size_t size, void* user,
                                   uint16_t sid);

    ZERODROID::BoosteroidAPI& api_;
    SDL_Renderer* renderer_{nullptr};
    int appId_{0};
    int streamWidth_{1280};
    int streamHeight_{720};
    std::string preferredGateway_;
    std::string sessionId_;
    std::string gatewayHost_;
    std::string gatewayApiBase_;
    std::string peerId_;

    std::atomic<EngineState> state_{EngineState::Idle};
    std::atomic<bool> quit_{false};
    std::atomic<bool> gotFrame_{false};
    std::atomic<bool> gotVideoPacket_{false};
    std::atomic<bool> gotAudioPacket_{false};
    std::atomic<bool> gotAccessUnit_{false};
    std::atomic<bool> presentedFirstFrame_{false};
    std::atomic<bool> channelAssociationReady_{false};
    std::atomic<PeerConnectionState> peerState_{PEER_CONNECTION_NEW};
    std::atomic<int> controllerId_{-1};
    std::atomic<uint32_t> inputCommand_{0};
    std::atomic<bool> xboxFaceLayout_{false};
    std::atomic<bool> guidePressed_{false};
    std::atomic<int> pingMs_{-1};
    std::atomic<uint64_t> lastMediaTicks_{0};
    std::atomic<uint64_t> sessionStartedTicks_{0};
    std::atomic<bool> nativeMediaStarted_{false};
    std::atomic<uint64_t> nativeStartedTicks_{0};

    mutable std::mutex statusMutex_;
    std::string status_;
    std::string error_;

    std::thread workerThread_;
    std::thread decodeThread_;
    WssClient control_;
    PeerConnection* peer_{nullptr};
    std::mutex peerMutex_;
    bool dataChannelOpened_{false};
    std::unordered_set<std::string> remoteCandidates_;
    uint64_t nextCandidatePoll_{0};

    std::mutex nativeMutex_;
    std::string nativeUdpHost_;
    std::string nativeKeyHex_;
    int nativeVideoPort_{0};
    int nativeAudioPort_{0};
    int nativeVideoSocket_{-1};
    int nativeAudioSocket_{-1};
    std::thread nativeVideoThread_;
    std::thread nativeAudioThread_;
    std::unordered_map<uint16_t, NativeVideoGroup> nativeGroups_;
    bool nativeSequenceStarted_{false};
    bool nativeAnyQueued_{false};
    uint16_t nativeNextGroup_{0};
    std::atomic<bool> nativeWaitingKeyframe_{true};
    std::atomic<uint32_t> nativeDroppedGroups_{0};
    std::atomic<uint32_t> nativeRecoveredGroups_{0};
    // Native UDP recovery is deliberately two-stage. A missing frame first
    // enters soft recovery so FFmpeg can conceal the loss and continue using
    // its existing reference surfaces. Only an actual decoder error enters
    // hard IDR gating. This avoids the permanent freeze seen in v0.8.4 after
    // one harmless sequence gap.
    std::atomic<bool> nativeRecovering_{false};
    std::atomic<uint64_t> nativeRecoveryStartedTicks_{0};
    std::atomic<uint64_t> nativeLastKeyframeRequestTicks_{0};
    std::atomic<uint32_t> nativeRecoveryCount_{0};

    VideoJitterBuffer jitter_;
    VideoDecoder video_;
    AudioPlayer audio_;
    DkVideoRenderer dkVideo_;
    std::mutex videoMutex_;
    std::condition_variable videoCv_;
    std::deque<VideoAccessUnit> videoQueue_;
    std::atomic<bool> decoderResyncRequested_{false};
    std::atomic<bool> decoderFlushOnKeyframe_{false};

    std::mutex frameMutex_;
    AVFrame* sharedFrame_{nullptr};
    AVFrame* presentFrame_{nullptr};
    bool sharedFrameValid_{false};
    uint64_t sharedFrameSequence_{0};
    uint64_t presentedSequence_{0};
    double nextPresentCounter_{0.0};

    std::mutex inputMutex_;
    bool padInitialized_{false};
    bool inputLogged_{false};
    bool mouseInputLogged_{false};
    bool keyboardInputLogged_{false};
    std::atomic<uint64_t> mouseMoveCount_{0};
    std::atomic<uint64_t> mouseClickCount_{0};
    std::atomic<uint64_t> keyboardEventCount_{0};
    bool previousGuide_{false};
    HidAnalogStickState previousLeft_{};
    HidAnalogStickState previousRight_{};
    u64 previousButtons_{0};
    int lastSentAxes_[6]{0, 0, -32767, 0, 0, -32767};
    uint64_t lastAxisSentTicks_[6]{};
    uint64_t lastInputDiagnosticTicks_{0};

    std::mutex rumbleMutex_;
    RumbleCommand rumble_;
    bool rumblePending_{false};

    std::mutex logMutex_;
    FILE* logFile_{nullptr};
};

}  // namespace gnx::stream
