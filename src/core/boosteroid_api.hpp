#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace ZERODROID {

struct BoosteroidUser {
    std::string id;
    std::string email;
    std::string nickname;
    std::string avatarUrl;
    bool hasActiveSubscription{false};
};

struct GameItem {
    int id{0};
    std::string title;
    std::string bannerUrl;
    std::string posterUrl;
    std::string store; // "steam", "epic", "ubisoft", etc.
    std::string platform;
    bool isInstalled{false};
    bool installAndPlay{false};
    bool isFree{false};
    bool controllerFriendly{false};
};

struct DeviceCodeAuthArtifact {
    std::string userCode;        // Optional short display code (unused by Boosteroid TV)
    std::string deviceCode;      // UUID polled by the TV client
    std::string verificationUrl; // URL opened by the phone after scanning
    std::string qrUrl;           // Full URL encoded in the QR image
    int expiresInSeconds{300};
    int intervalSeconds{3};
};

enum class DeviceCodePollResult {
    Pending,
    Succeeded,
    Expired,
    Error,
};

struct StreamGateway {
    std::string id;
    std::string host;
    int port{0};
    std::string region;
    std::string label;
};

struct ServerLocation {
    int id{0};
    std::string title;
    std::string country;
    bool active{false};
    bool available{false};
};

struct ServerPreferences {
    bool allowDistantRegions{true};
    int preferredLocationId{0};  // 0 = automatic
};

struct StreamSessionConfig {
    std::string sessionId;
    std::string homeUrl;
    // Exact, paired values returned by /streaming/session/details.  The
    // signature is valid only for the gateway assigned to this VM.
    std::string assignedGateway;
    std::string signedQuery;
    std::vector<std::string> sessionQueries;
    std::vector<StreamGateway> gateways;
    std::string accessToken;
    std::string authDataToken;
    std::string preferredCodec{"h264"};
};

class BoosteroidAPI {
public:
    BoosteroidAPI(const std::string& baseUrl = "https://cloud.boosteroid.com");
    ~BoosteroidAPI();

    // Session persistence on Switch SD card (sdmc:/switch/zerodroid/session.json)
    bool loadSessionFromSD();
    bool saveSessionToSD();

    // Android TV QR login flow.
    bool requestDeviceCode(DeviceCodeAuthArtifact& outArtifact);
    DeviceCodePollResult pollDeviceCodeStatus(
        const std::string& deviceCode,
        std::string& outAuthToken);
    const std::string& lastError() const { return m_lastError; }
    
    bool setAuthToken(const std::string& token);
    bool isAuthenticated() const;
    void logout();

    // User Profile & Subscription
    bool getUserProfile(BoosteroidUser& outUser);

    // Catalog & Library
    bool getInstalledGames(std::vector<GameItem>& outGames);
    bool getCatalogGames(std::vector<GameItem>& outGames);
    bool getBoostoreGames(const std::string& query, std::vector<GameItem>& outGames);
    bool addGameToLibrary(int appId);

    // WebRTC Streaming Launch
    bool getStreamingGateways(std::vector<StreamGateway>& outGateways);
    bool getServerConfiguration(std::vector<ServerLocation>& outLocations,
                                ServerPreferences& outPreferences);
    bool updateServerConfiguration(const ServerPreferences& preferences);
    bool startStreamingSession(int appId, StreamSessionConfig& outConfig,
                               std::atomic<bool>* cancelFlag = nullptr,
                               const std::function<void(const std::string&)>& log = {});
    bool stopStreamingSession(const std::string& sessionId);

private:
    bool startStreamingSessionImpl(int appId, StreamSessionConfig& outConfig,
                                   std::atomic<bool>* cancelFlag,
                                   bool allowRequeue,
                                   const std::function<void(const std::string&)>& log);

    std::string m_baseUrl;
    std::string m_authToken;
    std::string m_refreshToken;
    std::string m_userDataToken;
    std::string m_sessionFilePath;
    std::string m_deviceId;
    std::string m_lastError;
    BoosteroidUser m_currentUser;
};

} // namespace ZERODROID
