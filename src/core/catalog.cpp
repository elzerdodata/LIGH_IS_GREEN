#include "catalog.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "../../vendor/json.hpp"

using nlohmann::json;

namespace gnx {

namespace {

// Client fingerprint the GSSV backend expects; matches what xbox.com/play sends.
constexpr const char* kDeviceInfo =
    "X-MS-Device-Info: {\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\","
    "\"clientAppType\":\"browser\",\"clientAppVersion\":\"26.1.97\","
    "\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\","
    "\"sdkInstallId\":\"\"}},\"dev\":{\"hw\":{\"make\":\"Microsoft\","
    "\"model\":\"unknown\",\"sdktype\":\"web\"},\"os\":{\"name\":\"android\","
    "\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"displayInfo\":"
    "{\"dimensions\":{\"widthInPixels\":1280,\"heightInPixels\":720},"
    "\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}},\"browser\":"
    "{\"browserName\":\"chrome\",\"browserVersion\":\"140.0.3485.54\"}}}";

std::string cover_url(std::string uri) {
    if (uri.empty()) return {};
    if (uri.rfind("//", 0) == 0)
        uri = "https:" + uri;
    else if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0)
        uri = "https://" + uri;
    uri += uri.find('?') == std::string::npos ? "?h=300" : "&h=300";
    return uri;
}

std::vector<Game> fetch_offering_titles(
    Http& http, const EndpointCredentials& endpoint,
    bool uses_f2p_offering) {
    HttpResponse response =
        http.get(endpoint.host + "/v2/titles",
                 {"Accept: application/json",
                  "Content-Type: application/json",
                  "X-Gssv-Client: XboxComBrowser", kDeviceInfo,
                  "Authorization: Bearer " + endpoint.token});
    if (!response.ok())
        throw std::runtime_error("titles request failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 300));

    json parsed = json::parse(response.body);
    const auto results = parsed.find("results");
    if (results == parsed.end() || !results->is_array()) return {};

    std::vector<Game> games;
    games.reserve(results->size());
    std::unordered_set<std::string> seen_title_ids;
    seen_title_ids.reserve(results->size());
    for (const json& entry : *results) {
        std::string title_id = entry.value("titleId", "");
        if (title_id.empty()) continue;

        const auto details_entry = entry.find("details");
        if (details_entry == entry.end() || !details_entry->is_object())
            continue;
        const json& details = *details_entry;
        // Missing entitlement is never treated as playable. The offering
        // supplies the category: xgpuweb is Game Pass; xgpuwebf2p is F2P plus
        // Stream Your Own Game/BYOG.
        if (!details.value("hasEntitlement", false)) continue;

        std::string product_id = details.value("productId", "");
        if (product_id.empty()) continue;
        if (!seen_title_ids.emplace(title_id).second) continue;

        Game game;
        game.title_id = std::move(title_id);
        game.product_id = std::move(product_id);
        game.uses_f2p_offering = uses_f2p_offering;
        game.available_on_f2p = uses_f2p_offering;
        games.push_back(std::move(game));
    }
    return games;
}

}  // namespace

std::vector<HomeConsole> fetch_home_consoles(
    Http& http, const EndpointCredentials& home) {
    HttpResponse response =
        http.get(home.host + "/v6/servers/home",
                 {"Accept: application/json",
                  "Content-Type: application/json",
                  "X-Gssv-Client: XboxComBrowser", kDeviceInfo,
                  "Authorization: Bearer " + home.token});
    if (!response.ok())
        throw std::runtime_error("console list failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 300));

    json parsed = json::parse(response.body, nullptr, false);
    std::vector<HomeConsole> consoles;
    if (parsed.is_discarded()) return consoles;
    for (const json& entry : parsed.value("results", json::array())) {
        HomeConsole console;
        console.server_id = entry.value("serverId", "");
        console.name = entry.value("serverName", "");
        console.console_type = entry.value("consoleType", "");
        console.power_state = entry.value("powerState", "");
        if (!console.server_id.empty()) consoles.push_back(std::move(console));
    }
    return consoles;
}

std::vector<Game> fetch_playable_titles(
    Http& http, const StreamingCredentials& credentials) {
    std::vector<Game> games;
    std::unordered_map<std::string, size_t> by_title_id;
    auto append_unique = [&](std::vector<Game>&& batch) {
        games.reserve(games.size() + batch.size());
        by_title_id.reserve(games.capacity());
        for (Game& game : batch) {
            auto found = by_title_id.find(game.title_id);
            if (found == by_title_id.end()) {
                by_title_id.emplace(game.title_id, games.size());
                games.push_back(std::move(game));
            } else if (game.available_on_f2p) {
                // Prefer the already-recorded regular offering for launch,
                // but do not lose membership in the alternative catalog.
                games[found->second].available_on_f2p = true;
            }
        }
    };

    // A free-only account uses xgpuwebf2p as its fallback cloud endpoint; do
    // not fetch it once as Game Pass and then again as F2P.
    if (!credentials.cloud_is_f2p_fallback)
        append_unique(fetch_offering_titles(http, credentials.cloud, false));

    if (credentials.cloud_f2p)
        append_unique(fetch_offering_titles(http, *credentials.cloud_f2p,
                                            true));
    else if (credentials.cloud_is_f2p_fallback)
        append_unique(fetch_offering_titles(http, credentials.cloud, true));

    std::sort(games.begin(), games.end(),
              [](const Game& a, const Game& b) { return a.title_id < b.title_id; });
    return games;
}

void fetch_names(Http& http, std::vector<Game>& games,
                 const std::string& market, const std::string& language) {
    // displaycatalog supplies metadata only; it never expands the already
    // entitlement-filtered title list. Keep requests batched for Switch latency.
    constexpr size_t kBatch = 50;
    std::unordered_map<std::string, std::vector<Game*>> by_product;
    for (Game& game : games)
        if (!game.product_id.empty())
            by_product[game.product_id].push_back(&game);

    std::vector<std::string> ids;
    ids.reserve(by_product.size());
    for (const auto& entry : by_product) ids.push_back(entry.first);

    for (size_t start = 0; start < ids.size(); start += kBatch) {
        std::string big_ids;
        big_ids.reserve(kBatch * 14);
        for (size_t i = start; i < std::min(start + kBatch, ids.size()); ++i) {
            if (!big_ids.empty()) big_ids += ",";
            big_ids += ids[i];
        }
        std::string url =
            "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=" +
            big_ids + "&market=" + market + "&languages=" + language +
            "&fieldsTemplate=Details";

        HttpResponse response = http.get(url);
        if (!response.ok()) continue;  // metadata is best-effort
        json parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) continue;

        for (const json& product : parsed.value("Products", json::array())) {
            std::string product_id = product.value("ProductId", "");
            auto found = by_product.find(product_id);
            if (found == by_product.end()) continue;

            const json localized =
                product.value("LocalizedProperties", json::array());
            if (localized.empty()) continue;
            const json& properties = localized.front();
            const std::string name = properties.value("ProductTitle", "");

            std::string poster, box_art, promotional_square;
            for (const json& image :
                 properties.value("Images", json::array())) {
                std::string purpose = image.value("ImagePurpose", "");
                if (purpose == "Poster") poster = image.value("Uri", "");
                else if (purpose == "BoxArt") box_art = image.value("Uri", "");
                else if (purpose == "FeaturePromotionalSquareArt")
                    promotional_square = image.value("Uri", "");
            }
            // The library is a square, console-style grid. Xbox normally
            // supplies native 1:1 BoxArt (2160x2160), while Poster is 2:3 and
            // forces either cropping or letterbox bands. Prefer genuine square
            // assets and retain Poster only as a compatibility fallback.
            const std::string& uri = !box_art.empty()
                                         ? box_art
                                     : !promotional_square.empty()
                                         ? promotional_square
                                         : poster;
            const std::string image = cover_url(uri);
            for (Game* game : found->second) {
                game->name = name;
                if (!image.empty()) game->box_art_url = image;
            }
        }
    }
}

}  // namespace gnx
