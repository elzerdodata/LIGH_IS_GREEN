#pragma once

#include <optional>
#include <string>
#include <vector>

#include "auth.hpp"
#include "http.hpp"

namespace gnx {

// Stream quality tier, selected via the device fingerprint the GSSV backend
// uses to pick resolution/bitrate caps (finding from better-xcloud):
//   android -> 720p; windows -> the most compatible 1080p pool; tizen can
//   expose a higher TV bitrate but may use a slower allocation pool. v0.6 keeps
//   the Tizen fingerprint as an explicit experiment instead of using it for
//   the normal high-bitrate preset.
enum class QualityTier { P720, P1080, P1080HQ, P1080HQTizen };

enum class SessionState {
    New,
    Provisioning,
    WaitingForResources,
    ReadyToConnect,
    Provisioned,
    Failed,
};

// Sanitized subset of the session /configuration response used by the native
// WebRTC stack. Credentials (including any TURN data) are intentionally not
// retained here.
struct StreamEndpoint {
    std::string address;
    int port = 0;
};

struct StreamConfiguration {
    bool fetched = false;
    std::vector<std::string> stun_servers;
    std::vector<StreamEndpoint> direct_endpoints;
};

// One xCloud streaming session: create -> poll state -> connect (passport
// token) -> SDP offer/answer -> ICE exchange -> keepalive loop.
class GssvSession {
public:
    GssvSession(Http& http, EndpointCredentials credentials,
                QualityTier tier = QualityTier::P1080HQ,
                std::string locale = "en-US");

    // POST /v5/sessions/cloud/play for a title.
    void start_cloud(const std::string& title_id, bool force_region = true);

    // POST /v5/sessions/home/play against your own console (remote play).
    // Use with the xhome offering's credentials, not the cloud ones.
    void start_home(const std::string& server_id);

    SessionState refresh_state();

    // Best-effort GET /{sessionPath}/configuration. Returns an empty,
    // fetched=false result if the endpoint or payload is unavailable so the
    // caller can retain its legacy STUN/ICE path.
    StreamConfiguration fetch_stream_configuration();

    // Best-effort queue estimate for a cloud title. The wait-time endpoint is
    // advisory and may be unavailable, so callers must keep polling the
    // session normally when no estimate is returned.
    std::optional<int> fetch_wait_time(const std::string& title_id);

    SessionState state() const { return state_; }
    const std::string& error_details() const { return error_details_; }

    // Must be called once when state reaches ReadyToConnect.
    void connect(const std::string& passport_token);

    // Sends our SDP offer, blocks (polling) until the server's answer arrives.
    std::string exchange_sdp(const std::string& offer_sdp, int max_bitrate_kbps = 0);

    // candidates: raw "candidate:..." strings; ufrag is our local ice-ufrag.
    void send_ice_candidates(const std::vector<std::string>& candidates,
                             const std::string& ufrag);
    // Polls once; empty vector = nothing yet. Sets *end_of_candidates when
    // the server signalled it has no more candidates coming.
    std::vector<std::string> receive_ice_candidates(
        bool* end_of_candidates = nullptr);

    void keepalive();
    void stop();

    // Closes any stale sessions left on the account (server allows only one).
    // platform: "cloud" (xCloud) or "home" (own-console remote play).
    static void cleanup_stale_sessions(Http& http,
                                       const EndpointCredentials& credentials,
                                       const std::string& platform = "cloud");

private:
    std::string url(const std::string& suffix) const;
    std::vector<std::string> headers() const;

    Http& http_;
    EndpointCredentials credentials_;
    QualityTier tier_;
    std::string locale_;  // BCP-47 sent as the streamed console's system language
    std::string session_path_;
    SessionState state_ = SessionState::New;
    std::string error_details_;
};

// Force stereo Opus in the offer SDP (useinbandfec=1 -> ...;stereo=1).
std::string sdp_force_stereo(const std::string& sdp);
// Scale the offered H264 decode caps (max-fs/max-mbps) from 720p to 1080p60.
std::string sdp_scale_video_caps_1080(const std::string& sdp);

}  // namespace gnx
