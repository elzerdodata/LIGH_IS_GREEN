#include "auth.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "../../vendor/json.hpp"

using nlohmann::json;

namespace gnx {

namespace {

// Public client id used by open xCloud clients for the device-code flow.
constexpr const char* kClientId = "1f907974-e22b-4810-a9de-d9647380c97e";
constexpr const char* kScope = "xboxlive.signin openid profile offline_access";
constexpr const char* kDeviceCodeUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char* kTokenUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";

json parse_json(const HttpResponse& response, const char* label) {
    json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_discarded())
        throw std::runtime_error(std::string(label) + ": invalid JSON: " +
                                 response.body.substr(0, 300));
    return parsed;
}

json expect_ok(const HttpResponse& response, const char* label) {
    if (!response.ok())
        throw std::runtime_error(std::string(label) + " failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 300));
    return parse_json(response, label);
}

const std::vector<std::string> kFormHeaders = {
    "Content-Type: application/x-www-form-urlencoded"};
const std::vector<std::string> kXblHeaders = {
    "Content-Type: application/json", "x-xbl-contract-version: 1"};

std::string normalized_region_name(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value)
        if (std::isalnum(ch))
            normalized.push_back(static_cast<char>(std::toupper(ch)));
    return normalized;
}

}  // namespace

XboxAuth::XboxAuth(std::string token_store_path)
    : store_path_(std::move(token_store_path)) {
    load_refresh_token();
}

void XboxAuth::logout() {
    refresh_token_.clear();
    std::remove(store_path_.c_str());
}

void XboxAuth::set_token_store(std::string path) {
    store_path_ = std::move(path);
    refresh_token_.clear();
    load_refresh_token();
}

void XboxAuth::set_preferred_server_region(std::string region_name) {
    std::lock_guard<std::mutex> lock(region_mutex_);
    preferred_server_region_ = std::move(region_name);
}

std::vector<ServerRegion> XboxAuth::available_cloud_regions() const {
    std::lock_guard<std::mutex> lock(region_mutex_);
    return available_cloud_regions_;
}

void XboxAuth::save_refresh_token(const std::string& token) {
    refresh_token_ = token;
    std::ofstream out(store_path_, std::ios::trunc);
    out << json{{"refresh_token", token}}.dump(2);
}

void XboxAuth::load_refresh_token() {
    std::ifstream in(store_path_);
    if (!in) return;
    json data = json::parse(in, nullptr, false);
    if (!data.is_discarded())
        refresh_token_ = data.value("refresh_token", "");
}

DeviceCode XboxAuth::request_device_code() {
    std::string body = std::string("client_id=") + kClientId +
                       "&scope=" + Http::urlencode(kScope);
    json response =
        expect_ok(http_.post(kDeviceCodeUrl, body, kFormHeaders), "device code");

    DeviceCode code;
    code.user_code = response.at("user_code");
    code.device_code = response.at("device_code");
    code.verification_uri = response.at("verification_uri");
    code.message = response.value("message", "");
    code.interval_secs = response.value("interval", 5);
    code.expires_in_secs = response.value("expires_in", 900);
    return code;
}

PollResult XboxAuth::poll_device_code(const DeviceCode& code) {
    std::string body =
        std::string(
            "grant_type=urn:ietf:params:oauth:grant-type:device_code") +
        "&client_id=" + kClientId + "&device_code=" + code.device_code;
    HttpResponse response = http_.post(kTokenUrl, body, kFormHeaders);

    if (!response.ok()) {
        json error = parse_json(response, "device code poll");
        std::string kind = error.value("error", "");
        if (kind == "authorization_pending" || kind == "slow_down")
            return PollResult::Pending;
        if (kind == "expired_token" || kind == "bad_verification_code")
            return PollResult::Expired;
        throw std::runtime_error("device code poll rejected: " +
                                 error.value("error_description", kind));
    }

    json token = parse_json(response, "device code poll");
    save_refresh_token(token.at("refresh_token"));
    return PollResult::Authorized;
}

std::string XboxAuth::refresh_user_token() {
    if (refresh_token_.empty())
        throw std::runtime_error("not signed in — run login first");

    std::string body = std::string("client_id=") + kClientId +
                       "&grant_type=refresh_token&refresh_token=" +
                       refresh_token_ + "&scope=" + Http::urlencode(kScope);
    HttpResponse response = http_.post(kTokenUrl, body, kFormHeaders);
    if (!response.ok()) {
        json error = parse_json(response, "token refresh");
        if (error.value("error", "") == "invalid_grant") {
            logout();
            throw std::runtime_error(
                "saved login expired; please sign in again");
        }
        throw std::runtime_error("token refresh failed: " +
                                 response.body.substr(0, 300));
    }

    json token = parse_json(response, "token refresh");
    save_refresh_token(token.at("refresh_token"));
    return token.at("access_token");
}

std::string XboxAuth::xsts_user_authenticate(const std::string& access_token) {
    json body = {
        {"Properties",
         {{"AuthMethod", "RPS"},
          {"RpsTicket", "d=" + access_token},
          {"SiteName", "user.auth.xboxlive.com"}}},
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType", "JWT"},
    };
    json response = expect_ok(
        http_.post("https://user.auth.xboxlive.com/user/authenticate",
                   body.dump(), kXblHeaders),
        "XSTS user authenticate");
    return response.at("Token");
}

std::pair<std::string, std::string> XboxAuth::xsts_authorize(
    const std::string& user_token, const std::string& relying_party) {
    json body = {
        {"Properties",
         {{"SandboxId", "RETAIL"}, {"UserTokens", {user_token}}}},
        {"RelyingParty", relying_party},
        {"TokenType", "JWT"},
    };
    json response = expect_ok(
        http_.post("https://xsts.auth.xboxlive.com/xsts/authorize",
                   body.dump(), kXblHeaders),
        "XSTS authorize");

    std::string uhs;
    if (response.contains("DisplayClaims")) {
        const json& xui = response["DisplayClaims"].value("xui", json::array());
        if (!xui.empty()) uhs = xui.front().value("uhs", "");
    }
    return {response.at("Token"), uhs};
}

EndpointCredentials XboxAuth::streaming_login(const std::string& gssv_token,
                                              const std::string& offering,
                                              const std::string& preferred_region) {
    json body = {{"token", gssv_token}, {"offeringId", offering}};
    json response = expect_ok(
        http_.post("https://" + offering +
                       ".gssv-play-prod.xboxlive.com/v2/login/user",
                   body.dump(),
                   {"Content-Type: application/json",
                    "x-gssv-client: XboxComBrowser"}),
        ("streaming login " + offering).c_str());

    EndpointCredentials credentials;
    credentials.token = response.at("gsToken");
    const std::string preferred = normalized_region_name(preferred_region);
    const ServerRegion* selected = nullptr;
    for (const json& region :
         response.at("offeringSettings").value("regions", json::array())) {
        ServerRegion candidate;
        candidate.name = region.value("name", "");
        candidate.base_uri = region.value("baseUri", "");
        candidate.is_default = region.value("isDefault", false);
        if (candidate.name.empty() || candidate.base_uri.empty()) continue;
        credentials.regions.push_back(std::move(candidate));
    }

    // Vector growth is complete, so pointers into it are now stable.
    if (!preferred.empty())
        for (const ServerRegion& region : credentials.regions)
            if (normalized_region_name(region.name) == preferred) {
                selected = &region;
                break;
            }
    if (!selected)
        for (const ServerRegion& region : credentials.regions)
            if (region.is_default) {
                selected = &region;
                break;
            }
    // A missing isDefault flag should not make the whole account unusable.
    if (!selected && !credentials.regions.empty())
        selected = &credentials.regions.front();

    if (!selected)
        throw std::runtime_error("streaming login " + offering +
                                 ": no available region");
    credentials.host = selected->base_uri;
    credentials.selected_region = selected->name;
    return credentials;
}

StreamingCredentials XboxAuth::fetch_streaming_credentials() {
    std::string access_token = refresh_user_token();
    std::string user_token = xsts_user_authenticate(access_token);
    auto [gssv_token, uhs] =
        xsts_authorize(user_token, "http://gssv.xboxlive.com/");
    (void)uhs;

    std::string preferred_region;
    {
        std::lock_guard<std::mutex> lock(region_mutex_);
        preferred_region = preferred_server_region_;
    }

    StreamingCredentials credentials;
    try {
        credentials.home = streaming_login(gssv_token, "xhome");
    } catch (const std::exception& error) {
        // No console attached to the account — cloud-only is fine. Keep the
        // reason: it is the only explanation available if the user then tries
        // to stream from a console anyway.
        credentials.home_error = error.what();
    }
    std::string cloud_error;
    try {
        credentials.cloud =
            streaming_login(gssv_token, "xgpuweb", preferred_region);
    } catch (const std::exception& error) {
        cloud_error = error.what();
    }
    std::string f2p_error;
    try {
        credentials.cloud_f2p = streaming_login(
            gssv_token, "xgpuwebf2p", preferred_region);
    } catch (const std::exception& error) {
        f2p_error = error.what();
        credentials.cloud_f2p = std::nullopt;
    }

    // Xbox permits accounts without a paid subscription to use the separate
    // F2P/owned-games offering. Previously a failed xgpuweb login aborted the
    // whole operation before xgpuwebf2p could be used.
    if (credentials.cloud.host.empty()) {
        if (credentials.cloud_f2p) {
            credentials.cloud = *credentials.cloud_f2p;
            credentials.cloud_is_f2p_fallback = true;
        } else {
            throw std::runtime_error(
                "Xbox cloud login failed (Game Pass: " + cloud_error +
                "; free-to-play/owned games: " + f2p_error + ")");
        }
    }
    {
        std::lock_guard<std::mutex> lock(region_mutex_);
        available_cloud_regions_ = credentials.cloud.regions;
    }
    return credentials;
}

std::string XboxAuth::fetch_passport_token() {
    refresh_user_token();
    if (refresh_token_.empty())
        throw std::runtime_error("not signed in");
    std::string body =
        std::string("client_id=") + kClientId +
        "&scope=service::http://Passport.NET/purpose::"
        "PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN"
        "&grant_type=refresh_token&refresh_token=" + refresh_token_;
    json response =
        expect_ok(http_.post("https://login.live.com/oauth20_token.srf", body,
                             kFormHeaders),
                  "passport token");
    return response.at("access_token");
}

XboxProfile XboxAuth::fetch_profile() {
    std::string access_token = refresh_user_token();
    std::string user_token = xsts_user_authenticate(access_token);
    auto [xbl_token, uhs] = xsts_authorize(user_token, "http://xboxlive.com");

    json response = expect_ok(
        http_.get("https://profile.xboxlive.com/users/me/profile/settings"
                  "?settings=GameDisplayPicRaw,Gamertag,Gamerscore",
                  {"x-xbl-contract-version: 3",
                   "Authorization: XBL3.0 x=" + uhs + ";" + xbl_token}),
        "profile");

    XboxProfile profile;
    for (const json& user :
         response.value("profileUsers", json::array())) {
        for (const json& setting : user.value("settings", json::array())) {
            std::string id = setting.value("id", "");
            std::string value = setting.value("value", "");
            if (id == "Gamertag") profile.gamertag = value;
            else if (id == "Gamerscore") profile.gamerscore = value;
            else if (id == "GameDisplayPicRaw") profile.avatar_url = value;
        }
    }
    return profile;
}

}  // namespace gnx
