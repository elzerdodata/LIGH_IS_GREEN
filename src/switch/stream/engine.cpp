#include "engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <sstream>
#include <cstring>

extern "C" {
#include <peer.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

extern "C" void gnx_peer_log_set(void (*cb)(const char* line));

#ifndef GNX_VERSION
#define GNX_VERSION "dev"
#endif

namespace {
// libpeer's LOG_REDIRECT sink funnels through this single active engine.
gnx::stream::Engine* g_log_engine = nullptr;

// Route ffmpeg's own diagnostics (H.264 reference errors, concealment, ...)
// into stream-log; without this the decoder's complaints are invisible.
void av_log_capture(void* avcl, int level, const char* fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_WARNING) return;
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) >= 300) return;  // never flood the SD card
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (n && g_log_engine)
        g_log_engine->log(std::string("ffmpeg| ") + buf);
}

void install_av_log_capture() { av_log_set_callback(&av_log_capture); }

// libsrtp + usrsctp are process-wide: initialized with the first Engine and
// released once, by Engine::global_shutdown, on the way out of the app.
bool g_peer_initialized = false;
}

namespace gnx::stream {

namespace {
// Safety cap only: each queue entry is one H.264 NALU, and pump_video drains
// the whole queue every render frame, so this is normally near-empty. Dropping
// individual NALUs corrupts the stream, so on overflow we clear and recover
// with a keyframe instead.
constexpr size_t kMaxQueuedVideo = 64;

struct TierProfile {
    int width, height, bitrate_kbps, fps;
};

TierProfile tier_profile(QualityTier tier) {
    switch (tier) {
        case QualityTier::P720: return {1280, 720, 10000, 60};
        case QualityTier::P1080: return {1920, 1080, 20000, 60};
        case QualityTier::P1080HQ: return {1920, 1080, 30000, 60};
        case QualityTier::P1080HQTizen: return {1920, 1080, 30000, 60};
    }
    return {1920, 1080, 20000, 60};
}

const char* pacing_name(VideoPacing pacing) {
    switch (pacing) {
        case VideoPacing::Steady: return "steady";
        case VideoPacing::Smooth: return "smooth";
        case VideoPacing::Motion: return "motion";
    }
    return "steady";
}

// Extract "candidate:..." lines from a local SDP for the /ice POST.
std::vector<std::string> local_candidates_from_sdp(const std::string& sdp) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = sdp.find("a=candidate:", at)) != std::string::npos) {
        size_t end = sdp.find_first_of("\r\n", at);
        out.push_back(sdp.substr(at + 2, end - at - 2));
        at = end == std::string::npos ? sdp.size() : end;
    }
    return out;
}

std::string ufrag_from_sdp(const std::string& sdp) {
    size_t at = sdp.find("a=ice-ufrag:");
    if (at == std::string::npos) return "";
    at += std::strlen("a=ice-ufrag:");
    size_t end = sdp.find_first_of("\r\n", at);
    return sdp.substr(at, end - at);
}

unsigned long candidate_priority(const std::string& candidate) {
    // candidate:<foundation> <component> <protocol> <priority> ...
    std::istringstream fields(candidate);
    std::string token;
    for (int field = 0; field <= 3; ++field) {
        if (!(fields >> token)) return 0;
    }
    char* end = nullptr;
    unsigned long priority = std::strtoul(token.c_str(), &end, 10);
    return end && *end == '\0' ? priority : 0;
}

}  // namespace

Engine::Engine(XboxAuth& auth, SDL_Renderer* renderer)
    : auth_(auth), renderer_(renderer) {
    http_.set_abort_flag(&quit_);  // don't block shutdown on an HTTP call
    // One-time global init of libsrtp + usrsctp. Without this, srtp_create()
    // fails (no inbound SRTP -> no decryptable video) and usrsctp never
    // associates (data channels never open). Idempotent guard: Engine is a
    // singleton, but be safe.
    if (!g_peer_initialized) {
        peer_init();
        g_peer_initialized = true;
    }
}

// usrsctp's two service threads ("SCTP timer", "SCTP iterator") run until
// usrsctp_finish(). Nothing used to call it, so they were still running when
// main() returned -- and the moment hbloader unmapped the NRO underneath
// them, they faulted on their next instruction (Instruction Abort, crash
// report with the NRO already gone from the module list). Call this once,
// after the last Engine is destroyed and before the app exits.
void Engine::global_shutdown() {
    if (!g_peer_initialized) return;
    g_peer_initialized = false;
    peer_deinit();
}

Engine::~Engine() { stop(); }

void Engine::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!log_file_) return;
    std::fprintf(log_file_, "[%8llu] %s\n",
                 static_cast<unsigned long long>(SDL_GetTicks64()),
                 line.c_str());
}

void Engine::start(const std::string& title_id, QualityTier tier,
                   const std::string& locale) {
    home_server_id_.clear();
    start_common(title_id, tier, locale);
}

void Engine::start_home(const std::string& server_id, QualityTier tier,
                        const std::string& locale) {
    home_server_id_ = server_id;
    start_common("(your console)", tier, locale);
}

void Engine::start_common(const std::string& title_id, QualityTier tier,
                          const std::string& locale) {
    stop();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        selected_region_.clear();
    }
    title_id_ = title_id;
    tier_ = tier;
    // Console Remote Play uses the proven Android fingerprint to create the
    // session, but can request 1080p media after WebRTC connects. Keep that
    // experimental request at the conservative 20 Mbps tier.
    media_tier_ = !home_server_id_.empty() && tier != QualityTier::P720
                      ? QualityTier::P1080
                      : tier;
    home_720_fallback_pending_ = false;
    locale_ = locale;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_) std::fclose(log_file_);
#ifdef __SWITCH__
        // Keep the previous session's log: rotate instead of overwrite.
        std::remove("sdmc:/switch/green-nx/stream-log-prev.txt");
        std::rename("sdmc:/switch/green-nx/stream-log.txt",
                    "sdmc:/switch/green-nx/stream-log-prev.txt");
        log_file_ = std::fopen("sdmc:/switch/green-nx/stream-log.txt", "w");
        // Logging is diagnostic and must not stall the sole RTP socket pump.
        // A synchronous fflush for the once-per-second audio line was enough
        // to produce a small regular video hitch on SD cards. Keep the session
        // in memory and let fclose() flush it on clean stream shutdown (fail()
        // flushes once so an error report still reaches the card).
        if (log_file_) std::setvbuf(log_file_, nullptr, _IOFBF, 256 * 1024);
#else
        log_file_ = stderr;
#endif
    }
    g_log_engine = this;
    gnx_peer_log_set([](const char* line) {
        if (g_log_engine) g_log_engine->log(std::string("  peer| ") + line);
    });
    const char* tier_name = tier == QualityTier::P720        ? "720p/android"
                            : tier == QualityTier::P1080     ? "1080p/windows"
                            : tier == QualityTier::P1080HQ   ? "1080pHQ/windows"
                                                             : "1080pHQ/tizen-experimental";
    log("Light_is_Green v" GNX_VERSION " | stream start: " + title_id +
        " | tier " + tier_name + " | pacing " + pacing_name(pacing_));
    quit_ = false;
    got_frame_ = false;
    channels_open_ = false;
    handshake_done_ = false;
    server_ended_ = false;
    last_media_ticks_ = 0;
    peer_state_ = PEER_CONNECTION_NEW;  // previous session left it CLOSED
    pli_sent_ = 0;
    // Cumulative, and the HUD's bitrate window starts from zero in run_peer:
    // carrying the previous stream's total over shows one absurd first sample.
    video_bytes_ = 0;
    install_av_log_capture();
    jitter_.reset();
    next_present_counter_ = 0;  // first frame presents immediately, then paced
    state_ = EngineState::StartingSession;
    video_.init(renderer_);
    audio_.init();
    audio_.set_gain(audio_gain_);
#ifdef __SWITCH__
    shared_frame_ = av_frame_alloc();
    present_frame_ = av_frame_alloc();
    prev_frame_ = av_frame_alloc();
    motion_frame_ = av_frame_alloc();
    shared_frame_valid_ = false;
    shared_frame_seq_ = 0;
    last_present_seq_ = 0;
    present_hold_refreshes_ = 0;
    smooth_have_present_ = false;
    smooth_refresh_phase_ = 0;
    source_refresh_period_ = 1;
    source_fast_streak_ = source_slow_streak_ = 0;
    last_rtp_timestamp_ = 0;
    have_rtp_timestamp_ = false;
    pace_new_ = pace_repeat_ = 0;
    pace_hold1_ = pace_hold2_ = pace_hold3_ = pace_hold4p_ = 0;
    pace_skip_ = pace_generated_ = 0;
#endif
    stream_epoch_ = SDL_GetTicks64();
    thread_ = std::thread(&Engine::worker, this);
#ifdef __SWITCH__
    // Decode runs on its own thread so hardware-decode latency never delays
    // input polling or the vsync-paced present on the main thread.
    decode_thread_ = std::thread(&Engine::decode_loop, this);
#endif
}

void Engine::stop() {
    quit_ = true;
    video_cv_.notify_all();  // wake the decode thread so it can see quit_
    if (thread_.joinable()) thread_.join();
    if (decode_thread_.joinable()) decode_thread_.join();
    if (g_log_engine == this) {
        gnx_peer_log_set(nullptr);
        g_log_engine = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_ && log_file_ != stderr) std::fclose(log_file_);
        log_file_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (peer_) {
            peer_connection_close(peer_);
            peer_connection_destroy(peer_);
            peer_ = nullptr;
        }
    }
    video_.shutdown();
    audio_.shutdown();
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_queue_.clear();
    }
#ifdef __SWITCH__
    {
        // Decode thread is joined; safe to release the hand-off frames (unrefs
        // any held NVTEGRA surface back to the decoder's pool).
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (shared_frame_) av_frame_free(&shared_frame_);
        if (present_frame_) av_frame_free(&present_frame_);
        if (prev_frame_) av_frame_free(&prev_frame_);
        if (motion_frame_) av_frame_free(&motion_frame_);
        for (SmoothFrame& queued : smooth_frames_)
            if (queued.frame) av_frame_free(&queued.frame);
        smooth_frames_.clear();
        shared_frame_valid_ = false;
    }
#endif
    if (state_ != EngineState::Failed) state_ = EngineState::Stopped;
}

std::string Engine::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

std::string Engine::error() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return error_;
}

std::string Engine::selected_region() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return selected_region_;
}

void Engine::set_status(const std::string& status) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = status;
}

// Orderly end of a session that the server closed on us. Not a failure: the
// UI treats Stopped as "go back to the library", so the user lands in the menu
// the way they would after ending the stream themselves.
void Engine::end_session() {
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_) std::fflush(log_file_);
    }
    set_status("Session ended");
    state_ = EngineState::Stopped;
}

void Engine::fail(const std::string& error) {
    log("FAIL: " + error);
    {
        // The log is fully buffered (setvbuf in start_common); a failure is
        // exactly when it must survive on the card, and the stream is dead
        // here so one synchronous flush costs nothing.
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_) std::fflush(log_file_);
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        error_ = error;
    }
    state_ = EngineState::Failed;
}

// ---- libpeer callbacks ----------------------------------------------------

void Engine::on_video(uint8_t* data, size_t size, void* user) {
    // Called on the worker thread inside peer_connection_loop() (peer_mutex_
    // held). `data` is a raw RTP packet; the jitter buffer reorders/assembles
    // complete access units and only emits clean, keyframe-anchored frames.
    auto* self = static_cast<Engine*>(user);
    self->last_media_ticks_.store(SDL_GetTicks64(), std::memory_order_relaxed);
    self->video_bytes_.fetch_add(size, std::memory_order_relaxed);  // HUD bitrate
    bool want_keyframe = false;
    self->jitter_.receive(
        data, size, SDL_GetTicks64(),
        [self](const uint8_t* au, size_t au_size, uint32_t rtp_timestamp) {
            {
                std::lock_guard<std::mutex> lock(self->video_mutex_);
                if (self->video_queue_.size() >= kMaxQueuedVideo)
                    self->video_queue_.clear();
                VideoAccessUnit unit;
                unit.data.assign(au, au + au_size);
                unit.rtp_timestamp = rtp_timestamp;
                self->video_queue_.push_back(std::move(unit));
            }
            self->video_cv_.notify_one();  // wake the decode thread (Switch)
        },
        [self](uint16_t pid, uint16_t blp) {
            // Retransmit request for lost packets (peer_mutex_ already held).
            if (self->peer_) peer_connection_send_nack(self->peer_, pid, blp);
        },
        &want_keyframe);
    if (want_keyframe) self->request_keyframe_locked();
}

void Engine::on_audio(uint8_t* data, size_t size, void* user) {
    // Called on the worker thread with peer_mutex_ held. `data` is a whole RTP
    // packet (rtp_decode_generic forwards header+payload, like the H.264 path).
    // Parse the header to find the Opus payload and the sequence number, then
    // hand it straight to the audio thread -- decode happens there, not here, so
    // audio never waits behind video/RTCP work on this thread.
    auto* self = static_cast<Engine*>(user);
    self->last_media_ticks_.store(SDL_GetTicks64(), std::memory_order_relaxed);
    if (size < 12) return;
    uint8_t csrc_count = data[0] & 0x0F;
    bool has_extension = (data[0] & 0x10) != 0;
    bool has_padding = (data[0] & 0x20) != 0;
    uint16_t seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];

    size_t offset = 12 + static_cast<size_t>(csrc_count) * 4;
    if (has_extension) {
        if (offset + 4 > size) return;
        uint16_t ext_words =
            (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4 + static_cast<size_t>(ext_words) * 4;
    }
    size_t end = size;
    if (has_padding && end > offset) {
        uint8_t pad = data[end - 1];
        if (pad <= end - offset) end -= pad;
    }
    if (offset > end) return;
    self->audio_.submit(seq, data + offset, end - offset);
}

void Engine::on_channel_message(char* data, size_t size, void* user,
                                uint16_t sid) {
    static_cast<Engine*>(user)->handle_channel_message(sid, data, size);
}

void Engine::on_channel_open(void* user) {
    static_cast<Engine*>(user)->channels_open_ = true;
}

void Engine::on_state_change(PeerConnectionState state, void* user) {
    static_cast<Engine*>(user)->peer_state_ = state;
}

// ---- data channel plumbing ------------------------------------------------

void Engine::open_data_channels() {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    if (!peer_) return;
    // The DTLS client uses even SCTP stream ids (RFC 8832). xCloud maps each
    // channel by its DCEP label, so the exact ids only need to be distinct.
    struct { const xcloud::ChannelConfig& cfg; uint16_t sid; } channels[] = {
        {xcloud::kControlChannel, 0},
        {xcloud::kInputChannel, 2},
        {xcloud::kMessageChannel, 4},
        {xcloud::kChatChannel, 6},
    };
    for (const auto& channel : channels) {
        DecpChannelType type =
            channel.cfg.max_retransmits == 0
                ? (channel.cfg.ordered ? DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT
                                       : DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED)
                : (channel.cfg.ordered ? DATA_CHANNEL_RELIABLE
                                       : DATA_CHANNEL_RELIABLE_UNORDERED);
        uint32_t reliability = channel.cfg.max_retransmits < 0
                                   ? 0
                                   : static_cast<uint32_t>(channel.cfg.max_retransmits);
        peer_connection_create_datachannel_sid(
            peer_, type, 0, reliability, const_cast<char*>(channel.cfg.label),
            const_cast<char*>(channel.cfg.protocol), channel.sid);
    }
    log("opened data channels (control/input/message/chat)");
}

void Engine::send_on_channel(const char* label, const std::string& payload) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    send_on_channel_locked(label, payload);
}

// Caller must hold peer_mutex_. Used from callbacks that libpeer already
// invokes with the lock held (see handle_channel_message).
void Engine::send_on_channel_locked(const char* label,
                                    const std::string& payload) {
    if (!peer_) return;
    uint16_t sid = 0;
    // control/message/chat carry JSON -> must be WebRTC string frames, or
    // xCloud ignores them (handshake + clientdevicecapabilities/quality).
    if (peer_connection_lookup_sid(peer_, label, &sid) == 0) {
        peer_connection_datachannel_send_text_sid(
            peer_, const_cast<char*>(payload.data()), payload.size(), sid);
        log("send [" + std::string(label) + " sid=" + std::to_string(sid) +
            "] " + payload.substr(0, 220));
    } else {
        log("send FAILED (no channel '" + std::string(label) + "')");
    }
}

void Engine::send_binary_on_channel(const char* label,
                                    const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    send_binary_on_channel_locked(label, payload);
}

void Engine::send_binary_on_channel_locked(const char* label,
                                           const std::vector<uint8_t>& payload) {
    if (!peer_) return;
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(peer_, label, &sid) == 0)
        peer_connection_datachannel_send_sid(
            peer_,
            const_cast<char*>(reinterpret_cast<const char*>(payload.data())),
            payload.size(), sid);
}

void Engine::handle_channel_message(uint16_t sid, const char* data,
                                    size_t size) {
    // IMPORTANT: libpeer invokes this from inside peer_connection_loop(), which
    // the worker already runs while holding peer_mutex_. peer_mutex_ is not
    // recursive, so we must NOT re-lock it here (doing so froze the worker the
    // instant xCloud's first message arrived -> stuck on "Handshaking"). peer_
    // is guaranteed alive for the duration of this callback.
    char* label = peer_ ? peer_connection_lookup_sid_label(peer_, sid) : nullptr;
    if (label && std::strcmp(label, "input") == 0) {
        // Binary telemetry/rumble from the server. Handle it here and return so
        // the raw bytes don't spam the log -- vibration reports can arrive many
        // times a second while a game is rumbling.
        handle_input_report(reinterpret_cast<const uint8_t*>(data), size);
        return;
    }
    // Log every inbound control/message payload so the exact xCloud protocol
    // exchange is visible in stream-log.txt during bring-up.
    {
        std::string preview(data, std::min<size_t>(size, 220));
        log("recv [" + std::string(label ? label : "sid?") + " sid=" +
            std::to_string(sid) + " len=" + std::to_string(size) + "] " +
            preview);
    }
    if (!label) return;

    // End-of-session notice from the server, e.g.
    //   target=/streaming/sessionLifetimeManagement/serverInitiatedDisconnect
    //   content={"reason":"KickForStopCommand"}
    // sent when the stream is stopped on the console or the console shuts
    // down. run_peer picks the flag up and ends the stream.
    if (std::strcmp(label, "message") == 0) {
        std::string payload(data, size);
        if (payload.find("serverInitiatedDisconnect") != std::string::npos) {
            // The reason sits in the escaped inner JSON ("content"), so skip
            // over whatever quoting separates the key from its value.
            std::string reason;
            size_t at = payload.find("reason");
            if (at != std::string::npos) {
                at += 6;
                while (at < payload.size() &&
                       (payload[at] == '\\' || payload[at] == '"' ||
                        payload[at] == ':' || payload[at] == ' '))
                    ++at;
                size_t end = at;
                while (end < payload.size() &&
                       (std::isalnum(static_cast<unsigned char>(payload[end])) ||
                        payload[end] == '_' || payload[end] == '-'))
                    ++end;
                reason = payload.substr(at, end - at);
            }
            log("server ended the session" +
                (reason.empty() ? std::string() : " (" + reason + ")"));
            server_ended_ = true;
            return;
        }
    }

    if (std::strcmp(label, "message") == 0 && !handshake_done_) {
        if (xcloud::is_handshake_ack(std::string(data, size))) {
            // Handshake acked: authorize the control channel, announce the
            // gamepad, then declare client capabilities (our quality lever).
            send_on_channel_locked("control", xcloud::authorization_request());
            send_on_channel_locked("control", xcloud::gamepad_changed(0, true));
            TierProfile profile = tier_profile(media_tier_);
            for (const std::string& message : xcloud::startup_messages(
                     profile.width, profile.height, profile.bitrate_kbps,
                     profile.fps))
                send_on_channel_locked("message", message);
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                send_binary_on_channel_locked("input", input_.client_metadata());
            }
            // Ask for an IDR immediately (both the RTCP PLI that xCloud
            // actually acts on, and the app-level message) so video can start
            // instead of waiting for the server's periodic keyframe.
            peer_connection_request_keyframe(peer_);
            send_on_channel_locked("control", xcloud::video_keyframe_requested());
            last_keyframe_req_ = SDL_GetTicks64();
            log("handshake complete, capabilities sent");
            handshake_done_ = true;
            if (state_ == EngineState::Negotiating)
                state_ = EngineState::WaitingForVideo;
        }
    }
}

void Engine::handle_input_report(const uint8_t* data, size_t size) {
    // Server "input"-channel report. We only act on Vibration (type 128). The
    // wire layout matches the xbox.com/play client (ref: greenlight):
    //   [0]  report type (128 = Vibration)
    //   [2]  rumble type (0 = four-motor)     [3]  gamepad index
    //   [4]  left motor %   [5]  right motor %
    //   [6]  left-trigger % [7]  right-trigger %   (all 0..100)
    //   [8:2] duration ms (LE)  [10:2] delay ms (LE)  [12] repeat count
    if (size < 13 || data[0] != 128) return;

    auto pct = [](uint8_t v) { return v >= 100 ? 1.0f : v / 100.0f; };
    // The Switch has no trigger actuators. Fold the trigger motors into the LOW
    // band (a duller thud) instead of the high band: driving the high band hard
    // produces an audible, harsh whine, and shooters hammer the triggers.
    float low_pct = pct(data[4]) + (pct(data[6]) + pct(data[7])) * 0.5f;
    if (low_pct > 1.0f) low_pct = 1.0f;
    float high_pct = pct(data[5]);

    uint16_t duration = static_cast<uint16_t>(data[8] | (data[9] << 8));
    uint16_t delay = static_cast<uint16_t>(data[10] | (data[11] << 8));
    uint8_t repeat = data[12];

    // Each report is self-terminating: SDL plays the effect for duration_ms and
    // stops on its own, exactly like the browser client's fixed-duration effect
    // -- so no "stop" packet is needed (the input channel is unreliable). For
    // repeated pulses we approximate the whole envelope as one window (the
    // off-gaps can't be reproduced through SDL) and cap it, so a corrupt length
    // can never leave a motor stuck on.
    uint32_t duration_ms = duration;
    if (repeat > 0)
        duration_ms += static_cast<uint32_t>(repeat) * (duration + delay);
    if (duration_ms > 4000) duration_ms = 4000;

    RumbleCommand cmd;
    cmd.low = static_cast<uint16_t>(low_pct * 65535.0f);
    cmd.high = static_cast<uint16_t>(high_pct * 65535.0f);
    cmd.duration_ms = duration_ms;
    {
        std::lock_guard<std::mutex> lock(rumble_mutex_);
        rumble_cmd_ = cmd;
        rumble_pending_ = true;
    }
    if (!rumble_logged_) {
        rumble_logged_ = true;
        log("rumble: first server vibration report received");
    }
}

bool Engine::take_rumble(RumbleCommand& out) {
    std::lock_guard<std::mutex> lock(rumble_mutex_);
    if (!rumble_pending_) return false;
    out = rumble_cmd_;
    rumble_pending_ = false;
    return true;
}

// ---- worker ---------------------------------------------------------------

// The session-setup phase is pure HTTP with nothing on screen but a status
// line, so every stage logs -- otherwise a stall here leaves a banner-only
// log with no way to tell WHERE it happened.
const char* session_state_name(SessionState state) {
    switch (state) {
        case SessionState::New: return "new";
        case SessionState::Provisioning: return "provisioning";
        case SessionState::WaitingForResources: return "waiting for resources";
        case SessionState::ReadyToConnect: return "ready to connect";
        case SessionState::Provisioned: return "provisioned";
        case SessionState::Failed: return "failed";
    }
    return "?";
}

std::string queue_wait_status(int seconds) {
    if (seconds <= 0) return "Queued - less than a minute remaining";
    if (seconds < 60)
        return "Queued - about " + std::to_string(seconds) +
               " seconds remaining";

    int minutes = seconds / 60;
    int remainder = seconds % 60;
    return "Queued - about " + std::to_string(minutes) + "m " +
           std::to_string(remainder) + "s remaining";
}

void Engine::worker() {
    try {
        bool home = !home_server_id_.empty();
        set_status(home ? "Signing in to your Xbox..."
                        : "Signing in to xCloud...");
        log("fetching streaming credentials");
        StreamingCredentials creds = auth_.fetch_streaming_credentials();
        cloud_ = home ? creds.home : creds.cloud;
        if (!home) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                selected_region_ = cloud_.selected_region;
            }
            log("server region: " + cloud_.selected_region + " | " +
                cloud_.host);
        }
        // Without a host every request goes out as a bare path, which curl
        // rejects as a malformed URL -- a useless error for the one thing that
        // actually went wrong: the remote-play login did not come back.
        if (cloud_.host.empty()) {
            if (!creds.home_error.empty())
                log("xhome login failed: " + creds.home_error);
            fail(home ? "Your account has no console available for remote "
                        "play right now. Check the console is on and signed "
                        "in, then try again."
                      : "xCloud is not available for this account");
            return;
        }

        set_status(home ? "Preparing Xbox Remote Play route..."
                        : "Connecting to " + cloud_.selected_region + "...");
        GssvSession::cleanup_stale_sessions(http_, cloud_,
                                            home ? "home" : "cloud");
        log("stale-session cleanup done");

        // Home streaming: a session request against a sleeping console acts
        // as the wake-up call but fails with AgentCommandError while the
        // console boots its streaming service (same behaviour Greenlight
        // sees). Retry a few times before surfacing the failure.
        // Cloud gets one retry too: a session can come up with a dead media
        // path (ICE connects, DTLS never answers) -- a fresh session
        // re-rolls that server-side fault.
        // Console readiness and WebRTC transport are independent failure
        // domains. A slow wake/register cycle must not consume the budget for
        // fresh ICE/DTLS sessions (the old shared six-attempt counter did).
        const int max_readiness_retries = home ? 10 : 1;
        const int max_transport_retries = home ? 8 : 2;
        int readiness_retries = 0;
        int transport_retries = 0;
        int total_attempts = 0;
        bool registering = false;       // console is still registering
        bool retrying_transport = false;  // previous session reached WebRTC
        while (!quit_) {
            ++total_attempts;
            if (total_attempts > 1) {
                int retry_number = retrying_transport ? transport_retries
                                                       : readiness_retries;
                int retry_limit = retrying_transport ? max_transport_retries
                                                      : max_readiness_retries;
                std::string of = " (retry " + std::to_string(retry_number) +
                                 " of " + std::to_string(retry_limit) + ")";
                set_status(registering
                               ? "Your console is still registering..." + of
                           : retrying_transport
                               ? "Rebuilding the media connection..." + of
                           : home ? "Waking your console..." + of
                                  : "Retrying the connection..." + of);

                // A stopped xHome session can remain reserved server-side for
                // several seconds. Progressive backoff gives it time to tear
                // down before asking the console for another ICE/DTLS route.
                int wait_seconds = home
                    ? (retrying_transport
                           ? std::min(20, 8 + transport_retries * 2)
                           : std::min(15, 5 + readiness_retries * 2))
                    : 3;
                log("retry backoff: " + std::to_string(wait_seconds) +
                    "s | readiness=" + std::to_string(readiness_retries) +
                    "/" + std::to_string(max_readiness_retries) +
                    " transport=" + std::to_string(transport_retries) +
                    "/" + std::to_string(max_transport_retries));
                for (int i = 0; i < wait_seconds * 10 && !quit_; ++i)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                if (quit_) break;
            }
            set_status("Requesting a session...");
            log("requesting session #" + std::to_string(total_attempts) +
                " | readiness=" + std::to_string(readiness_retries) +
                " transport=" + std::to_string(transport_retries));
            // Home: the console agent only accepts the android fingerprint
            // (green-vita, the working reference, always sends it) -- the
            // windows/tizen quality-tier fingerprints get AgentCommandError.
            GssvSession session(http_, cloud_,
                                home ? QualityTier::P720 : tier_, locale_);
            if (home)
                session.start_home(home_server_id_);
            else
                session.start_cloud(title_id_);
            log("session created, polling state");

            set_status("Waiting for a server...");
            bool connected = false;
            bool retry_transport = false;
            SessionState logged_state = SessionState::New;
            std::string session_error;
            int queue_estimate_seconds = -1;
            auto queue_estimate_fetched_at =
                std::chrono::steady_clock::time_point{};
            auto next_queue_estimate_refresh =
                std::chrono::steady_clock::time_point{};
            for (int i = 0; i < 300 && !quit_; ++i) {
                SessionState state = session.refresh_state();
                if (state != logged_state) {
                    logged_state = state;
                    log(std::string("session state: ") +
                        session_state_name(state) + " (poll " +
                        std::to_string(i) + ")");
                }
                if (state == SessionState::WaitingForResources && !home) {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= next_queue_estimate_refresh) {
                        next_queue_estimate_refresh =
                            now + std::chrono::seconds(15);
                        std::optional<int> estimate =
                            session.fetch_wait_time(title_id_);
                        if (estimate) {
                            queue_estimate_seconds = *estimate;
                            queue_estimate_fetched_at = now;
                            log("queue estimate: " +
                                std::to_string(*estimate) + " seconds");
                        } else {
                            log("queue estimate unavailable");
                        }
                    }

                    if (queue_estimate_seconds >= 0) {
                        int elapsed = static_cast<int>(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                now - queue_estimate_fetched_at)
                                .count());
                        set_status(queue_wait_status(std::max(
                            0, queue_estimate_seconds - elapsed)));
                    } else {
                        set_status("Queued - estimating wait time...");
                    }
                } else if (state == SessionState::Provisioning) {
                    set_status(home ? "Preparing your Xbox..."
                                    : "Preparing your cloud server...");
                }
                if (state == SessionState::ReadyToConnect && !connected) {
                    set_status("Authenticating...");
                    session.connect(auth_.fetch_passport_token());
                    connected = true;
                } else if (state == SessionState::Provisioned) {
                    if (run_peer(session)) {
                        session.stop();
                        return;
                    }
                    // Dead media path: retry with a fresh session; only an
                    // exhausted transport budget surfaces a final failure.
                    retry_transport = true;
                    break;
                } else if (state == SessionState::Failed) {
                    session_error = session.error_details();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
            }
            session.stop();
            if (quit_) return;
            if (retry_transport) {
                ++transport_retries;
                bool home_720_fallback = home_720_fallback_pending_;
                home_720_fallback_pending_ = false;

                // If the experimental offer never even reaches a usable
                // transport, do not burn every retry at 1080p. The next fresh
                // xHome session uses the proven 720p profile immediately.
                if (home && media_tier_ != QualityTier::P720) {
                    media_tier_ = QualityTier::P720;
                    home_720_fallback = true;
                    log("media negotiation failed at 1080p; forcing 720p "
                        "for all remaining retries");
                }
                if (transport_retries > max_transport_retries) {
                    fail(home
                         ? "Remote Play could not establish a media path after "
                           "several fresh sessions. Leave the Xbox on for a "
                           "minute, then check NAT/IPv6/UDP or restart Remote "
                           "Features."
                         : "xCloud could not establish a media path after "
                           "several fresh sessions.");
                    return;
                }
                log(home_720_fallback
                        ? "retrying Remote Play with 720p stable capabilities"
                        : "retrying with a fresh session (dead media path)");
                {
                    // Dispose of the dead session's peer (normally stop()'s
                    // job) so the next run_peer starts from scratch.
                    std::lock_guard<std::mutex> lock(peer_mutex_);
                    if (peer_) {
                        peer_connection_close(peer_);
                        peer_connection_destroy(peer_);
                        peer_ = nullptr;
                    }
                }
                peer_state_ = PEER_CONNECTION_NEW;
                channels_open_ = false;
                handshake_done_ = false;
                server_ended_ = false;
                last_media_ticks_ = 0;
                pli_sent_ = 0;
                video_bytes_ = 0;
                jitter_.reset();
                {
                    std::lock_guard<std::mutex> lock(video_mutex_);
                    video_queue_.clear();
                }
                audio_.shutdown();
                audio_.init();
                audio_.set_gain(audio_gain_);
                state_ = EngineState::StartingSession;  // back to connect UI
                registering = false;
                retrying_transport = true;

                // A stale xHome route or short-lived token can survive a few
                // otherwise fresh sessions. Refresh the endpoint every third
                // transport failure while retaining the last known-good route
                // if Xbox's credential service is temporarily unavailable.
                if (home && transport_retries % 3 == 0) {
                    set_status("Refreshing the Xbox route...");
                    try {
                        StreamingCredentials refreshed =
                            auth_.fetch_streaming_credentials();
                        if (!refreshed.home.host.empty()) {
                            cloud_ = refreshed.home;
                            log("xhome endpoint credentials refreshed after " +
                                std::to_string(transport_retries) +
                                " media failures");
                        } else {
                            log("xhome credential refresh returned no host; "
                                "keeping the previous endpoint");
                        }
                        GssvSession::cleanup_stale_sessions(http_, cloud_,
                                                            "home");
                        log("stale xhome sessions cleaned during route refresh");
                    } catch (const std::exception& error) {
                        log(std::string("xhome route refresh/cleanup failed; "
                                        "keeping the previous endpoint: ") +
                            error.what());
                    }
                }
                continue;
            }
            // Both of these mean "the console is not ready yet, ask again":
            // AgentCommandError while it boots its streaming service, and
            // WaitingForServerToRegister while that service registers with
            // Microsoft (an awake console that was just rebooted, or one whose
            // remote features were re-enabled, sits there for a while). The
            // second one used to fall through to a hard failure immediately,
            // so remote play looked broken when it only needed
            // another try.
            registering = session_error.find("WaitingForServerToRegister") !=
                          std::string::npos;
            bool console_not_ready =
                registering ||
                session_error.find("AgentCommandError") != std::string::npos;
            if (session_error.empty()) {
                if (!home || ++readiness_retries > max_readiness_retries) {
                    fail("Timed out waiting for a session");
                    return;
                }
                registering = true;
                retrying_transport = false;
                log("xhome session polling timed out; retrying readiness " +
                    std::to_string(readiness_retries) + "/" +
                    std::to_string(max_readiness_retries));
                continue;
            }
            log("session #" + std::to_string(total_attempts) +
                " failed: " + session_error);
            if (console_not_ready) ++readiness_retries;
            if (!console_not_ready ||
                readiness_retries > max_readiness_retries) {
                fail(console_not_ready
                         ? "Your console never finished registering for remote "
                           "play. Turn Remote Features off and on, or restart "
                           "the console, then try again."
                         : "Session failed: " + session_error);
                return;
            }
            retrying_transport = false;
        }
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

bool Engine::run_peer(GssvSession& session) {
    state_ = EngineState::Negotiating;
    set_status("Negotiating connection...");

    PeerConfiguration config{};
    config.ice_servers[0].urls = "stun:stun.l.google.com:19302";
    config.audio_codec = CODEC_OPUS;
    config.video_codec = CODEC_H264;
    config.datachannel = DATA_CHANNEL_BINARY;
    config.onvideotrack = &Engine::on_video;
    config.onaudiotrack = &Engine::on_audio;
    config.user_data = this;

    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_ = peer_connection_create(&config);
        if (!peer_) {
            log("peer connection allocation failed; requesting a fresh session");
            set_status("Media setup failed - retrying...");
            return false;
        }
        peer_connection_oniceconnectionstatechange(peer_,
                                                   &Engine::on_state_change);
        // NOTE: the client must open these channels, but libpeer can only send
        // the DATA_CHANNEL_OPEN once the SCTP association is up. We therefore
        // defer creation until on_channel_open (SCTP connected) fires -- see
        // open_data_channels() in the negotiation loop below.
        peer_connection_ondatachannel(peer_, &Engine::on_channel_message,
                                      &Engine::on_channel_open, nullptr);
    }

    const char* offer = nullptr;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        offer = peer_connection_create_offer(peer_);
    }
    if (!offer) {
        log("SDP offer creation failed; requesting a fresh session");
        set_status("Video negotiation failed - retrying...");
        return false;
    }
    log("local offer created (" + std::to_string(std::strlen(offer)) +
        " bytes)");

    // The base offer now matches the known-good native client's template
    // exactly (recvonly, PT 102, full fmtp, goog-remb/fir, stereo opus).
    // No b=AS/TIAS lines: working clients don't send them; the bitrate cap is
    // declared via clientdevicecapabilities.maxBitrateKbps instead.
    std::string munged = sdp_force_stereo(offer);  // no-op safety net
    bool home = !home_server_id_.empty();
    if (home) {
        // Keep the known-good Remote Play offer at 720p by default. The beta
        // 1080p path changes only media capabilities after the Android session
        // was accepted: 8160 macroblocks, 1080p60 throughput and H.264 level
        // 4.2. If no video arrives, the worker recreates the session at 720p.
        if (media_tier_ != QualityTier::P720)
            munged = sdp_scale_video_caps_1080(munged);
        size_t at = munged.find("profile-level-id=42e01f");
        if (at != std::string::npos)
            munged.replace(at + 17, 6,
                           media_tier_ == QualityTier::P720 ? "42e020"
                                                            : "42e02a");
        log(std::string("home media request: ") +
            (media_tier_ == QualityTier::P720 ? "1280x720 stable"
                                               : "1920x1080 experimental"));
        log("home offer sdp:\n" + munged);
    } else if (media_tier_ != QualityTier::P720) {
        // 720p tier ships the template verbatim (proven accepted); 1080p
        // tiers scale the declared decode capability to 1080p60.
        munged = sdp_scale_video_caps_1080(munged);
    }
    // Pass the answer to libpeer VERBATIM. Never rewrite it: the server has
    // already chosen the codec, and any reserialization risks corrupting the
    // CRLF line endings, which would make libpeer parse the ICE ufrag/pwd with
    // a stray '\r' and send STUN checks with a wrong integrity key (silently
    // dropped by the server -> connection never completes).
    std::string answer;
    try {
        answer = session.exchange_sdp(munged);
    } catch (const std::exception& error) {
        log(std::string("SDP exchange failed; requesting a fresh session: ") +
            error.what());
        set_status("Xbox signaling failed - retrying...");
        return false;
    }
    log("answer received (" + std::to_string(answer.size()) + " bytes)");
    // Dump both SDPs for offline inspection of ICE/setup/codec lines.
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_) {
            std::fprintf(log_file_, "----- OFFER -----\n%s\n----- ANSWER -----\n%s\n-----\n",
                         munged.c_str(), answer.c_str());
        }
    }

    // Our candidates go to the server over /ice (they are already embedded
    // in the offer SDP too, but the official client posts them explicitly).
    try {
        std::vector<std::string> local = local_candidates_from_sdp(munged);
        std::string ufrag = ufrag_from_sdp(munged);
        log("posting " + std::to_string(local.size()) +
            " local candidates (ufrag " + ufrag + ")");
        if (!local.empty()) session.send_ice_candidates(local, ufrag);
    } catch (const std::exception& error) {
        log(std::string("local candidate post failed: ") + error.what());
    }

    // IMPORTANT: xCloud trickles its candidates via /ice, not in the answer
    // SDP — and libpeer builds candidate pairs exactly once, inside
    // set_remote_description. So collect the server's candidates FIRST.
    std::vector<std::string> remote;
    bool remote_has_real_candidate = false;
    {
        // xHome's useful Teredo/relay candidate can arrive well after the
        // priority-100 placeholder. Give consoles outside the LAN ten extra
        // seconds before declaring that no route exists.
        Uint64 gather_deadline =
            SDL_GetTicks64() + (home ? 25000 : 15000);
        bool done = false;
        int quiet_polls = 0;
        // xCloud first returns a placeholder front candidate (priority 100 on
        // 13.104.x) that never answers STUN; the REAL (Teredo) candidate can
        // trickle in seconds later. Settling for the placeholder alone makes
        // ICE fail, so keep polling until a real candidate shows up.
        auto has_real_candidate = [&remote]() {
            for (const std::string& candidate : remote)
                if (candidate_priority(candidate) > 1000) return true;
            return false;
        };
        while (!quit_ && !done && SDL_GetTicks64() < gather_deadline) {
            size_t before = remote.size();
            try {
                for (std::string& candidate :
                     session.receive_ice_candidates(&done))
                    remote.push_back(std::move(candidate));
            } catch (const std::exception& error) {
                log(std::string("ice poll failed: ") + error.what());
            }
            quiet_polls = remote.size() == before ? quiet_polls + 1 : 0;
            // No end marker but candidates stopped coming: assume complete --
            // but never settle while all we have is the dead placeholder.
            if (!remote.empty() && has_real_candidate() && quiet_polls >= 4)
                break;
            if (!done)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        remote_has_real_candidate = has_real_candidate();
    }
    // libpeer checks pairs in insertion order. Put the public/Teredo route
    // before xHome's priority-100 placeholder so an Internet connection does
    // not burn several STUN timeouts on a known dead endpoint first.
    std::stable_sort(remote.begin(), remote.end(),
                     [](const std::string& a, const std::string& b) {
                         return candidate_priority(a) > candidate_priority(b);
                     });
    log("collected " + std::to_string(remote.size()) + " remote candidates");
    for (const std::string& candidate : remote) log("  remote cand: " + candidate);
    for (const std::string& candidate : local_candidates_from_sdp(munged))
        log("  local  cand: " + candidate);
    if (remote.empty() || (home && !remote_has_real_candidate)) {
        log(remote.empty()
                ? (home
                       ? "xhome returned no remote ICE candidates; fresh route needed"
                       : "xCloud returned no remote ICE candidates")
                : "xhome returned only the non-routable placeholder candidate; "
                  "fresh route needed");
        set_status(home ? "No Xbox route yet - retrying..."
                        : "No server route yet - retrying...");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        for (const std::string& candidate : remote)
            peer_connection_add_ice_candidate(
                peer_, const_cast<char*>(candidate.c_str()));
        // Builds pairs from every remote candidate above, then -> CHECKING.
        peer_connection_set_remote_description(peer_, answer.c_str(),
                                               SDP_TYPE_ANSWER);
    }
    log("remote description set, checking connectivity");

    // GSSV keepalive is a blocking HTTPS request (timeout as high as 15 s).
    // It must never run on this thread: the loop below is also the sole
    // libpeer socket pump, and pausing it lets the Switch's UDP receive queue
    // overflow -- in practice a video/audio hitch followed by a PLI almost
    // exactly every 15 seconds. Run it on its own thread; after signaling
    // completes nothing else touches `session` until run_peer returns, and
    // the RAII joiner covers every return path (a destroyed joinable thread
    // would std::terminate).
    std::atomic<bool> keepalive_stop{false};
    std::thread keepalive_thread([this, &session, &keepalive_stop] {
        Uint64 next = SDL_GetTicks64() + 15000;
        Uint64 next_flush = SDL_GetTicks64() + 2000;
        while (!quit_ && !keepalive_stop) {
            Uint64 now = SDL_GetTicks64();
            // The buffered log (setvbuf in start_common) reaches the card only
            // on fclose/fail -- an app killed from HOME or a slept console
            // loses the whole session. Flushing here keeps SD latency off the
            // socket-pump thread and caps the loss at ~2 s of tail.
            if (now >= next_flush) {
                next_flush = now + 2000;
                std::lock_guard<std::mutex> lock(log_mutex_);
                if (log_file_) std::fflush(log_file_);
            }
            if (now < next) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min<Uint64>(100, next - now)));
                continue;
            }
            Uint64 started = now;
            try {
                session.keepalive();
            } catch (const std::exception& error) {
                if (!quit_ && !keepalive_stop)
                    log(std::string("keepalive failed: ") + error.what());
            }
            Uint64 elapsed = SDL_GetTicks64() - started;
            if (elapsed >= 100)
                log("keepalive took " + std::to_string(elapsed) +
                    "ms (off media thread)");
            next = SDL_GetTicks64() + 15000;
        }
    });
    struct KeepaliveJoiner {
        std::atomic<bool>& stop;
        std::thread& thread;
        ~KeepaliveJoiner() {
            stop = true;
            if (thread.joinable()) thread.join();
        }
    } keepalive_joiner{keepalive_stop, keepalive_thread};

    Uint64 ice_connected_at = 0;  // when peer state first reached connected
    Uint64 last_rr = SDL_GetTicks64();
    Uint64 last_consent = SDL_GetTicks64();
    Uint64 last_audio_stats = SDL_GetTicks64();
    Uint64 prev_audio_time = SDL_GetTicks64();
    uint32_t prev_audio_frames = 0;
    uint32_t prev_audio_out = 0;
    uint64_t prev_hud_bytes = 0;               // HUD bitrate window (video bytes)
    Uint64 prev_hud_time = SDL_GetTicks64();
    Uint64 idr_wait_start = 0;
    Uint64 last_idr_wait_log = 0;
    Uint64 negotiation_started = SDL_GetTicks64();
    Uint64 last_loop_tick = SDL_GetTicks64();  // detects a suspended app
    bool opened_channels = false;
    bool sent_handshake = false;
    PeerConnectionState last_logged_state = PEER_CONNECTION_NEW;

    while (!quit_) {
        // Drain all packets ready on the socket this cycle. Video at 1080p is
        // ~2500 packets/s; processing one-per-iteration-then-sleeping dropped
        // most of them (socket-buffer overflow) and wrecked the video. We drain
        // in bounded batches so the render thread can still grab peer_mutex_ to
        // send input between batches, and only sleep when the socket is idle.
        bool drained_any = false;
        {
            std::lock_guard<std::mutex> lock(peer_mutex_);
            for (int i = 0; peer_ && i < 64; ++i) {
                if (peer_connection_loop(peer_) > 0)
                    drained_any = true;
                else
                    break;  // socket empty (select timed out) -> stop draining
            }
        }

        Uint64 now = SDL_GetTicks64();
        PeerConnectionState current = peer_state_;
        if (current != last_logged_state) {
            last_logged_state = current;
            log(std::string("peer state: ") +
                peer_connection_state_to_string(current));
        }

        // channels_open_ is set from libpeer's SCTP onopen (association up).
        // Only now can DATA_CHANNEL_OPEN be sent; open our channels with
        // distinct even (DTLS-client) stream ids, then start the handshake.
        if (channels_open_ && !opened_channels) {
            opened_channels = true;
            open_data_channels();
            set_status("Handshaking...");
        }

        if (opened_channels && !sent_handshake) {
            sent_handshake = true;
            send_on_channel("message", xcloud::message_handshake());
        }

        if (peer_state_ == PEER_CONNECTION_FAILED) {
            if (!got_frame_) {
                log("WebRTC failed during negotiation; requesting a fresh session");
                set_status("WebRTC route failed - retrying...");
                return false;
            }
            fail("WebRTC connection failed");
            return true;
        }
        // The server announced the end of the session (stream stopped on the
        // console, console powered off, another client took over). Without
        // this the loop kept running against a dead peer: the last decoded
        // frame stayed on screen forever and the app never left the stream.
        if (server_ended_) {
            if (!got_frame_) {
                log("server ended the session before media started; retrying");
                set_status("Xbox ended setup early - retrying...");
                return false;
            }
            log("session ended by the server -- returning to the library");
            end_session();
            return true;
        }
        // Dead media path: ICE is up (the front-door placeholder answers
        // STUN) but DTLS/SCTP never completes -- the handshake starves on
        // CONN_EOF because nothing behind the front door talks back. Healthy
        // sessions open their channels ~1-2 s after connecting. xHome can be
        // substantially slower across NAT/Teredo, so Remote Play gets 25 s;
        // cloud keeps the tighter 12 s window.
        if (!ice_connected_at && (current == PEER_CONNECTION_CONNECTED ||
                                  current == PEER_CONNECTION_COMPLETED))
            ice_connected_at = now;
        Uint64 dtls_timeout = home ? 25000 : 12000;
        if (ice_connected_at && !channels_open_ &&
            now - ice_connected_at > dtls_timeout) {
            log("ICE connected but DTLS/SCTP never completed -- dead media path");
            set_status("Secure media channel stalled - retrying...");
            return false;
        }
        // The console accepted the Android/xhome session but did not start a
        // video track for the experimental 1080p capability set. Retry once
        // with the proven 720p media offer instead of making the user leave
        // the stream and change Settings manually.
        if (!home_server_id_.empty() &&
            media_tier_ != QualityTier::P720 && handshake_done_ &&
            !got_frame_ && now - negotiation_started > 25000) {
            log("home 1080p produced no video after 25s; falling back to 720p");
            set_status("1080p unavailable - retrying at 720p...");
            media_tier_ = QualityTier::P720;
            home_720_fallback_pending_ = true;
            return false;
        }
        if (home && media_tier_ == QualityTier::P720 && handshake_done_ &&
            !got_frame_ && now - negotiation_started > 60000) {
            log("home 720p handshake completed but no video arrived after "
                "60s; requesting another fresh session");
            set_status("Xbox sent no video - retrying...");
            return false;
        }
        Uint64 negotiation_timeout = home ? 75000 : 45000;
        if (state_ == EngineState::Negotiating &&
            SDL_GetTicks64() - negotiation_started > negotiation_timeout) {
            log("negotiation timed out after " +
                std::to_string(negotiation_timeout / 1000) +
                "s; requesting a fresh session");
            set_status("Negotiation timed out - retrying...");
            return false;
        }
        // Peer gone after it was up: libpeer's consent check timed out (the
        // console dropped off the network or was switched off without telling
        // us). Only meaningful once ICE connected -- CLOSED is also the enum's
        // zero value, so an unconnected peer must not trip this.
        if (ice_connected_at && (current == PEER_CONNECTION_CLOSED ||
                                 current == PEER_CONNECTION_DISCONNECTED)) {
            if (!got_frame_) {
                log("peer disconnected before media started; retrying");
                set_status("Xbox media route disconnected - retrying...");
                return false;
            }
            fail("Connection to the console was lost");
            return true;
        }
        // The app can be suspended mid-stream (HOME menu, console sleep):
        // every thread freezes while the wall clock keeps running, so the
        // gap is not a stall. Restart the window from the moment we resume.
        if (now - last_loop_tick > 2000)
            last_media_ticks_.store(now, std::memory_order_relaxed);
        last_loop_tick = now;
        // Media-stall watchdog. RTP stops the moment a session really ends,
        // but libpeer needs ~20 s of failed consent checks to notice, and a
        // half-open path may never close at all. Ten seconds without a single
        // video or audio packet is dead either way; end the stream instead of
        // holding a frozen frame.
        if (got_frame_) {
            Uint64 last_media = last_media_ticks_.load(std::memory_order_relaxed);
            if (last_media && now - last_media > 10000) {
                fail("Stream stalled: no video or audio for 10s");
                return true;
            }
        }

        // Until the first frame decodes, keep asking for a keyframe. xCloud may
        // start mid-GOP (only P-frames) or drop our first request; a single
        // request isn't enough. request_keyframe_locked() self-throttles to 1/s.
        if (handshake_done_ && !got_frame_) {
            std::lock_guard<std::mutex> lock(peer_mutex_);
            request_keyframe_locked();
        }

        // Make an IDR drought visible: if the jitter buffer keeps waiting for a
        // real keyframe, say so (with how long and how many PLIs went out)
        // instead of silently dropping frames.
        if (handshake_done_ && jitter_.waiting_keyframe()) {
            if (!idr_wait_start) idr_wait_start = now;
            if (now - last_idr_wait_log >= 2000 && now - idr_wait_start >= 2000) {
                last_idr_wait_log = now;
                log("waiting for IDR (" +
                    std::to_string((now - idr_wait_start) / 1000) + "s, " +
                    std::to_string(pli_sent_.load()) + " PLIs sent)");
            }
        } else {
            idr_wait_start = 0;
        }

        // Periodic RTCP Receiver Report + REMB: standard receiver etiquette
        // (loss accounting + bandwidth headroom signal).
        if (now - last_rr > 1000) {
            last_rr = now;
            uint8_t fraction;
            uint32_t cumulative, highest_ext;
            if (jitter_.report_stats(&fraction, &cumulative, &highest_ext)) {
                {
                    std::lock_guard<std::mutex> lock(peer_mutex_);
                    if (peer_) {
                        peer_connection_send_receiver_report(
                            peer_, fraction, cumulative, highest_ext, 0);
                        peer_connection_send_remb(
                            peer_,
                            static_cast<uint32_t>(
                                tier_profile(media_tier_).bitrate_kbps) *
                                1000u);
                    }
                }
#ifdef __SWITCH__
                // Feed the debug HUD: real bitrate (RTP video bytes over the
                // window), packet loss (RTCP fraction), audio buffer depth.
                uint64_t vb = video_bytes_.load(std::memory_order_relaxed);
                double dt = (now > prev_hud_time)
                                ? static_cast<double>(now - prev_hud_time)
                                : 0.0;
                float mbps = dt > 0.0 ? static_cast<double>(vb - prev_hud_bytes) *
                                            8.0 / dt / 1000.0
                                      : 0.0f;
                prev_hud_bytes = vb;
                prev_hud_time = now;
                float loss_pct = static_cast<float>(fraction) * 100.0f / 255.0f;
                dk_video_.set_net_stats(mbps, loss_pct, audio_.stats().queue_ms);
#endif
            }
        }

        // Audio pipeline telemetry: cumulative counters logged once per second
        // so a dropout shows up as its cause (loss vs. queue starvation vs.
        // decode failure) instead of a guess. Only meaningful once streaming.
        if (now - last_audio_stats > 1000) {
            auto a = audio_.stats();
            uint32_t in_hz = (now > prev_audio_time)
                ? (a.frames - prev_audio_frames) * 1000 / (now - prev_audio_time)
                : 0;
            uint32_t out_hz = (now > prev_audio_time)
                ? (a.out_samples - prev_audio_out) * 1000 / (now - prev_audio_time)
                : 0;
            prev_audio_frames = a.frames;
            prev_audio_out = a.out_samples;
            prev_audio_time = now;
            last_audio_stats = now;
            if (got_frame_) {
                size_t decode_q;
                {
                    std::lock_guard<std::mutex> lock(video_mutex_);
                    decode_q = video_queue_.size();
                }
                const auto video_stats = jitter_.stats();
                log("video| pkt=" + std::to_string(video_stats.packets) +
                    " frame=" + std::to_string(video_stats.frames) +
                    " drop=" + std::to_string(video_stats.dropped) +
                    " nack=" + std::to_string(video_stats.nacks) +
                    " resync=" + std::to_string(video_stats.resyncs) +
                    " au=" + std::to_string(video_stats.last_frame_bytes) +
                    "B decodeq=" + std::to_string(decode_q));
                log("audio| rx=" + std::to_string(a.received) +
                    " play=" + std::to_string(a.played) +
                    " fail=" + std::to_string(a.failed) +
                    " lost=" + std::to_string(a.lost) +
                    " under=" + std::to_string(a.underruns) +
                    " drop=" + std::to_string(a.dropped_ms) + "ms" +
                    " q=" + std::to_string(a.queue_ms) + "ms" +
                    " in=" + std::to_string(in_hz) + "hz" +
                    " out=" + std::to_string(out_hz) + "hz" +
                    " dev=" + std::to_string(audio_.device_hz()) + "hz" +
                    " ema=" + std::to_string(a.ema_ms) + "ms" +
                    " adj=" + std::to_string(a.adj_ppm) + "ppm");
#ifdef __SWITCH__
                // Present cadence: new/repeated flips, hold-duration buckets
                // (1/2/3/4+ refreshes), skipped frames, smooth-queue depth.
                size_t smooth_q;
                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    smooth_q = smooth_frames_.size();
                }
                log("pace| new=" + std::to_string(pace_new_.exchange(0)) +
                    " gen=" +
                    std::to_string(pace_generated_.exchange(0)) +
                    " rep=" + std::to_string(pace_repeat_.exchange(0)) +
                    " hold=" + std::to_string(pace_hold1_.exchange(0)) + "/" +
                    std::to_string(pace_hold2_.exchange(0)) + "/" +
                    std::to_string(pace_hold3_.exchange(0)) + "/" +
                    std::to_string(pace_hold4p_.exchange(0)) +
                    " skip=" + std::to_string(pace_skip_.exchange(0)) +
                    " src=" + (source_refresh_period_.load() == 2 ? "30" : "60") +
                    "fps q=" + std::to_string(smooth_q));
#endif
            }
        }

        // ICE consent freshness (RFC 7675): keep the peer's consent to send us
        // media alive. A full WebRTC stack does this every ~5s; libpeer doesn't.
        if (now - last_consent > 2000) {
            last_consent = now;
            std::lock_guard<std::mutex> lock(peer_mutex_);
            if (peer_) peer_connection_send_consent(peer_);
        }

        // Only yield when idle. While video is flowing we loop right back and
        // keep draining at full speed (the select() inside paces idle cycles).
        if (!drained_any)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;  // stop requested: a normal end, nothing to retry
}

// ---- render-thread interface ----------------------------------------------

#ifdef __SWITCH__
// Dedicated decode thread. Pops assembled access units, hardware-decodes each
// (NVTEGRA), and hands the freshest decoded surface to the render thread through
// shared_frame_. Decoding here rather than inline in pump_video keeps the main
// thread free for input and a steady vsync-paced present. Every AU is decoded in
// order (P-frames reference earlier frames); the render thread just presents
// whichever frame is latest, so intermediate frames are dropped at present time,
// never skipped at decode time.
void Engine::decode_loop() {
    while (!quit_) {
        VideoAccessUnit unit;
        {
            std::unique_lock<std::mutex> lock(video_mutex_);
            video_cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return quit_.load() || !video_queue_.empty();
            });
            if (quit_) break;
            if (video_queue_.empty()) continue;
            unit = std::move(video_queue_.front());
            video_queue_.pop_front();
        }
        if (video_.decode(unit.data.data(), unit.data.size())) {
            // H.264's RTP clock is fixed at 90 kHz: a frame delta is ~1500
            // ticks at 60 fps and ~3000 at 30 fps. Unlike packet arrival or
            // decode spacing, this value is not distorted when Wi-Fi delivers
            // several frames as a burst. Streaks debounce real source changes.
            if (have_rtp_timestamp_) {
                const uint32_t delta = unit.rtp_timestamp - last_rtp_timestamp_;
                if (delta >= 900 && delta <= 2200) {
                    ++source_fast_streak_;
                    source_slow_streak_ = 0;
                    if (source_fast_streak_ >= 8)
                        source_refresh_period_.store(1,
                                                     std::memory_order_relaxed);
                } else if (delta > 2200 && delta <= 4200) {
                    ++source_slow_streak_;
                    source_fast_streak_ = 0;
                    if (source_slow_streak_ >= 8)
                        source_refresh_period_.store(2,
                                                     std::memory_order_relaxed);
                } else {
                    // A source pause, timestamp discontinuity, or skipped
                    // encoder frame is not evidence of a cadence switch.
                    source_fast_streak_ = source_slow_streak_ = 0;
                }
            }
            last_rtp_timestamp_ = unit.rtp_timestamp;
            have_rtp_timestamp_ = true;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                ++shared_frame_seq_;
                if (pacing_ != VideoPacing::Steady) {
                    // Source order, not newest-wins: the clone refs the same
                    // NVTEGRA surface, so the hard cap below is what keeps the
                    // decoder's surface pool from starving.
                    AVFrame* queued = av_frame_clone(video_.current_frame());
                    if (queued)
                        smooth_frames_.push_back({queued, shared_frame_seq_});
                    while (smooth_frames_.size() > 4) {
                        SmoothFrame stale = smooth_frames_.front();
                        smooth_frames_.pop_front();
                        if (stale.frame) av_frame_free(&stale.frame);
                        pace_skip_.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    av_frame_unref(shared_frame_);
                    av_frame_ref(shared_frame_, video_.current_frame());
                    shared_frame_valid_ = true;
                }
            }
            if (!got_frame_) {
                got_frame_ = true;
                state_ = EngineState::Streaming;
            }
        }
        // Recover from packet loss / corrupt frames with a fresh keyframe
        // (throttled) instead of staying blocky until the next periodic IDR.
        if (video_.take_error()) request_keyframe();
    }
}
#endif

SDL_Texture* Engine::pump_video() {
#ifdef __SWITCH__
    // Present-only: decode_thread_ produces frames. Present decoded frames on
    // a STEADY software clock (59.94 Hz), not once per network frame:
    //  * Stutter: presenting on network arrival ties the flip cadence to arrival
    //    jitter, which drifts against the panel's 60 Hz -> periodic judder even
    //    on a fast link. A steady local clock decouples the two.
    //  * Green screen: re-presenting the held frame when nothing new decoded
    //    keeps a static / low-fps scene (e.g. a "syncing save" screen where
    //    xCloud nearly stops sending) from decaying to an empty surface -- which
    //    the YUV->RGB shader turns bright green.
    // The rate matches the panel's NTSC-derived 59.94 Hz in performance-counter
    // ticks (whole-millisecond deadlines quantize to an uneven 16/17 ms grid).
    // Staying at-or-under the panel rate matters: deko3d aborts (acquireImage ->
    // DkResult_Fail) if we queue frames faster than the compositor drains them.
    // We take our OWN ref of the shared frame so the decode thread can keep
    // producing without recycling the surface the GPU is still sampling.
    constexpr double kDisplayHz = 59.94;
    const double interval =
        static_cast<double>(SDL_GetPerformanceFrequency()) / kDisplayHz;
    double now = static_cast<double>(SDL_GetPerformanceCounter());
    if (dk_video_.initialized() && got_frame_ && now >= next_present_counter_) {
        AVFrame* frame = nullptr;
        AVFrame* motion_frame = nullptr;
        float motion_blend = 0.0f;
        uint64_t frame_seq = 0;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (motion_frame_) av_frame_unref(motion_frame_);
            if (pacing_ != VideoPacing::Steady) {
                uint32_t period =
                    (pacing_ == VideoPacing::Motion)
                        ? 2
                        : source_refresh_period_.load(std::memory_order_relaxed);
                bool due = !smooth_have_present_ ||
                           ++smooth_refresh_phase_ >= period;
                // >= 2 keeps one decoded frame in reserve so a late arrival
                // becomes a queue dip, not a visible repeat. If a network
                // burst left more than that reserve behind, discard one stale
                // presentation frame now; otherwise the extra latency would
                // remain for the rest of the session because source and panel
                // normally advance at the same average rate.
                if (due && smooth_frames_.size() >= 2) {
                    if (smooth_frames_.size() >= 3) {
                        SmoothFrame stale = smooth_frames_.front();
                        smooth_frames_.pop_front();
                        if (stale.frame) av_frame_free(&stale.frame);
                        pace_skip_.fetch_add(1, std::memory_order_relaxed);
                    }
                    SmoothFrame next = smooth_frames_.front();
                    smooth_frames_.pop_front();

                    // Shift present_frame_ into prev_frame_ (both are fully rendered/flushed primary frames)
                    if (prev_frame_) av_frame_unref(prev_frame_);
                    if (present_frame_ && present_frame_->data[0]) {
                        av_frame_move_ref(prev_frame_, present_frame_);
                    }

                    av_frame_unref(present_frame_);
                    av_frame_move_ref(present_frame_, next.frame);
                    av_frame_free(&next.frame);
                    frame_seq = next.seq;
                    smooth_have_present_ = true;
                    smooth_refresh_phase_ = 0;
                } else if (smooth_have_present_) {
                    frame_seq = last_present_seq_;  // hold the current frame
                }

                if (pacing_ == VideoPacing::Motion && smooth_have_present_) {
                    if (prev_frame_ && prev_frame_->data[0] && present_frame_ && present_frame_->data[0]) {
                        frame = prev_frame_;
                        if (smooth_refresh_phase_ == 1) {
                            if (motion_frame_ && av_frame_ref(motion_frame_, present_frame_) == 0) {
                                motion_frame = motion_frame_;
                                motion_blend = 0.5f;
                            }
                        }
                    } else {
                        frame = present_frame_;
                    }
                } else if (smooth_have_present_) {
                    frame = present_frame_;
                }
            } else if (shared_frame_valid_) {
                av_frame_unref(present_frame_);
                if (av_frame_ref(present_frame_, shared_frame_) == 0)
                    frame = present_frame_;
                frame_seq = shared_frame_seq_;
            }
        }
        if (frame) {
            // Hold accounting for the pace| line: how many refreshes the
            // previous frame stayed up (2/2/2... = perfect 30 fps cadence).
            if (frame_seq != last_present_seq_) {
                pace_new_.fetch_add(1, std::memory_order_relaxed);
                if (last_present_seq_) {
                    if (present_hold_refreshes_ == 1)
                        pace_hold1_.fetch_add(1, std::memory_order_relaxed);
                    else if (present_hold_refreshes_ == 2)
                        pace_hold2_.fetch_add(1, std::memory_order_relaxed);
                    else if (present_hold_refreshes_ == 3)
                        pace_hold3_.fetch_add(1, std::memory_order_relaxed);
                    else
                        pace_hold4p_.fetch_add(1, std::memory_order_relaxed);
                    if (frame_seq > last_present_seq_ + 1)
                        pace_skip_.fetch_add(
                            static_cast<uint32_t>(frame_seq -
                                                  last_present_seq_ - 1),
                            std::memory_order_relaxed);
                }
                last_present_seq_ = frame_seq;
                present_hold_refreshes_ = 1;
            } else if (motion_frame) {
                pace_generated_.fetch_add(1, std::memory_order_relaxed);
                ++present_hold_refreshes_;
            } else {
                pace_repeat_.fetch_add(1, std::memory_order_relaxed);
                ++present_hold_refreshes_;
            }
            dk_video_.render(frame, motion_frame, motion_blend);
        }
        next_present_counter_ += interval;
        if (next_present_counter_ < now)
            next_present_counter_ = now + interval;
    }
    return nullptr;
#else
    // PC: no decode thread (SDL texture upload must stay on this render thread),
    // so decode inline and hand back the SDL texture.
    for (;;) {
        VideoAccessUnit unit;
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            if (video_queue_.empty()) break;
            unit = std::move(video_queue_.front());
            video_queue_.pop_front();
        }
        if (video_.decode(unit.data.data(), unit.data.size()) && !got_frame_) {
            got_frame_ = true;
            state_ = EngineState::Streaming;
        }
    }
    if (video_.take_error()) request_keyframe();
    return got_frame_ ? video_.texture() : nullptr;
#endif
}

bool Engine::begin_deko_output() {
#ifdef __SWITCH__
    dk_video_.set_logger([this](const char* m) { log(std::string(m)); });
    dk_video_.set_quick_menu_state(quick_menu_state_);
    bool ok = dk_video_.init();
    log(ok ? "deko3d output started" : "deko3d output FAILED to start");
    return ok;
#else
    return false;
#endif
}

void Engine::set_quick_menu_state(const QuickMenuState& state) {
    QuickMenuState next = normalized_quick_menu(state);
    set_pacing(static_cast<VideoPacing>(next.pacing));
    quick_menu_state_ = next;
#ifdef __SWITCH__
    dk_video_.set_quick_menu_state(quick_menu_state_);
#endif
}

void Engine::set_pacing(VideoPacing pacing) {
    if (pacing != VideoPacing::Steady && pacing != VideoPacing::Smooth &&
        pacing != VideoPacing::Motion)
        pacing = VideoPacing::Steady;
#ifdef __SWITCH__
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (pacing_ == pacing) return;

    // Preserve the frame currently on screen, but release every queued or
    // generated surface owned by the old mode. This is especially important
    // when leaving Motion: retaining its second NVDEC surface can make the next
    // shader pass sample a recycled buffer and flash green.
    if (prev_frame_) av_frame_unref(prev_frame_);
    if (motion_frame_) av_frame_unref(motion_frame_);
    for (SmoothFrame& queued : smooth_frames_)
        if (queued.frame) av_frame_free(&queued.frame);
    smooth_frames_.clear();
    smooth_refresh_phase_ = 0;

    if (pacing == VideoPacing::Steady) {
        shared_frame_valid_ = false;
        if (shared_frame_) av_frame_unref(shared_frame_);
        if (shared_frame_ && present_frame_ && present_frame_->data[0] &&
            av_frame_ref(shared_frame_, present_frame_) == 0) {
            shared_frame_valid_ = true;
            shared_frame_seq_ = last_present_seq_;
        }
        smooth_have_present_ = false;
    } else {
        // Smooth/Motion may keep displaying the stable current frame while the
        // decode thread builds a fresh two-frame reserve for the new mode.
        smooth_have_present_ =
            present_frame_ && present_frame_->data[0] != nullptr;
        if (shared_frame_) av_frame_unref(shared_frame_);
        shared_frame_valid_ = false;
    }
    pacing_ = pacing;
#else
    pacing_ = pacing;
#endif
    log(std::string("pacing changed live: ") + pacing_name(pacing));
}

void Engine::set_sharpness(int level) {
    QuickMenuState next = quick_menu_state_;
    next.sharpness = level;
    set_quick_menu_state(next);
}

void Engine::set_debug_hud(bool enabled) {
    QuickMenuState next = quick_menu_state_;
    next.performance = enabled;
    set_quick_menu_state(next);
}

void Engine::end_deko_output() {
#ifdef __SWITCH__
    dk_video_.shutdown();
    log("deko3d output stopped");
#endif
}

void Engine::set_guide_button_pressed(bool pressed) {
#ifdef __SWITCH__
    dk_video_.set_guide_pressed(pressed);
#else
    (void)pressed;
#endif
}

void Engine::send_gamepad(const xcloud::GamepadFrame& frame) {
    if (!handshake_done_) return;
    // Once the peer is gone every send fails inside libpeer and logs an error;
    // at 125 Hz that filled the SD log with "sctp not connected" until the app
    // was killed. Nothing to send input to anyway.
    PeerConnectionState peer_state = peer_state_;
    if (peer_state != PEER_CONNECTION_CONNECTED &&
        peer_state != PEER_CONNECTION_COMPLETED)
        return;
    std::vector<uint8_t> packet;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        packet = input_.gamepad_packet(
            frame, static_cast<double>(SDL_GetTicks64() - stream_epoch_));
    }
    send_binary_on_channel("input", packet);
}

void Engine::request_keyframe() {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    request_keyframe_locked();
}

// Caller must hold peer_mutex_ (used from on_video, which runs under it).
void Engine::request_keyframe_locked() {
    if (!handshake_done_ || !peer_) return;
    // Throttle: at most one request per second.
    Uint64 now = SDL_GetTicks64();
    if (now - last_keyframe_req_.load() < 1000) return;
    last_keyframe_req_ = now;
    pli_sent_++;
    peer_connection_request_keyframe(peer_);  // RTCP PLI (the one xCloud honors)
    send_on_channel_locked("control", xcloud::video_keyframe_requested());
}

}  // namespace gnx::stream
