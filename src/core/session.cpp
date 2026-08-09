#include "session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "../../vendor/json.hpp"

using nlohmann::json;

namespace gnx {

namespace {

const char* os_name(QualityTier tier) {
    switch (tier) {
        case QualityTier::P720: return "android";
        case QualityTier::P1080: return "windows";
        case QualityTier::P1080HQ: return "windows";
        case QualityTier::P1080HQTizen: return "tizen";
    }
    return "windows";
}

// x-ms-device-info fingerprint matching the requested tier.
std::string device_info_header(QualityTier tier) {
    json info = {
        {"appInfo",
         {{"env",
           {{"clientAppId", "www.xbox.com"},
            {"clientAppType", "browser"},
            {"clientAppVersion", "26.1.97"},
            {"clientSdkVersion", "10.3.7"},
            {"httpEnvironment", "prod"},
            {"sdkInstallId", ""}}}}},
        {"dev",
         {{"hw", {{"make", "Microsoft"}, {"model", "unknown"}, {"sdktype", "web"}}},
          {"os", {{"name", os_name(tier)}, {"ver", "22631.2715"}, {"platform", "desktop"}}},
          {"displayInfo",
           // Report the tier's real resolution like working clients do
           // (green-vita: 1280x720). A fabricated 4096x2160 risks landing in
           // an unknown-device profile server-side.
           {{"dimensions",
             {{"widthInPixels", tier == QualityTier::P720 ? 1280 : 1920},
              {"heightInPixels", tier == QualityTier::P720 ? 720 : 1080}}},
            {"pixelDensity", {{"dpiX", 1}, {"dpiY", 1}}}}},
          {"browser",
           {{"browserName", "chrome"}, {"browserVersion", "140.0.3485.54"}}}}},
    };
    return "X-MS-Device-Info: " + info.dump();
}

json parse_or_throw(const HttpResponse& response, const char* label) {
    if (!response.ok())
        throw std::runtime_error(std::string(label) + " failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 400));
    if (response.body.empty()) return json{{"status", response.status}};
    json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_discarded())
        throw std::runtime_error(std::string(label) + ": invalid JSON");
    return parsed;
}

// Home consoles attach an errorDetails object with all-null fields to
// perfectly good responses (the answer rides right alongside it); only a
// non-null field inside errorDetails is a real failure.
bool is_real_exchange_error(const json& value) {
    if (!value.contains("errorDetails") || value["errorDetails"].is_null())
        return false;
    const json& details = value["errorDetails"];
    if (!details.is_object()) return true;
    for (const auto& item : details.items())
        if (!item.value().is_null()) return true;
    return false;
}

void throw_on_exchange_error(const json& value, const char* label) {
    if (is_real_exchange_error(value))
        throw std::runtime_error(std::string(label) + ": " +
                                 value.dump().substr(0, 400));
}

// A Teredo IPv6 address (RFC 4380) embeds the node's public IPv4 address and
// UDP port, both bit-inverted. xCloud advertises its real media endpoint this
// way; decode it to a plain IPv4 host we can actually reach. Mirrors
// greenlight's teredo.ts. Xbox may send either expanded or compressed IPv6.
bool decode_teredo(const std::string& addr, std::string* ipv4, int* port) {
    auto parse_side = [](const std::string& side,
                         std::vector<unsigned>* words) {
        if (side.empty()) return true;
        size_t start = 0;
        while (start < side.size()) {
            size_t colon = side.find(':', start);
            std::string token = side.substr(start, colon - start);
            if (token.empty() || token.size() > 4) return false;
            char* end = nullptr;
            unsigned long value = std::strtoul(token.c_str(), &end, 16);
            if (!end || *end != '\0' || value > 0xFFFF) return false;
            words->push_back(static_cast<unsigned>(value));
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
        return true;
    };

    std::string normalized = addr;
    if (normalized.size() >= 2 && normalized.front() == '[' &&
        normalized.back() == ']')
        normalized = normalized.substr(1, normalized.size() - 2);
    size_t scope = normalized.find('%');
    if (scope != std::string::npos) normalized.resize(scope);

    std::vector<unsigned> left;
    std::vector<unsigned> right;
    size_t compressed = normalized.find("::");
    std::array<unsigned, 8> words{};
    if (compressed == std::string::npos) {
        if (!parse_side(normalized, &left) || left.size() != words.size())
            return false;
        std::copy(left.begin(), left.end(), words.begin());
    } else {
        // Only one "::" is valid, and it must replace at least one word.
        if (normalized.find("::", compressed + 2) != std::string::npos)
            return false;
        if (!parse_side(normalized.substr(0, compressed), &left) ||
            !parse_side(normalized.substr(compressed + 2), &right) ||
            left.size() + right.size() >= words.size())
            return false;
        std::copy(left.begin(), left.end(), words.begin());
        std::copy(right.begin(), right.end(),
                  words.begin() + (words.size() - right.size()));
    }

    // Teredo prefix is 2001:0000::/32, not merely any 2001:: address.
    if (words[0] != 0x2001 || words[1] != 0x0000) return false;

    unsigned bytes[4] = {
        (~(words[6] >> 8)) & 0xFF,
        (~words[6]) & 0xFF,
        (~(words[7] >> 8)) & 0xFF,
        (~words[7]) & 0xFF,
    };
    *port = static_cast<int>((~words[5]) & 0xFFFF);
    *ipv4 = std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
            std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
    return true;
}

bool is_ipv4_literal(const std::string& address) {
    size_t start = 0;
    for (int octet = 0; octet < 4; ++octet) {
        size_t dot = address.find('.', start);
        if ((octet < 3 && dot == std::string::npos) ||
            (octet == 3 && dot != std::string::npos))
            return false;
        size_t end = dot == std::string::npos ? address.size() : dot;
        if (end == start || end - start > 3) return false;
        unsigned value = 0;
        for (size_t i = start; i < end; ++i) {
            if (address[i] < '0' || address[i] > '9') return false;
            value = value * 10 + static_cast<unsigned>(address[i] - '0');
        }
        if (value > 255) return false;
        start = end + 1;
    }
    return start == address.size() + 1;
}

int configuration_port(const json& details, const char* key) {
    if (!details.contains(key)) return 0;
    try {
        const json& value = details[key];
        long long port = 0;
        if (value.is_number_integer() || value.is_number_unsigned()) {
            port = value.get<long long>();
        } else if (value.is_string()) {
            const std::string text = value.get<std::string>();
            char* end = nullptr;
            port = std::strtol(text.c_str(), &end, 10);
            if (!end || *end != '\0') return 0;
        }
        return port > 0 && port <= 65535 ? static_cast<int>(port) : 0;
    } catch (const std::exception&) {
        return 0;
    }
}

// libpeer's pinned STUN parser accepts IPv4/hostname URLs with an explicit
// port. Normalize the browser form and reject TURN or IPv6 URLs here; the
// Switch build has no IPv6 transport and no complete TURN relay path.
bool normalize_stun_url(const std::string& input, std::string* normalized) {
    if (input.rfind("stun:", 0) != 0) return false;
    std::string target = input.substr(5);
    size_t query = target.find('?');
    if (query != std::string::npos) target.resize(query);
    if (target.empty() || target.find_first_of("/\\ \t\r\n[]") !=
                              std::string::npos)
        return false;

    std::string host = target;
    int port = 3478;
    size_t colon = target.rfind(':');
    if (colon != std::string::npos) {
        // A second colon means an IPv6 literal, unsupported by this build.
        if (target.find(':') != colon) return false;
        host = target.substr(0, colon);
        std::string port_text = target.substr(colon + 1);
        char* end = nullptr;
        long parsed = std::strtol(port_text.c_str(), &end, 10);
        if (!end || *end != '\0' || parsed <= 0 || parsed > 65535)
            return false;
        port = static_cast<int>(parsed);
    }
    // agent_gather_candidate uses a fixed 64-byte hostname buffer.
    if (host.empty() || host.size() > 63) return false;
    *normalized = "stun:" + host + ":" + std::to_string(port);
    return true;
}

void add_configuration_endpoint(StreamConfiguration* config,
                                const std::string& address, int port) {
    if (!is_ipv4_literal(address) || port <= 0 || port > 65535) return;
    auto same_endpoint = [&](const StreamEndpoint& endpoint) {
        return endpoint.address == address && endpoint.port == port;
    };
    if (std::find_if(config->direct_endpoints.begin(),
                     config->direct_endpoints.end(), same_endpoint) ==
        config->direct_endpoints.end())
        config->direct_endpoints.push_back({address, port});
}

}  // namespace

GssvSession::GssvSession(Http& http, EndpointCredentials credentials,
                         QualityTier tier, std::string locale)
    : http_(http),
      credentials_(std::move(credentials)),
      tier_(tier),
      locale_(std::move(locale)) {}

std::string GssvSession::url(const std::string& suffix) const {
    std::string path = session_path_;
    if (!path.empty() && path.front() != '/') path = "/" + path;
    return credentials_.host + path + suffix;
}

std::vector<std::string> GssvSession::headers() const {
    return {
        "Accept: application/json",
        "Content-Type: application/json",
        "X-Gssv-Client: XboxComBrowser",
        device_info_header(tier_),
        "Authorization: Bearer " + credentials_.token,
    };
}

void GssvSession::start_cloud(const std::string& title_id, bool force_region) {
    json fallback_regions = json::array();
    if (!force_region) {
        for (const auto& r : credentials_.regions) {
            if (r.name != credentials_.selected_region && !r.name.empty()) {
                fallback_regions.push_back(r.name);
            }
        }
    }

    json body = {
        {"clientSessionId", ""},
        {"titleId", title_id},
        {"systemUpdateGroup", ""},
        {"settings",
         {{"nanoVersion", "V3;WebrtcTransport.dll"},
          {"enableOptionalDataCollection", false},
          {"enableTextToSpeech", false},
          {"highContrast", 0},
          {"locale", locale_},
          {"useIceConnection", false},
          {"timezoneOffsetMinutes", 120},
          {"sdkType", "web"},
          {"osName", os_name(tier_)}}},
        {"serverId", ""},
        {"fallbackRegionNames", fallback_regions},
    };
    json response = parse_or_throw(
        http_.post(credentials_.host + "/v5/sessions/cloud/play", body.dump(),
                   headers()),
        "session start");
    session_path_ = response.at("sessionPath");
    state_ = SessionState::New;
}

// Same request shape as start_cloud, but on the home platform: the target is
// a serverId (your console) instead of a titleId. Field values mirror
// green-vita's working home client exactly -- in particular
// useIceConnection stays false; true makes the console agent reject the
// start with AgentCommandError.
void GssvSession::start_home(const std::string& server_id) {
    json body = {
        {"clientSessionId", ""},
        {"titleId", ""},
        {"systemUpdateGroup", ""},
        {"settings",
         {{"nanoVersion", "V3;WebrtcTransport.dll"},
          {"enableOptionalDataCollection", false},
          {"enableTextToSpeech", false},
          {"highContrast", 0},
          {"locale", locale_},
          {"useIceConnection", false},
          {"timezoneOffsetMinutes", 120},
          {"sdkType", "web"},
          {"osName", os_name(tier_)}}},
        {"serverId", server_id},
        {"fallbackRegionNames", json::array()},
    };
    json response = parse_or_throw(
        http_.post(credentials_.host + "/v5/sessions/home/play", body.dump(),
                   headers()),
        "home session start");
    session_path_ = response.at("sessionPath");
    state_ = SessionState::New;
}

SessionState GssvSession::refresh_state() {
    json response = parse_or_throw(http_.get(url("/state"), headers()),
                                   "session state");
    std::string state = response.value("state", "");
    if (state == "Provisioning") state_ = SessionState::Provisioning;
    else if (state == "WaitingForResources")
        state_ = SessionState::WaitingForResources;
    else if (state == "ReadyToConnect") state_ = SessionState::ReadyToConnect;
    else if (state == "Provisioned") state_ = SessionState::Provisioned;
    else if (state == "Failed" || state == "Error") {
        state_ = SessionState::Failed;
        error_details_ = response.value("errorDetails", json::object()).dump();
    }
    return state_;
}

StreamConfiguration GssvSession::fetch_stream_configuration() {
    StreamConfiguration config;
    try {
        HttpResponse response = http_.get(url("/configuration"), headers());
        if (!response.ok() || response.body.empty()) return config;

        json parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() ||
            !parsed.contains("serverDetails"))
            return config;

        json details = parsed["serverDetails"];
        // Some service versions have serialized this inner object as a JSON
        // string. Accept both forms without exposing the response in logs.
        if (details.is_string())
            details = json::parse(details.get<std::string>(), nullptr, false);
        if (details.is_discarded() || !details.is_object()) return config;
        config.fetched = true;

        if (details.contains("stunServerAddresses") &&
            details["stunServerAddresses"].is_array()) {
            for (const json& item : details["stunServerAddresses"]) {
                if (!item.is_string()) continue;
                std::string server;
                if (!normalize_stun_url(item.get<std::string>(), &server) ||
                    std::find(config.stun_servers.begin(),
                              config.stun_servers.end(), server) !=
                        config.stun_servers.end())
                    continue;
                config.stun_servers.push_back(std::move(server));
            }
        }

        const std::array<std::pair<const char*, const char*>, 3> endpoint_keys{{
            {"ipAddress", "port"},
            {"ipV4Address", "ipV4Port"},
            {"ipV6Address", "ipV6Port"},
        }};
        for (const auto& [address_key, port_key] : endpoint_keys) {
            if (!details.contains(address_key) ||
                !details[address_key].is_string())
                continue;
            std::string address = details[address_key].get<std::string>();
            int reported_port = configuration_port(details, port_key);
            if (is_ipv4_literal(address)) {
                add_configuration_endpoint(&config, address, reported_port);
                add_configuration_endpoint(&config, address, 9002);
                continue;
            }

            // Native IPv6 cannot be used by this Switch/libpeer build. Teredo
            // is still useful because it encodes the console's public IPv4
            // and NAT-mapped UDP port.
            std::string ipv4;
            int teredo_port = 0;
            if (!decode_teredo(address, &ipv4, &teredo_port)) continue;
            add_configuration_endpoint(&config, ipv4, reported_port);
            add_configuration_endpoint(&config, ipv4, 9002);
            add_configuration_endpoint(&config, ipv4, teredo_port);
        }
    } catch (const std::exception&) {
        // Configuration improves NAT traversal but is not required by the
        // legacy signaling path; never abort a session when it is unavailable.
        return StreamConfiguration{};
    }
    return config;
}

std::optional<int> GssvSession::fetch_wait_time(
    const std::string& title_id) {
    if (title_id.empty()) return std::nullopt;
    try {
        HttpResponse response = http_.get(
            credentials_.host + "/v1/waittime/" + title_id, headers());
        if (!response.ok() || response.body.empty()) return std::nullopt;

        json parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;

        const char* keys[] = {
            "estimatedTotalWaitTimeInSeconds",
            "estimatedWaitTimeInSeconds",
            "estimatedAllocationTimeInSeconds",
            "waitTotalInSeconds",
            "waitTimeInSeconds",
            "estimatedWaitTime"
        };
        for (const char* key : keys) {
            if (parsed.contains(key)) {
                if (parsed[key].is_number()) {
                    int sec = static_cast<int>(parsed[key].get<double>());
                    if (sec >= 0) return sec;
                } else if (parsed[key].is_string()) {
                    try {
                        int sec = std::stoi(parsed[key].get<std::string>());
                        if (sec >= 0) return sec;
                    } catch (...) {}
                }
            }
        }
        return std::nullopt;
    } catch (const std::exception&) {
        // Queue information is optional. A failed estimate must never abort
        // the real session-state polling and WebRTC setup.
        return std::nullopt;
    }
}

void GssvSession::connect(const std::string& passport_token) {
    json body = {{"userToken", passport_token}};
    parse_or_throw(http_.post(url("/connect"), body.dump(), headers()),
                   "session connect");
}

std::string sdp_set_max_bitrate(const std::string& sdp, int max_kbps) {
    if (max_kbps <= 0) return sdp;
    std::string out = sdp;
    std::string b_line = "b=AS:" + std::to_string(max_kbps) + "\r\n";
    size_t video_m = out.find("m=video");
    if (video_m != std::string::npos) {
        size_t next_line = out.find("\r\n", video_m);
        if (next_line != std::string::npos) {
            out.insert(next_line + 2, b_line);
        }
    }
    return out;
}

std::string GssvSession::exchange_sdp(const std::string& offer_sdp, int max_bitrate_kbps) {
    std::string offer = sdp_set_max_bitrate(offer_sdp, max_bitrate_kbps);
    json body = {
        {"messageType", "offer"},
        {"sdp", offer},
        {"requestId", "1"},
        {"configuration",
         {{"chatConfiguration",
           {{"bytesPerSample", 2},
            {"expectedClipDurationMs", 20},
            {"format", {{"codec", "opus"}, {"container", "webm"}}},
            {"numChannels", 1},
            {"sampleFrequencyHz", 24000}}},
          {"chat", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"control", {{"minVersion", 1}, {"maxVersion", 3}}},
          {"input", {{"minVersion", 1}, {"maxVersion", 9}}},
          {"message", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"reliableinput", {{"minVersion", 9}, {"maxVersion", 9}}},
          {"unreliableinput", {{"minVersion", 9}, {"maxVersion", 9}}}}},
    };
    parse_or_throw(http_.post(url("/sdp"), body.dump(), headers()),
                   "sdp offer");

    for (int attempt = 0; attempt < 120; ++attempt) {
        json response =
            parse_or_throw(http_.get(url("/sdp"), headers()), "sdp poll");
        if (response.value("status", 0) == 204) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        throw_on_exchange_error(response, "sdp exchange");
        std::string exchange = response.value("exchangeResponse", "");
        json parsed = json::parse(exchange, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("sdp"))
            throw std::runtime_error("sdp answer missing sdp payload");
        return parsed["sdp"];
    }
    throw std::runtime_error("timed out waiting for SDP answer");
}

void GssvSession::send_ice_candidates(
    const std::vector<std::string>& candidates, const std::string& ufrag) {
    // Match the official web client / greenlight exactly: messageType
    // "iceCandidate", key "candidate" (singular) holding an array of
    // stringified candidate objects, each carrying our ice-ufrag. Getting
    // this shape wrong makes xCloud withhold its real (Teredo) candidate and
    // ignore our connectivity checks.
    json list = json::array();
    for (const std::string& candidate : candidates) {
        json entry = {{"candidate", candidate},
                      {"sdpMid", "0"},
                      {"sdpMLineIndex", 0},
                      {"usernameFragment", ufrag}};
        list.push_back(entry.dump());
    }
    // Terminate the list, as the web client does.
    list.push_back(json{{"candidate", "a=end-of-candidates"},
                        {"sdpMid", "0"},
                        {"sdpMLineIndex", 0},
                        {"usernameFragment", ufrag}}
                       .dump());
    json body = {{"messageType", "iceCandidate"}, {"candidate", list}};
    json response = parse_or_throw(
        http_.post(url("/ice"), body.dump(), headers()), "ice send");
    throw_on_exchange_error(response, "ice send");
}

std::vector<std::string> GssvSession::receive_ice_candidates(
    bool* end_of_candidates) {
    if (end_of_candidates) *end_of_candidates = false;
    json response =
        parse_or_throw(http_.get(url("/ice"), headers()), "ice poll");
    if (response.value("status", 0) == 204) return {};
    throw_on_exchange_error(response, "ice poll");

    std::string exchange = response.value("exchangeResponse", "");
    json parsed = json::parse(exchange, nullptr, false);
    if (parsed.is_discarded()) return {};

    const json* items = nullptr;
    if (parsed.is_array()) items = &parsed;
    else if (parsed.contains("candidates")) items = &parsed["candidates"];
    if (!items) return {};

    std::vector<std::string> out;
    int synthetic_foundation = 20;
    for (const json& item : *items) {
        json entry = item;
        if (entry.is_string())
            entry = json::parse(entry.get<std::string>(), nullptr, false);
        if (entry.is_discarded() || !entry.is_object()) continue;
        std::string candidate = entry.value("candidate", "");
        if (candidate.rfind("a=", 0) == 0) candidate = candidate.substr(2);
        if (candidate.find("end-of-candidates") != std::string::npos) {
            if (end_of_candidates) *end_of_candidates = true;
            continue;
        }
        if (candidate.rfind("candidate:", 0) != 0) continue;
        while (!candidate.empty() &&
               (candidate.back() == ' ' || candidate.back() == '\r'))
            candidate.pop_back();

        // Tokens: candidate:<foundation> <comp> <proto> <prio> <addr> <port> ...
        std::istringstream fields(candidate);
        std::vector<std::string> parts;
        std::string token;
        while (fields >> token) parts.push_back(token);
        if (parts.size() < 6) continue;
        const std::string& address = parts[4];

        if (address.find(':') == std::string::npos) {
            out.push_back(candidate);  // plain IPv4
            continue;
        }
        // IPv6: only usable if it's a Teredo address encoding an IPv4 endpoint.
        std::string ipv4;
        int teredo_port = 0;
        if (!decode_teredo(address, &ipv4, &teredo_port)) continue;
        // Keep the original ICE priority so the converted public route is not
        // mistaken for xHome's priority-100 front-door placeholder. A few
        // servers have emitted an unusably low priority, so give a converted
        // Teredo endpoint a conservative floor above that placeholder.
        char* priority_end = nullptr;
        unsigned long parsed_priority =
            std::strtoul(parts[3].c_str(), &priority_end, 10);
        if (!priority_end || *priority_end != '\0') parsed_priority = 0;
        unsigned long synthetic_priority =
            std::max<unsigned long>(parsed_priority, 16777215UL);

        // The working Greenlight route tries UDP 9002 first, then the port
        // encoded by Teredo. Keep that order because libpeer checks pairs in
        // insertion order, and avoid adding the same endpoint twice.
        int previous_port = -1;
        for (int port : {9002, teredo_port}) {
            if (port <= 0 || port == previous_port) continue;
            previous_port = port;
            out.push_back("candidate:" + std::to_string(synthetic_foundation++) +
                          " " + parts[1] + " " + parts[2] + " " +
                          std::to_string(synthetic_priority) + " " + ipv4 +
                          " " + std::to_string(port) + " typ host");
        }
    }
    return out;
}

void GssvSession::keepalive() {
    parse_or_throw(http_.post(url("/keepalive"), "", headers()), "keepalive");
}

void GssvSession::stop() {
    if (session_path_.empty()) return;
    try {
        http_.del(url(""), headers());
    } catch (const std::exception&) {}
}

void GssvSession::cleanup_stale_sessions(
    Http& http, const EndpointCredentials& credentials,
    const std::string& platform) {
    GssvSession probe(http, credentials,
                      platform == "home" ? QualityTier::P720
                                         : QualityTier::P1080HQ);
    try {
        json response = parse_or_throw(
            http.get(credentials.host + "/v5/sessions/" + platform + "/active",
                     probe.headers()),
            "active sessions");
        std::vector<std::string> paths;
        // Collect any "sessionPath"/"path" strings anywhere in the response.
        std::function<void(const json&)> walk = [&](const json& node) {
            if (node.is_array())
                for (const json& item : node) walk(item);
            else if (node.is_object())
                for (auto& [key, value] : node.items()) {
                    if ((key == "sessionPath" || key == "path") &&
                        value.is_string())
                        paths.push_back(value);
                    walk(value);
                }
        };
        walk(response);
        for (const std::string& path : paths) {
            std::string full = path;
            if (!full.empty() && full.front() != '/') full = "/" + full;
            try {
                http.del(credentials.host + full, probe.headers());
            } catch (const std::exception&) {}
        }
    } catch (const std::exception&) {
        // 404 = no active-session endpoint; nothing to clean.
    }
}

// ---- SDP munging ----------------------------------------------------------

std::string sdp_force_stereo(const std::string& sdp) {
    const std::string needle = "useinbandfec=1";
    std::string result = sdp;
    size_t at = result.find(needle);
    if (at != std::string::npos &&
        result.find("stereo=1") == std::string::npos)
        result.insert(at + needle.size(), ";stereo=1");
    return result;
}

std::string sdp_scale_video_caps_1080(const std::string& sdp) {
    // The base offer declares 720p decode capability (max-fs=3600 MBs,
    // max-mbps=108000). For 1080p tiers scale to 1920x1080@60: 8160 MBs,
    // 489600 MB/s. Same-length in-place replacements keep the SDP intact.
    std::string out = sdp;
    size_t at = out.find("max-fs=3600");
    if (at != std::string::npos) out.replace(at + 7, 4, "8160");
    at = out.find("max-mbps=108000");
    if (at != std::string::npos) out.replace(at + 9, 6, "489600");
    return out;
}

}  // namespace gnx
