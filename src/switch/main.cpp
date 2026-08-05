#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <future>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>
#include "../core/boosteroid_api.hpp"
#include "../core/http.hpp"
#include "../core/qrcodegen.hpp"
#include "json.hpp"
#include "covers.hpp"
#include "gfx.hpp"
#include "stream/engine.hpp"

// Render crisp QR Code via software surface to bypass HW rect limits
void draw_qr_code(SDL_Renderer* renderer, int startX, int startY, int boxSize, const std::string& text) {
    if (text.empty() || !renderer) return;

    static SDL_Texture* cached_qr = nullptr;
    static std::string cached_text = "";

    if (cached_text != text) {
        if (cached_qr) {
            SDL_DestroyTexture(cached_qr);
            cached_qr = nullptr;
        }
        cached_text = text;

        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
            int modules = qr.getSize();
            int cellSize = boxSize / (modules + 4);

            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, boxSize, boxSize, 32, SDL_PIXELFORMAT_RGBA32);
            if (surf) {
                SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 255, 255, 255, 255));
                int marginX = (boxSize - modules * cellSize) / 2;
                int marginY = (boxSize - modules * cellSize) / 2;
                Uint32 black = SDL_MapRGBA(surf->format, 12, 10, 24, 255);
                
                for (int y = 0; y < modules; ++y) {
                    for (int x = 0; x < modules; ++x) {
                        if (qr.getModule(x, y)) {
                            SDL_Rect r = {marginX + x * cellSize, marginY + y * cellSize, cellSize, cellSize};
                            SDL_FillRect(surf, &r, black);
                        }
                    }
                }
                cached_qr = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_FreeSurface(surf);
            }
        } catch (...) {
        }
    }

    SDL_Rect dst = {startX, startY, boxSize, boxSize};
    if (cached_qr) {
        SDL_RenderCopy(renderer, cached_qr, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &dst);
    }
}

struct AsyncPollResult {
    ZERODROID::DeviceCodePollResult result{ZERODROID::DeviceCodePollResult::Error};
    std::string token;
};

struct AsyncLibraryResult {
    bool profileOk{false};
    bool installedOk{false};
    bool catalogOk{false};
    ZERODROID::BoosteroidUser user;
    std::vector<ZERODROID::GameItem> installedGames;
    std::vector<ZERODROID::GameItem> catalogGames;
    std::string installedError;
    std::string catalogError;
    std::string profileError;
};

struct AsyncGameActionResult {
    bool ok{false};
    int gameId{0};
    std::string error;
};

struct AsyncServerConfigResult {
    bool ok{false};
    std::vector<ZERODROID::ServerLocation> locations;
    ZERODROID::ServerPreferences preferences;
    std::string error;
};

struct AsyncServerSaveResult {
    bool ok{false};
    std::string error;
};

struct ServerSettings {
    bool allowDistantRegions{true};
    int preferredLocationId{0};
    std::string preferredLocationLabel{"Automatico"};
    bool xboxFaceLayout{false};
    int language{0};  // 0 = Espanol, 1 = English
    int mouseSpeed{gnx::stream::MouseNormal};
    int resolutionMode{gnx::stream::ResolutionAuto};
    int picturePreset{gnx::stream::PresetNatural};
    int brightness{0};
    int contrast{100};
    int saturation{100};
    int gamma{100};
    int sharpness{0};
    std::vector<int> favoriteGameIds;
    std::vector<int> recentGameIds;
};

enum class LibraryTab {
    MyGames = 0,
    Favorites = 1,
    Recent = 2,
    Catalog = 3,
    InstallAndPlay = 4,
};

struct VisibleGame {
    bool catalog{false};
    std::size_t index{0};
};

ServerSettings load_server_settings() {
    ServerSettings settings;
    std::ifstream file("sdmc:/switch/ZERODROID/settings.json");
    if (!file.is_open()) return settings;
    try {
        nlohmann::json value;
        file >> value;
        settings.allowDistantRegions =
            value.value("allowDistantRegions", true);
        settings.preferredLocationId =
            value.value("preferredLocationId", 0);
        settings.preferredLocationLabel = value.value(
            "preferredLocationLabel", std::string("Automatico"));
        settings.xboxFaceLayout = value.value("xboxFaceLayout", false);
        settings.language = std::clamp(value.value("language", 0), 0, 1);
        settings.mouseSpeed = std::clamp(
            value.value("mouseSpeed", gnx::stream::MouseNormal),
            gnx::stream::MousePrecise, gnx::stream::MouseFast);
        settings.resolutionMode = std::clamp(
            value.value("resolutionMode", gnx::stream::ResolutionAuto),
            gnx::stream::ResolutionAuto, gnx::stream::Resolution1440p);
        settings.picturePreset = std::clamp(
            value.value("picturePreset", gnx::stream::PresetNatural),
            gnx::stream::PresetNatural, gnx::stream::PresetCustom);
        settings.brightness = std::clamp(value.value("brightness", 0), -20, 20);
        settings.contrast = std::clamp(value.value("contrast", 100), 70, 130);
        settings.saturation = std::clamp(value.value("saturation", 100), 0, 150);
        settings.gamma = std::clamp(value.value("gamma", 100), 50, 200);
        settings.sharpness = std::clamp(value.value("sharpness", 0), 0, 3);
        settings.favoriteGameIds = value.value(
            "favoriteGameIds", std::vector<int>{});
        settings.recentGameIds = value.value(
            "recentGameIds", std::vector<int>{});
    } catch (...) {
    }
    return settings;
}

bool save_server_settings(const ServerSettings& settings) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/ZERODROID", 0777);
    std::ofstream file("sdmc:/switch/ZERODROID/settings.json");
    if (!file.is_open()) return false;
    file << nlohmann::json({
        {"version", 6},
        {"allowDistantRegions", settings.allowDistantRegions},
        {"preferredLocationId", settings.preferredLocationId},
        {"preferredLocationLabel", settings.preferredLocationLabel},
        {"xboxFaceLayout", settings.xboxFaceLayout},
        {"language", settings.language},
        {"mouseSpeed", settings.mouseSpeed},
        {"resolutionMode", settings.resolutionMode},
        {"picturePreset", settings.picturePreset},
        {"brightness", settings.brightness},
        {"contrast", settings.contrast},
        {"saturation", settings.saturation},
        {"gamma", settings.gamma},
        {"sharpness", settings.sharpness},
        {"favoriteGameIds", settings.favoriteGameIds},
        {"recentGameIds", settings.recentGameIds},
    }).dump(2);
    return file.good();
}

bool contains_game_id(const std::vector<int>& values, int gameId) {
    return std::find(values.begin(), values.end(), gameId) != values.end();
}

void toggle_game_id(std::vector<int>& values, int gameId) {
    const auto found = std::find(values.begin(), values.end(), gameId);
    if (found == values.end()) {
        values.push_back(gameId);
    } else {
        values.erase(found);
    }
}

void remember_recent_game(std::vector<int>& values, int gameId) {
    values.erase(std::remove(values.begin(), values.end(), gameId), values.end());
    values.insert(values.begin(), gameId);
    constexpr std::size_t kRecentLimit = 30;
    if (values.size() > kRecentLimit) values.resize(kRecentLimit);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string open_search_keyboard(const std::string& initial, int language) {
    SwkbdConfig keyboard{};
    char buffer[256]{};
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return initial;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetGuideText(
        &keyboard, language == 1 ? "Search your games" : "Buscar juegos");
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    const Result result = swkbdShow(&keyboard, buffer, sizeof(buffer));
    swkbdClose(&keyboard);
    return R_SUCCEEDED(result) ? std::string(buffer) : initial;
}

std::string fit_text(
    gnx::gfx::Gfx& gfx,
    const std::string& text,
    int maxWidth,
    gnx::gfx::FontSize size = gnx::gfx::FontSize::Small) {
    if (gfx.text_width(text, size) <= maxWidth) return text;

    std::string shortened = text;
    constexpr const char* suffix = "...";
    while (!shortened.empty() &&
           gfx.text_width(shortened + suffix, size) > maxWidth) {
        std::size_t previous = shortened.size() - 1;
        while (previous > 0 &&
               (static_cast<unsigned char>(shortened[previous]) & 0xc0U) == 0x80U) {
            --previous;
        }
        shortened.erase(previous);
    }
    return shortened + suffix;
}

std::vector<std::string> wrap_text_lines(
    gnx::gfx::Gfx& gfx,
    const std::string& text,
    int maxWidth,
    int maxLines,
    gnx::gfx::FontSize size = gnx::gfx::FontSize::Small) {
    std::vector<std::string> lines;
    std::string remaining = text;
    while (!remaining.empty() && static_cast<int>(lines.size()) < maxLines) {
        if (gfx.text_width(remaining, size) <= maxWidth) {
            lines.push_back(remaining);
            break;
        }

        std::size_t best = std::string::npos;
        std::size_t position = 0;
        while ((position = remaining.find(' ', position)) != std::string::npos) {
            const std::string candidate = remaining.substr(0, position);
            if (gfx.text_width(candidate, size) > maxWidth) break;
            best = position;
            ++position;
        }

        if (best == std::string::npos) {
            lines.push_back(fit_text(gfx, remaining, maxWidth, size));
            break;
        }

        lines.push_back(remaining.substr(0, best));
        remaining.erase(0, best + 1);
        while (!remaining.empty() && remaining.front() == ' ') {
            remaining.erase(remaining.begin());
        }

        if (static_cast<int>(lines.size()) == maxLines && !remaining.empty()) {
            lines.back() = fit_text(
                gfx, lines.back() + " " + remaining, maxWidth, size);
        }
    }
    return lines;
}

const char* resolution_label(int mode, bool english, bool compact = false) {
    switch (mode) {
        case gnx::stream::Resolution720p:
            return compact ? "720P" :
                (english ? "720p - performance" : "720p - rendimiento");
        case gnx::stream::Resolution1080p:
            return compact ? "1080P" :
                (english ? "1080p - quality" : "1080p - calidad");
        case gnx::stream::Resolution1440p:
            return compact ? "1440P" :
                (english ? "1440p - experimental" : "1440p - experimental");
        case gnx::stream::ResolutionAuto:
        default:
            return compact ? "AUTO" :
                (english ? "Automatic - 720p handheld / 1080p dock"
                         : "Automatico - 720p portatil / 1080p dock");
    }
}

std::pair<int, int> stream_dimensions(int mode) {
    switch (mode) {
        case gnx::stream::Resolution720p:
            return {1280, 720};
        case gnx::stream::Resolution1080p:
            return {1920, 1080};
        case gnx::stream::Resolution1440p:
            return {2560, 1440};
        case gnx::stream::ResolutionAuto:
        default:
            return appletGetOperationMode() == AppletOperationMode_Console
                ? std::pair<int, int>{1920, 1080}
                : std::pair<int, int>{1280, 720};
    }
}

bool point_in_rect(int x, int y, const gnx::stream::QuickRect& rect) {
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
}

// Touch controller for the in-stream two-dot menu. Returns true when the tap
// belongs to the panel. A tap outside closes it and remains available to the
// independent Guide/Home target.
bool handle_stream_settings_touch(gnx::stream::QuickMenuState& state,
                                  int x, int y, bool& layoutChanged,
                                  bool& persistentChanged) {
    layoutChanged = false;
    persistentChanged = false;
    if (point_in_rect(x, y, gnx::stream::kQuickToggleRect)) {
        state.open = !state.open;
        return true;
    }
    if (!state.open) return false;
    if (!point_in_rect(x, y, gnx::stream::kQuickPanelRect)) {
        state.open = false;
        return false;
    }

    bool changed = false;
    if (point_in_rect(x, y,
                      gnx::stream::quick_row_rect(
                          gnx::stream::QuickPerformance))) {
        state.performance = !state.performance;
        changed = true;
    } else if (point_in_rect(
                   x, y, gnx::stream::quick_row_rect(
                             gnx::stream::QuickController))) {
        state.xboxFaceLayout = !state.xboxFaceLayout;
        layoutChanged = true;
        persistentChanged = true;
        changed = true;
    } else if (point_in_rect(x, y, gnx::stream::kQuickResetRect)) {
        gnx::stream::apply_picture_preset(
            state, gnx::stream::PresetNatural);
        persistentChanged = true;
        changed = true;
    } else {
        for (int row = gnx::stream::QuickMouseSpeed;
             row <= gnx::stream::QuickSharpness; ++row) {
            int direction = 0;
            if (point_in_rect(x, y, gnx::stream::quick_minus_rect(row))) {
                direction = -1;
            } else if (point_in_rect(
                           x, y, gnx::stream::quick_plus_rect(row))) {
                direction = 1;
            } else if ((row == gnx::stream::QuickMouseSpeed ||
                        row == gnx::stream::QuickResolution ||
                        row == gnx::stream::QuickPreset) &&
                       point_in_rect(x, y,
                                     gnx::stream::quick_row_rect(row))) {
                direction = 1;
            }
            if (direction == 0) continue;

            if (row == gnx::stream::QuickMouseSpeed) {
                state.mouseSpeed = (state.mouseSpeed + direction + 3) % 3;
            } else if (row == gnx::stream::QuickResolution) {
                state.resolutionMode =
                    (state.resolutionMode + direction + 4) % 4;
            } else if (row == gnx::stream::QuickPreset) {
                int preset = state.picturePreset;
                if (preset == gnx::stream::PresetCustom) {
                    preset = direction > 0 ? gnx::stream::PresetNatural
                                           : gnx::stream::PresetSoft;
                } else {
                    preset = (preset + direction + 5) % 5;
                }
                gnx::stream::apply_picture_preset(state, preset);
            } else {
                state.picturePreset = gnx::stream::PresetCustom;
                if (row == gnx::stream::QuickBrightness) {
                    state.brightness += direction * 5;
                } else if (row == gnx::stream::QuickContrast) {
                    state.contrast += direction * 5;
                } else if (row == gnx::stream::QuickSaturation) {
                    state.saturation += direction * 5;
                } else if (row == gnx::stream::QuickGamma) {
                    state.gamma += direction * 5;
                } else {
                    state.sharpness = (state.sharpness + direction + 4) % 4;
                }
            }
            persistentChanged = true;
            changed = true;
            break;
        }
    }
    if (changed) state = gnx::stream::normalized_quick_menu(state);
    return true;
}

void draw_cover_texture(
    SDL_Renderer* renderer,
    SDL_Texture* texture,
    const SDL_Rect& destination) {
    if (!renderer || !texture) return;

    int width = 0;
    int height = 0;
    if (SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) != 0 ||
        width <= 0 || height <= 0) {
        return;
    }

    // "Contain" instead of "cover": keep the complete artwork visible and
    // letterbox the unused area. Previous versions deliberately cropped the
    // source to fill the card, which cut faces, logos and game titles.
    SDL_Rect fitted = destination;
    const long long sourceRatio = static_cast<long long>(width) * destination.h;
    const long long targetRatio = static_cast<long long>(height) * destination.w;
    if (sourceRatio > targetRatio) {
        fitted.h = std::max(1, destination.w * height / width);
        fitted.y += (destination.h - fitted.h) / 2;
    } else if (sourceRatio < targetRatio) {
        fitted.w = std::max(1, destination.h * width / height);
        fitted.x += (destination.w - fitted.w) / 2;
    }
    SDL_RenderCopy(renderer, texture, nullptr, &fitted);
}

void initialize_log() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/ZERODROID", 0777);
    if (std::freopen("sdmc:/switch/ZERODROID/zerodroid.log", "w", stderr)) {
        setvbuf(stderr, nullptr, _IOLBF, 0);
    }
}

// ZERODROID-Switch Main Engine with Direct 2D QR Rendering
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    initialize_log();
    std::fprintf(stderr, "ZERODROID v%s starting\n", ZERODROID_VERSION);

    // 1. Initialize Nintendo Switch Network Sockets
    const bool socketsReady = R_SUCCEEDED(socketInitializeDefault());
    if (!socketsReady) {
        std::fprintf(stderr, "Socket init failed\n");
    } else {
        std::fprintf(stderr, "Network sockets ready\n");
    }

    // Initialize PL service for shared fonts (REQUIRED for text rendering)
    const bool plReady = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (!plReady) {
        std::fprintf(stderr, "PL service init failed\n");
    }

    // The QR flow is HTTPS. Switch has no system CA store, so use the bundle
    // shipped in this NRO's romfs.
    const bool romfsReady = R_SUCCEEDED(romfsInit());
    if (romfsReady) {
        gnx::Http::set_ca_bundle("romfs:/cacert.pem");
        std::fprintf(stderr, "romfs and TLS CA bundle ready\n");
    } else {
        std::fprintf(stderr, "romfs init failed; HTTPS will not work\n");
    }

    // 2. Initialize Graphical Subsystem (SDL2 2D Engine)
    gnx::gfx::Gfx gfx;
    if (!gfx.init()) {
        std::fprintf(stderr, "GFX Init failed!\n");
        if (romfsReady) romfsExit();
        if (plReady) plExit();
        if (socketsReady) socketExit();
        return EXIT_FAILURE;
    }
    auto covers = std::make_unique<gnx::Covers>(
        gfx, "sdmc:/switch/ZERODROID/covers");

    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    // 3. Initialize ZERODROID Boosteroid Engine
    ZERODROID::BoosteroidAPI api;
    ServerSettings serverSettings = load_server_settings();
    auto ui = [&serverSettings](const char* spanish, const char* english) {
        return serverSettings.language == 1 ? english : spanish;
    };
    gnx::stream::QuickMenuState quickMenuState;
    quickMenuState.xboxFaceLayout = serverSettings.xboxFaceLayout;
    quickMenuState.mouseSpeed = serverSettings.mouseSpeed;
    quickMenuState.resolutionMode = serverSettings.resolutionMode;
    quickMenuState.picturePreset = serverSettings.picturePreset;
    quickMenuState.brightness = serverSettings.brightness;
    quickMenuState.contrast = serverSettings.contrast;
    quickMenuState.saturation = serverSettings.saturation;
    quickMenuState.gamma = serverSettings.gamma;
    quickMenuState.sharpness = serverSettings.sharpness;
    quickMenuState = gnx::stream::normalized_quick_menu(quickMenuState);

    const auto persistQuickMenu = [&]() {
        serverSettings.xboxFaceLayout = quickMenuState.xboxFaceLayout;
        serverSettings.mouseSpeed = quickMenuState.mouseSpeed;
        serverSettings.resolutionMode = quickMenuState.resolutionMode;
        serverSettings.picturePreset = quickMenuState.picturePreset;
        serverSettings.brightness = quickMenuState.brightness;
        serverSettings.contrast = quickMenuState.contrast;
        serverSettings.saturation = quickMenuState.saturation;
        serverSettings.gamma = quickMenuState.gamma;
        serverSettings.sharpness = quickMenuState.sharpness;
        save_server_settings(serverSettings);
    };
    auto stream = std::make_unique<gnx::stream::Engine>(api, gfx.renderer());
    stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
    stream->set_quick_menu_state(quickMenuState);
    ZERODROID::DeviceCodeAuthArtifact authArtifact;

    bool hasSession = api.loadSessionFromSD();
    std::string loginStatus = ui("Preparando inicio de sesion...",
                                 "Preparing sign in...");
    if (!hasSession) {
        if (api.requestDeviceCode(authArtifact)) {
            loginStatus = ui("Esperando autorizacion desde el movil...",
                             "Waiting for authorization from your phone...");
            std::fprintf(stderr, "QR UUID generated\n");
        } else {
            loginStatus = ui("No se pudo crear el codigo QR.",
                             "Could not create the QR code.");
        }
    }

    using SteadyClock = std::chrono::steady_clock;
    auto qrCreatedAt = SteadyClock::now();
    auto nextPollAt = SteadyClock::now();
    std::future<AsyncPollResult> pollFuture;
    bool pollInFlight = false;

    std::future<AsyncLibraryResult> libraryFuture;
    bool libraryInFlight = false;
    bool libraryReady = false;
    bool libraryOk = false;
    bool catalogOk = false;
    ZERODROID::BoosteroidUser libraryUser;
    std::vector<ZERODROID::GameItem> libraryGames;
    std::vector<ZERODROID::GameItem> catalogGames;
    std::vector<VisibleGame> visibleGames;
    std::string libraryError;
    std::string catalogError;
    std::string libraryNotice;
    std::string searchQuery;
    LibraryTab libraryTab = LibraryTab::MyGames;
    std::size_t selectedGame = 0;
    std::future<AsyncGameActionResult> gameActionFuture;
    bool gameActionInFlight = false;
    int gameActionId = 0;
    bool settingsOpen = false;
    bool serverConfigReady = false;
    bool serverConfigInFlight = false;
    std::future<AsyncServerConfigResult> serverConfigFuture;
    std::vector<ZERODROID::ServerLocation> serverLocations;
    std::string serverConfigError;
    std::size_t selectedLocation = 0;  // 0 = automatic
    int selectedSettingsRow = 0;       // regions, controller, resolution, location, language, logout
    bool serverSaveInFlight = false;
    std::future<AsyncServerSaveResult> serverSaveFuture;
    bool launchPanel = false;
    bool dekoStreaming = false;
    bool touchWasDown = false;
    bool touchCapturedByOverlay = false;
    bool touchMouseActive = false;
    int touchDownX = 0;
    int touchDownY = 0;
    int touchLastX = 0;
    int touchLastY = 0;
    int touchTravel = 0;
    uint64_t touchDownTick = 0;
    bool touchClickSyntheticDown = false;
    uint64_t touchClickReleaseAt = 0;
    uint64_t guideTouchUntil = 0;
    bool minusTapPending = false;
    bool minusMouseUsed = false;
    bool minusSyntheticDown = false;
    bool mouseLeftDown = false;
    bool mouseRightDown = false;
    bool reconnectRequested = false;
    uint64_t minusSyntheticReleaseAt = 0;
    uint64_t mouseMoveTick = SDL_GetTicks64();
    uint64_t overlayMetricsTick = 0;
    uint64_t mouseCursorVisibleUntil = 0;
    float virtualMouseX = 0.5f;
    float virtualMouseY = 0.5f;
    std::string launchingTitle;
    int launchingGameId = 0;
    std::string activeStreamTitle;
    int activeStreamGameId = 0;
    int activeStreamWidth = 1280;
    int activeStreamHeight = 720;

    // The remote native stream does not reliably draw a mouse pointer, so
    // ZERODROID renders its own. Every mouse/touch action refreshes a three
    // second visibility window; inactivity hides it without changing position.
    const auto showVirtualCursor = [&](uint64_t nowTicks) {
        quickMenuState.mouseCursorX = virtualMouseX;
        quickMenuState.mouseCursorY = virtualMouseY;
        quickMenuState.mouseCursorVisible = true;
        mouseCursorVisibleUntil = nowTicks + 3000;
        stream->set_quick_menu_state(quickMenuState);
    };
    const auto hideVirtualCursor = [&]() {
        if (!quickMenuState.mouseCursorVisible) return;
        quickMenuState.mouseCursorVisible = false;
        mouseCursorVisibleUntil = 0;
        stream->set_quick_menu_state(quickMenuState);
    };

    const auto gameFromVisible = [&](const VisibleGame& visible)
        -> ZERODROID::GameItem* {
        auto& source = visible.catalog ? catalogGames : libraryGames;
        return visible.index < source.size() ? &source[visible.index] : nullptr;
    };

    auto rebuildVisibleGames = [&]() {
        visibleGames.clear();
        const std::string normalizedQuery = lowercase_ascii(searchQuery);
        const auto matchesSearch = [&](const ZERODROID::GameItem& game) {
            return normalizedQuery.empty() ||
                   lowercase_ascii(game.title + " " + game.store + " " +
                                   game.platform).find(normalizedQuery) !=
                       std::string::npos;
        };
        const auto appendMatches = [&](bool catalog, std::size_t index) {
            const auto& source = catalog ? catalogGames : libraryGames;
            if (index < source.size() && matchesSearch(source[index])) {
                visibleGames.push_back({catalog, index});
            }
        };
        if (libraryTab == LibraryTab::Recent) {
            for (int gameId : serverSettings.recentGameIds) {
                const auto found = std::find_if(
                    libraryGames.begin(), libraryGames.end(),
                    [gameId](const ZERODROID::GameItem& game) {
                        return game.id == gameId;
                    });
                if (found != libraryGames.end() && matchesSearch(*found)) {
                    visibleGames.push_back({false, static_cast<std::size_t>(
                        std::distance(libraryGames.begin(), found))});
                }
            }
        } else if (libraryTab == LibraryTab::Favorites) {
            std::vector<int> added;
            for (std::size_t index = 0; index < libraryGames.size(); ++index) {
                const auto& game = libraryGames[index];
                if (contains_game_id(serverSettings.favoriteGameIds, game.id)) {
                    appendMatches(false, index);
                    added.push_back(game.id);
                }
            }
            for (std::size_t index = 0; index < catalogGames.size(); ++index) {
                const auto& game = catalogGames[index];
                if (!contains_game_id(added, game.id) &&
                    contains_game_id(serverSettings.favoriteGameIds, game.id)) {
                    appendMatches(true, index);
                }
            }
        } else if (libraryTab == LibraryTab::Catalog ||
                   libraryTab == LibraryTab::InstallAndPlay) {
            for (std::size_t index = 0; index < catalogGames.size(); ++index) {
                if (libraryTab == LibraryTab::InstallAndPlay &&
                    !catalogGames[index].installAndPlay) continue;
                appendMatches(true, index);
            }
        } else {
            for (std::size_t index = 0; index < libraryGames.size(); ++index) {
                appendMatches(false, index);
            }
        }
        selectedGame = visibleGames.empty()
            ? 0
            : std::min(selectedGame, visibleGames.size() - 1);
    };

    auto startServerConfigLoad = [&]() {
        if (!hasSession || serverConfigInFlight || serverSaveInFlight) return;
        serverConfigInFlight = true;
        serverConfigReady = false;
        serverConfigError.clear();
        serverConfigFuture = std::async(std::launch::async, [&api]() {
            AsyncServerConfigResult result;
            result.ok = api.getServerConfiguration(
                result.locations, result.preferences);
            if (!result.ok) result.error = api.lastError();
            return result;
        });
    };

    auto startServerSave = [&]() {
        if (!serverConfigReady || serverConfigInFlight || serverSaveInFlight) {
            return;
        }
        const ZERODROID::ServerPreferences preferences{
            serverSettings.allowDistantRegions,
            serverSettings.preferredLocationId};
        serverSaveInFlight = true;
        serverConfigError.clear();
        serverSaveFuture = std::async(
            std::launch::async, [&api, preferences]() {
                AsyncServerSaveResult result;
                result.ok = api.updateServerConfiguration(preferences);
                if (!result.ok) result.error = api.lastError();
                return result;
            });
    };

    auto startLibraryLoad = [&]() {
        if (!hasSession || libraryInFlight || gameActionInFlight) return;
        libraryInFlight = true;
        libraryReady = false;
        libraryOk = false;
        libraryError.clear();
        catalogError.clear();
        libraryNotice.clear();
        std::fprintf(stderr, "Starting Boosteroid library load\n");
        libraryFuture = std::async(std::launch::async, [&api]() {
            AsyncLibraryResult result;
            result.profileOk = api.getUserProfile(result.user);
            if (!result.profileOk) result.profileError = api.lastError();
            result.installedOk = api.getInstalledGames(result.installedGames);
            if (!result.installedOk) result.installedError = api.lastError();
            result.catalogOk = api.getCatalogGames(result.catalogGames);
            if (!result.catalogOk) result.catalogError = api.lastError();
            if (result.catalogOk) {
                for (auto& catalogGame : result.catalogGames) {
                    const auto installed = std::find_if(
                        result.installedGames.begin(), result.installedGames.end(),
                        [&](const ZERODROID::GameItem& game) {
                            return game.id == catalogGame.id;
                        });
                    if (installed != result.installedGames.end()) {
                        catalogGame.isInstalled = true;
                    }
                }
            }
            return result;
        });
    };

    auto startAddToLibrary = [&](const ZERODROID::GameItem& game) {
        if (gameActionInFlight || libraryInFlight || game.id <= 0) return;
        gameActionInFlight = true;
        gameActionId = game.id;
        libraryNotice = ui("Agregando a Mis juegos...",
                           "Adding to My games...");
        gameActionFuture = std::async(
            std::launch::async, [&api, gameId = game.id]() {
                AsyncGameActionResult result;
                result.gameId = gameId;
                result.ok = api.addGameToLibrary(gameId);
                if (!result.ok) result.error = api.lastError();
                return result;
            });
    };

    auto createNewQr = [&]() {
        if (pollInFlight) return;
        if (api.requestDeviceCode(authArtifact)) {
            qrCreatedAt = SteadyClock::now();
            nextPollAt = qrCreatedAt;
            loginStatus = ui("Esperando autorizacion desde el movil...",
                             "Waiting for authorization from your phone...");
            std::fprintf(stderr, "New QR UUID generated\n");
        } else {
            loginStatus = ui("No se pudo crear el codigo QR.",
                             "Could not create the QR code.");
        }
    };

    auto logout = [&]() {
        if (libraryInFlight || gameActionInFlight || serverConfigInFlight ||
            serverSaveInFlight) {
            libraryNotice = ui(
                "Espera a que terminen las consultas para cerrar la sesion.",
                "Wait for the pending requests before signing out.");
            return false;
        }
        api.logout();
        hasSession = false;
        libraryReady = false;
        libraryOk = false;
        catalogOk = false;
        libraryGames.clear();
        catalogGames.clear();
        visibleGames.clear();
        libraryUser = ZERODROID::BoosteroidUser();
        libraryError.clear();
        catalogError.clear();
        libraryNotice.clear();
        searchQuery.clear();
        libraryTab = LibraryTab::MyGames;
        selectedGame = 0;
        settingsOpen = false;
        covers->drop_textures();
        createNewQr();
        return true;
    };

    if (hasSession) startLibraryLoad();

    // 4. Main SDL2 Graphical Render Loop
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 kDown = padGetButtonsDown(&pad);
        const u64 kUp = padGetButtonsUp(&pad);
        const u64 kHeld = padGetButtons(&pad);

        if (dekoStreaming) {
            const uint64_t nowTicks = SDL_GetTicks64();
            const bool overlayWasOpenAtFrameStart = quickMenuState.open ||
                quickMenuState.sessionActionsOpen;
            const u64 exitMask =
                HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus;
            const bool leaveStream = (kHeld & exitMask) == exitMask;

            // A plain Minus press remains the normal View/Back button. It is
            // delayed until release so holding Minus can safely become the
            // virtual-mouse modifier without also reaching the game.
            if (minusSyntheticDown && nowTicks >= minusSyntheticReleaseAt) {
                stream->send_controller_button(6, false);
                minusSyntheticDown = false;
            }
            if (touchClickSyntheticDown && nowTicks >= touchClickReleaseAt) {
                stream->send_mouse_button(0, false);
                touchClickSyntheticDown = false;
            }
            if (kDown & HidNpadButton_Minus) {
                minusTapPending = true;
                minusMouseUsed = false;
            }

            HidTouchScreenState touchState{};
            const bool touching =
                hidGetTouchScreenStates(&touchState, 1) > 0 &&
                touchState.count > 0;
            if (touching) {
                const int x = touchState.touches[0].x * gnx::gfx::kWidth / 1280;
                const int y = touchState.touches[0].y * gnx::gfx::kHeight / 720;
                if (!touchWasDown) {
                    touchDownX = x;
                    touchDownY = y;
                    touchLastX = x;
                    touchLastY = y;
                    touchTravel = 0;
                    touchDownTick = nowTicks;
                    touchCapturedByOverlay = quickMenuState.open ||
                        quickMenuState.sessionActionsOpen ||
                        point_in_rect(x, y, gnx::stream::kQuickToggleRect) ||
                        point_in_rect(x, y, gnx::stream::kGuideButtonRect);
                    touchMouseActive = !touchCapturedByOverlay &&
                        quickMenuState.mouseModeEnabled;
                    if (touchMouseActive) showVirtualCursor(nowTicks);
                    // Trackpad semantics: touching a new place never teleports
                    // the remote cursor and never presses a button immediately.
                } else if (touchMouseActive &&
                           (x != touchLastX || y != touchLastY)) {
                    const int deltaX = x - touchLastX;
                    const int deltaY = y - touchLastY;
                    touchTravel += std::abs(deltaX) + std::abs(deltaY);
                    static constexpr float touchGains[3] = {0.75f, 1.0f, 1.35f};
                    const int speedIndex = std::clamp(
                        quickMenuState.mouseSpeed,
                        static_cast<int>(gnx::stream::MousePrecise),
                        static_cast<int>(gnx::stream::MouseFast));
                    const float gain = touchGains[speedIndex];
                    virtualMouseX = std::clamp(
                        virtualMouseX +
                            (static_cast<float>(deltaX) / gnx::gfx::kWidth) * gain,
                        0.0f, 1.0f);
                    virtualMouseY = std::clamp(
                        virtualMouseY +
                            (static_cast<float>(deltaY) / gnx::gfx::kHeight) * gain,
                        0.0f, 1.0f);
                    stream->send_mouse_position(
                        virtualMouseX, virtualMouseY, true);
                    showVirtualCursor(nowTicks);
                } else if (!touchMouseActive &&
                           (x != touchLastX || y != touchLastY)) {
                    touchTravel += std::abs(x - touchLastX) +
                                   std::abs(y - touchLastY);
                }
                touchWasDown = true;
                touchLastX = x;
                touchLastY = y;
            } else if (touchWasDown) {
                touchWasDown = false;
                if (touchMouseActive) {
                    constexpr uint64_t kTapMaxMs = 260;
                    constexpr int kTapMaxTravel = 30;
                    const uint64_t heldMs = nowTicks - touchDownTick;
                    if (heldMs <= kTapMaxMs && touchTravel <= kTapMaxTravel) {
                        // A quick tap clicks exactly where the cursor already is;
                        // it does not jump to the finger's absolute position.
                        stream->send_mouse_position(
                            virtualMouseX, virtualMouseY, true);
                        showVirtualCursor(nowTicks);
                        if (touchClickSyntheticDown)
                            stream->send_mouse_button(0, false);
                        stream->send_mouse_button(0, true);
                        touchClickSyntheticDown = true;
                        touchClickReleaseAt = nowTicks + 45;
                    }
                    touchMouseActive = false;
                } else if (touchCapturedByOverlay) {
                    const int dx = std::abs(touchLastX - touchDownX);
                    const int dy = std::abs(touchLastY - touchDownY);
                    if (dx < 100 && dy < 100) {
                        const int tapX = touchLastX;
                        const int tapY = touchLastY;
                        bool menuStateChanged = false;

                        if (point_in_rect(
                                tapX, tapY,
                                gnx::stream::kGuideButtonRect)) {
                            // The always-visible Xbox/X touch icon is now the
                            // session-actions launcher. Guide/Home remains the
                            // first action inside the panel.
                            quickMenuState.sessionActionsOpen =
                                !quickMenuState.sessionActionsOpen;
                            quickMenuState.reconnectConfirmOpen = false;
                            quickMenuState.open = false;
                            if (!quickMenuState.sessionActionsOpen &&
                                quickMenuState.mouseModeEnabled)
                                showVirtualCursor(nowTicks);
                            menuStateChanged = true;
                        } else if (quickMenuState.sessionActionsOpen) {
                            if (quickMenuState.reconnectConfirmOpen) {
                                if (point_in_rect(
                                        tapX, tapY,
                                        gnx::stream::kReconnectConfirmRect)) {
                                    reconnectRequested = true;
                                    quickMenuState.reconnectConfirmOpen = false;
                                    quickMenuState.sessionActionsOpen = false;
                                } else if (point_in_rect(
                                               tapX, tapY,
                                               gnx::stream::kReconnectCancelRect) ||
                                           !point_in_rect(
                                               tapX, tapY,
                                               gnx::stream::kReconnectConfirmPanelRect)) {
                                    quickMenuState.reconnectConfirmOpen = false;
                                }
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionGuideRect)) {
                                guideTouchUntil = nowTicks + 160;
                                quickMenuState.sessionActionsOpen = false;
                                quickMenuState.reconnectConfirmOpen = false;
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionSteamRect)) {
                                stream->send_steam_overlay();
                                quickMenuState.sessionActionsOpen = false;
                                quickMenuState.reconnectConfirmOpen = false;
                                // Steam's overlay is mouse-driven. Reveal the
                                // local pointer as soon as the Control Center
                                // closes so Exit Game can be reached without a
                                // blind first movement.
                                if (quickMenuState.mouseModeEnabled)
                                    showVirtualCursor(nowTicks);
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionAltTabRect)) {
                                stream->send_alt_tab();
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionKeyboardRect)) {
                                // The native Switch keyboard cannot safely own
                                // the display while deko3d owns the streaming
                                // swapchain. Keep this informational until a
                                // suspend/resume text injection path is proven.
                                stream->log("session overlay keyboard info tapped");
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionMouseRect)) {
                                quickMenuState.mouseModeEnabled =
                                    !quickMenuState.mouseModeEnabled;
                                if (!quickMenuState.mouseModeEnabled) {
                                    if (mouseLeftDown)
                                        stream->send_mouse_button(0, false);
                                    if (mouseRightDown)
                                        stream->send_mouse_button(2, false);
                                    mouseLeftDown = false;
                                    mouseRightDown = false;
                                    hideVirtualCursor();
                                } else {
                                    showVirtualCursor(nowTicks);
                                }
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionReconnectRect)) {
                                quickMenuState.reconnectConfirmOpen = true;
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionSettingsRect)) {
                                quickMenuState.sessionActionsOpen = false;
                                quickMenuState.reconnectConfirmOpen = false;
                                quickMenuState.open = true;
                            } else if (point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionCloseRect) ||
                                       !point_in_rect(
                                           tapX, tapY,
                                           gnx::stream::kSessionPanelRect)) {
                                quickMenuState.sessionActionsOpen = false;
                                quickMenuState.reconnectConfirmOpen = false;
                                if (quickMenuState.mouseModeEnabled)
                                    showVirtualCursor(nowTicks);
                            }
                            menuStateChanged = true;
                        } else {
                            const bool wasOpen = quickMenuState.open;
                            bool layoutChanged = false;
                            bool persistentChanged = false;
                            const bool quickTouch =
                                handle_stream_settings_touch(
                                    quickMenuState, tapX, tapY,
                                    layoutChanged, persistentChanged);
                            if (point_in_rect(
                                    tapX, tapY,
                                    gnx::stream::kQuickToggleRect)) {
                                quickMenuState.sessionActionsOpen = false;
                                quickMenuState.reconnectConfirmOpen = false;
                            }
                            if (quickTouch || wasOpen != quickMenuState.open) {
                                menuStateChanged = true;
                            }
                            if (layoutChanged) {
                                serverSettings.xboxFaceLayout =
                                    quickMenuState.xboxFaceLayout;
                                stream->set_xbox_face_layout(
                                    serverSettings.xboxFaceLayout);
                            }
                            if (persistentChanged) persistQuickMenu();
                        }

                        if (menuStateChanged) {
                            stream->set_quick_menu_state(quickMenuState);
                        }
                    }
                }
                touchCapturedByOverlay = false;
                touchMouseActive = false;
            }

            if (quickMenuState.mouseCursorVisible &&
                mouseCursorVisibleUntil != 0 &&
                nowTicks >= mouseCursorVisibleUntil && !touchMouseActive) {
                hideVirtualCursor();
            }

            const u64 overlayShortcut =
                HidNpadButton_Plus | HidNpadButton_Minus;
            if ((kHeld & overlayShortcut) == overlayShortcut &&
                (kDown & overlayShortcut) != 0) {
                quickMenuState.sessionActionsOpen =
                    !quickMenuState.sessionActionsOpen;
                quickMenuState.reconnectConfirmOpen = false;
                quickMenuState.open = false;
                minusTapPending = false;
                minusMouseUsed = true;
                stream->set_quick_menu_state(quickMenuState);
            }
            if ((kDown & HidNpadButton_B) &&
                (quickMenuState.sessionActionsOpen || quickMenuState.open)) {
                quickMenuState.sessionActionsOpen = false;
                quickMenuState.reconnectConfirmOpen = false;
                quickMenuState.open = false;
                if (quickMenuState.mouseModeEnabled)
                    showVirtualCursor(nowTicks);
                else
                    stream->set_quick_menu_state(quickMenuState);
            }

            if (quickMenuState.sessionActionsOpen &&
                (overlayMetricsTick == 0 || nowTicks - overlayMetricsTick >= 500)) {
                overlayMetricsTick = nowTicks;
                const auto liveState = stream->state();
                quickMenuState.sessionStatus =
                    liveState == gnx::stream::EngineState::Streaming
                        ? "CONNECTED"
                        : (liveState == gnx::stream::EngineState::Failed
                               ? "FAILED"
                               : "CONNECTING");
                quickMenuState.currentGame = activeStreamTitle;
                quickMenuState.gatewayLabel = stream->gateway();
                quickMenuState.streamWidth = activeStreamWidth;
                quickMenuState.streamHeight = activeStreamHeight;
                const bool docked =
                    appletGetOperationMode() == AppletOperationMode_Console;
                quickMenuState.outputWidth = docked ? 1920 : 1280;
                quickMenuState.outputHeight = docked ? 1080 : 720;
                quickMenuState.sessionSeconds = stream->session_seconds();
                quickMenuState.droppedGroups = stream->dropped_groups();
                quickMenuState.recoveredGroups = stream->recovered_groups();
                quickMenuState.recoveryRequests = stream->recovery_requests();
                quickMenuState.mouseMoves = stream->mouse_moves();
                quickMenuState.mouseClicks = stream->mouse_clicks();
                quickMenuState.keyboardEvents = stream->keyboard_events();
                stream->set_quick_menu_state(quickMenuState);
            }

            const bool guidePressed = nowTicks < guideTouchUntil;
            stream->set_guide_button_pressed(guidePressed);

            HidAnalogStickState leftStick = padGetStickPos(&pad, 0);
            HidAnalogStickState rightStick = padGetStickPos(&pad, 1);
            HidAnalogStickState gameRightStick = rightStick;
            u64 gameButtons = kHeld & ~HidNpadButton_Minus;
            const bool minusHeld = (kHeld & HidNpadButton_Minus) != 0;
            const bool streamOverlayOpen = overlayWasOpenAtFrameStart ||
                quickMenuState.open || quickMenuState.sessionActionsOpen;
            if (streamOverlayOpen) {
                // The control centre owns input while visible. Send neutral
                // controller state so opening the overlay cannot also move the
                // game, rotate the camera or activate a menu behind it.
                leftStick = {};
                gameRightStick = {};
                gameButtons = 0;
            }

            const float mouseDt = std::clamp(
                static_cast<float>(nowTicks - mouseMoveTick) / 1000.0f,
                0.0f, 0.05f);
            mouseMoveTick = nowTicks;

            if (minusHeld && quickMenuState.mouseModeEnabled &&
                !streamOverlayOpen) {
                // Minus owns desktop-input controls while held. Suppress their
                // normal gamepad meaning so the remote game never receives a
                // camera movement, trigger, X button or stick click at the same
                // time as mouse/keyboard input.
                gameRightStick = {};
                gameButtons &= ~(HidNpadButton_StickR | HidNpadButton_X |
                                 HidNpadButton_ZR | HidNpadButton_ZL);

                if (kDown & HidNpadButton_X) {
                    stream->send_alt_tab();
                    minusMouseUsed = true;
                    minusTapPending = false;
                }

                const bool wantsLeftClick =
                    (kHeld & (HidNpadButton_ZR | HidNpadButton_ZL)) != 0;
                if (wantsLeftClick != mouseLeftDown) {
                    stream->send_mouse_button(0, wantsLeftClick);
                    mouseLeftDown = wantsLeftClick;
                    showVirtualCursor(nowTicks);
                    minusMouseUsed = true;
                    minusTapPending = false;
                }

                const bool wantsRightClick =
                    (kHeld & HidNpadButton_StickR) != 0;
                if (wantsRightClick != mouseRightDown) {
                    stream->send_mouse_button(2, wantsRightClick);
                    mouseRightDown = wantsRightClick;
                    showVirtualCursor(nowTicks);
                    minusMouseUsed = true;
                    minusTapPending = false;
                }

                constexpr float kMouseDeadzone = 4500.0f;
                const float rx = static_cast<float>(rightStick.x);
                const float ry = static_cast<float>(rightStick.y);
                const float magnitude = std::sqrt(rx * rx + ry * ry);
                if (magnitude > kMouseDeadzone) {
                    const float response = std::pow(std::clamp(
                        (magnitude - kMouseDeadzone) /
                            (32767.0f - kMouseDeadzone),
                        0.0f, 1.0f), 1.55f);
                    static constexpr float speeds[3] = {0.38f, 0.78f, 1.28f};
                    const int speedIndex = std::clamp(
                        quickMenuState.mouseSpeed,
                        static_cast<int>(gnx::stream::MousePrecise),
                        static_cast<int>(gnx::stream::MouseFast));
                    const float distance = speeds[speedIndex] * response * mouseDt;
                    virtualMouseX = std::clamp(
                        virtualMouseX + (rx / magnitude) * distance,
                        0.0f, 1.0f);
                    virtualMouseY = std::clamp(
                        virtualMouseY - (ry / magnitude) * distance,
                        0.0f, 1.0f);
                    stream->send_mouse_position(
                        virtualMouseX, virtualMouseY, true);
                    showVirtualCursor(nowTicks);
                    minusMouseUsed = true;
                    minusTapPending = false;
                }
            } else {
                if (mouseLeftDown) {
                    stream->send_mouse_button(0, false);
                    mouseLeftDown = false;
                }
                if (mouseRightDown) {
                    stream->send_mouse_button(2, false);
                    mouseRightDown = false;
                }
                if ((kUp & HidNpadButton_Minus) && minusTapPending &&
                    !minusMouseUsed && !leaveStream) {
                    stream->send_controller_button(6, true);
                    minusSyntheticDown = true;
                    minusSyntheticReleaseAt = nowTicks + 45;
                }
                if (kUp & HidNpadButton_Minus) {
                    minusTapPending = false;
                    minusMouseUsed = false;
                }
            }

            stream->send_gamepad(leftStick, gameRightStick, gameButtons);
            stream->pump_video();

            if (reconnectRequested && activeStreamGameId > 0) {
                // Release every synthetic input before dropping the local
                // transport. Unlike a normal Stop, this path deliberately does
                // not send terminating/hangup/dequeue, allowing Boosteroid to
                // keep the existing VM and game alive for the new attachment.
                if (touchClickSyntheticDown) stream->send_mouse_button(0, false);
                if (mouseLeftDown) stream->send_mouse_button(0, false);
                if (mouseRightDown) stream->send_mouse_button(2, false);
                if (minusSyntheticDown) {
                    stream->send_controller_button(6, false);
                }
                stream->end_deko_output();
                stream->disconnect_for_reconnect();
                dekoStreaming = false;
                touchWasDown = false;
                touchCapturedByOverlay = false;
                touchMouseActive = false;
                touchClickSyntheticDown = false;
                touchTravel = 0;
                guideTouchUntil = 0;
                minusTapPending = false;
                minusMouseUsed = false;
                minusSyntheticDown = false;
                mouseLeftDown = false;
                mouseRightDown = false;
                mouseCursorVisibleUntil = 0;
                quickMenuState.mouseCursorVisible = false;
                reconnectRequested = false;
                quickMenuState.open = false;
                quickMenuState.sessionActionsOpen = false;
                quickMenuState.reconnectConfirmOpen = false;
                if (!gfx.resume()) break;

                stream = std::make_unique<gnx::stream::Engine>(
                    api, gfx.renderer());
                stream->set_xbox_face_layout(
                    serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                covers = std::make_unique<gnx::Covers>(
                    gfx, "sdmc:/switch/ZERODROID/covers");

                launchingTitle = ui("Reconectando: ", "Reconnecting: ") +
                                 activeStreamTitle;
                launchingGameId = 0;
                libraryNotice = ui(
                    "Reconectando a la sesion que sigue abierta...",
                    "Reconnecting to the session that is still running...");
                launchPanel = true;
                stream->start(activeStreamGameId, activeStreamWidth,
                              activeStreamHeight);
                continue;
            }

            const auto streamState = stream->state();
            if (leaveStream || streamState == gnx::stream::EngineState::Failed ||
                streamState == gnx::stream::EngineState::Stopped) {
                if (touchClickSyntheticDown) stream->send_mouse_button(0, false);
                if (mouseLeftDown) stream->send_mouse_button(0, false);
                if (mouseRightDown) stream->send_mouse_button(2, false);
                if (minusSyntheticDown) stream->send_controller_button(6, false);
                const std::string streamError = stream->error();
                stream->end_deko_output();
                stream->stop();
                dekoStreaming = false;
                touchWasDown = false;
                touchCapturedByOverlay = false;
                touchMouseActive = false;
                touchClickSyntheticDown = false;
                touchTravel = 0;
                guideTouchUntil = 0;
                minusTapPending = false;
                minusMouseUsed = false;
                minusSyntheticDown = false;
                mouseLeftDown = false;
                mouseRightDown = false;
                mouseCursorVisibleUntil = 0;
                quickMenuState.mouseCursorVisible = false;
                reconnectRequested = false;
                quickMenuState.open = false;
                quickMenuState.sessionActionsOpen = false;
                quickMenuState.reconnectConfirmOpen = false;
                if (!gfx.resume()) break;
                stream = std::make_unique<gnx::stream::Engine>(api, gfx.renderer());
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                covers = std::make_unique<gnx::Covers>(
                    gfx, "sdmc:/switch/ZERODROID/covers");
                libraryNotice = streamError.empty()
                    ? "Streaming cerrado."
                    : "Streaming termino: " + streamError;
            }
            continue;
        }

        if (kDown & HidNpadButton_Plus) {
            break; // Exit app cleanly
        }

        const auto now = SteadyClock::now();

        if (serverConfigInFlight &&
            serverConfigFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncServerConfigResult completed = serverConfigFuture.get();
                serverLocations = std::move(completed.locations);
                serverConfigError = std::move(completed.error);
                serverConfigReady = completed.ok;
                selectedLocation = 0;
                if (completed.ok) {
                    serverSettings.allowDistantRegions =
                        completed.preferences.allowDistantRegions;
                    serverSettings.preferredLocationId =
                        completed.preferences.preferredLocationId;
                    serverSettings.preferredLocationLabel = "Automatico";
                    for (std::size_t i = 0; i < serverLocations.size(); ++i) {
                        if (serverLocations[i].id ==
                            serverSettings.preferredLocationId) {
                            selectedLocation = i + 1;
                            serverSettings.preferredLocationLabel =
                                serverLocations[i].title;
                            break;
                        }
                    }
                    save_server_settings(serverSettings);
                }
            } catch (const std::exception& error) {
                serverConfigReady = false;
                serverConfigError = error.what();
            }
            serverConfigInFlight = false;
        }

        if (serverSaveInFlight &&
            serverSaveFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncServerSaveResult completed = serverSaveFuture.get();
                if (completed.ok) {
                    save_server_settings(serverSettings);
                    libraryNotice = "Configuracion de servidor guardada en Boosteroid.";
                    serverConfigError.clear();
                } else {
                    serverConfigError = completed.error;
                    libraryNotice = "No se pudo guardar: " + completed.error;
                }
            } catch (const std::exception& error) {
                serverConfigError = error.what();
            }
            serverSaveInFlight = false;
        }

        if (launchPanel &&
            stream->state() == gnx::stream::EngineState::Streaming) {
            if (launchingGameId != 0) {
                remember_recent_game(serverSettings.recentGameIds,
                                     launchingGameId);
                save_server_settings(serverSettings);
                rebuildVisibleGames();
                launchingGameId = 0;
            }
            covers->drop_textures();
            gfx.suspend();
            if (!stream->begin_deko_output()) {
                stream->stop();
                if (!gfx.resume()) break;
                stream = std::make_unique<gnx::stream::Engine>(api, gfx.renderer());
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                covers = std::make_unique<gnx::Covers>(
                    gfx, "sdmc:/switch/ZERODROID/covers");
                libraryNotice = "No se pudo iniciar la salida de video de Switch.";
                launchPanel = false;
            } else {
                launchPanel = false;
                dekoStreaming = true;
                touchWasDown = false;
                touchCapturedByOverlay = false;
                touchMouseActive = false;
                touchClickSyntheticDown = false;
                touchTravel = 0;
                minusTapPending = false;
                minusMouseUsed = false;
                minusSyntheticDown = false;
                mouseLeftDown = false;
                mouseRightDown = false;
                reconnectRequested = false;
                quickMenuState.sessionActionsOpen = false;
                quickMenuState.reconnectConfirmOpen = false;
                mouseMoveTick = SDL_GetTicks64();
                virtualMouseX = 0.5f;
                virtualMouseY = 0.5f;
                mouseCursorVisibleUntil = 0;
                quickMenuState.mouseCursorX = virtualMouseX;
                quickMenuState.mouseCursorY = virtualMouseY;
                quickMenuState.mouseCursorVisible = false;
                stream->set_quick_menu_state(quickMenuState);
                continue;
            }
        }
        if (!hasSession && pollInFlight &&
            pollFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const AsyncPollResult completed = pollFuture.get();
            pollInFlight = false;
            switch (completed.result) {
                case ZERODROID::DeviceCodePollResult::Succeeded:
                    hasSession = true;
                    loginStatus = ui("Sesion vinculada correctamente.",
                                     "Account linked successfully.");
                    std::fprintf(stderr, "QR login succeeded and session was saved\n");
                    startLibraryLoad();
                    break;
                case ZERODROID::DeviceCodePollResult::Expired:
                    std::fprintf(stderr, "QR expired; generating a replacement\n");
                    createNewQr();
                    break;
                case ZERODROID::DeviceCodePollResult::Error:
                    loginStatus = ui("Error de red. Reintentando...",
                                     "Network error. Retrying...");
                    std::fprintf(stderr, "QR poll error: %s\n", api.lastError().c_str());
                    nextPollAt = now + std::chrono::seconds(authArtifact.intervalSeconds);
                    break;
                case ZERODROID::DeviceCodePollResult::Pending:
                    loginStatus = ui(
                        "Esperando autorizacion desde el movil...",
                        "Waiting for authorization from your phone...");
                    nextPollAt = now + std::chrono::seconds(authArtifact.intervalSeconds);
                    break;
            }
        }

        if (!hasSession && !pollInFlight && !authArtifact.deviceCode.empty() && now >= nextPollAt) {
            const std::string code = authArtifact.deviceCode;
            pollFuture = std::async(std::launch::async, [&api, code]() {
                AsyncPollResult result;
                result.result = api.pollDeviceCodeStatus(code, result.token);
                return result;
            });
            pollInFlight = true;
        }

        if (!hasSession && !pollInFlight &&
            now - qrCreatedAt >= std::chrono::seconds(authArtifact.expiresInSeconds)) {
            createNewQr();
        }

        if (hasSession && libraryInFlight &&
            libraryFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncLibraryResult completed = libraryFuture.get();
                libraryGames = std::move(completed.installedGames);
                catalogGames = std::move(completed.catalogGames);
                libraryUser = std::move(completed.user);
                libraryOk = completed.installedOk;
                catalogOk = completed.catalogOk;
                libraryError = std::move(completed.installedError);
                catalogError = std::move(completed.catalogError);
                if (libraryError.empty() && !completed.profileOk) {
                    libraryError = std::move(completed.profileError);
                }
                if (!libraryOk && libraryError.empty()) {
                    libraryError = ui(
                        "No se pudo cargar la biblioteca de Boosteroid.",
                        "Could not load the Boosteroid library.");
                }
                selectedGame = 0;
                rebuildVisibleGames();
                std::fprintf(stderr,
                             "Content load finished: library=%d/%zu catalog=%d/%zu\n",
                             libraryOk ? 1 : 0, libraryGames.size(),
                             catalogOk ? 1 : 0, catalogGames.size());
            } catch (const std::exception& error) {
                libraryOk = false;
                libraryError = error.what();
                std::fprintf(stderr, "Library worker exception: %s\n", error.what());
            } catch (...) {
                libraryOk = false;
                libraryError = ui(
                    "Fallo desconocido al cargar la biblioteca.",
                    "Unknown error while loading the library.");
                std::fprintf(stderr, "Unknown library worker exception\n");
            }
            libraryInFlight = false;
            libraryReady = true;
        }

        if (hasSession && gameActionInFlight &&
            gameActionFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncGameActionResult completed = gameActionFuture.get();
                if (completed.ok) {
                    auto catalogGame = std::find_if(
                        catalogGames.begin(), catalogGames.end(),
                        [&](const ZERODROID::GameItem& game) {
                            return game.id == completed.gameId;
                        });
                    if (catalogGame != catalogGames.end()) {
                        catalogGame->isInstalled = true;
                        const auto installed = std::find_if(
                            libraryGames.begin(), libraryGames.end(),
                            [&](const ZERODROID::GameItem& game) {
                                return game.id == completed.gameId;
                            });
                        if (installed == libraryGames.end()) {
                            libraryGames.push_back(*catalogGame);
                        }
                    }
                    libraryNotice = ui(
                        "Agregado a Mis juegos. Presiona A otra vez para jugar.",
                        "Added to My games. Press A again to play.");
                    rebuildVisibleGames();
                } else {
                    libraryNotice = ui("No se pudo agregar: ",
                                       "Could not add: ") + completed.error;
                }
            } catch (const std::exception& error) {
                libraryNotice = error.what();
            }
            gameActionInFlight = false;
            gameActionId = 0;
        }

        if (!hasSession && (kDown & HidNpadButton_Y)) {
            createNewQr();
        }

        if (hasSession && !settingsOpen && !launchPanel &&
            libraryReady && !libraryOk && !libraryInFlight &&
            (kDown & HidNpadButton_Y)) {
            startLibraryLoad();
        }

        if (launchPanel && (kDown & HidNpadButton_B)) {
            const std::string reason = stream->error();
            stream->stop();
            launchPanel = false;
            launchingGameId = 0;
            libraryNotice = reason.empty()
                ? "Inicio cancelado."
                : "No se pudo iniciar: " + reason;
        }

        if (settingsOpen) {
            const std::size_t locationCount = serverLocations.size() + 1;
            if (kDown & HidNpadButton_Up) {
                selectedSettingsRow = std::max(0, selectedSettingsRow - 1);
            }
            if (kDown & HidNpadButton_Down) {
                selectedSettingsRow = std::min(5, selectedSettingsRow + 1);
            }
            if (selectedSettingsRow == 0 && serverConfigReady &&
                (kDown & (HidNpadButton_Left | HidNpadButton_Right |
                          HidNpadButton_A))) {
                serverSettings.allowDistantRegions =
                    !serverSettings.allowDistantRegions;
                save_server_settings(serverSettings);
                startServerSave();
            }
            if (selectedSettingsRow == 1 &&
                (kDown & (HidNpadButton_Left | HidNpadButton_Right |
                          HidNpadButton_A))) {
                serverSettings.xboxFaceLayout =
                    !serverSettings.xboxFaceLayout;
                quickMenuState.xboxFaceLayout =
                    serverSettings.xboxFaceLayout;
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                persistQuickMenu();
                libraryNotice = serverSettings.xboxFaceLayout
                    ? ui("Controles configurados con disposicion fisica Xbox.",
                         "Controls now use the physical Xbox layout.")
                    : ui("Controles configurados con etiquetas Nintendo.",
                         "Controls now use Nintendo button labels.");
            }
            if (selectedSettingsRow == 2 &&
                (kDown & (HidNpadButton_Left | HidNpadButton_Right |
                          HidNpadButton_A))) {
                const int direction = (kDown & HidNpadButton_Left) ? -1 : 1;
                serverSettings.resolutionMode =
                    (serverSettings.resolutionMode + direction + 4) % 4;
                quickMenuState.resolutionMode = serverSettings.resolutionMode;
                stream->set_quick_menu_state(quickMenuState);
                persistQuickMenu();
                libraryNotice = ui(
                    "La resolucion se aplicara al iniciar el proximo juego.",
                    "The resolution applies when the next game starts.");
            }
            if (selectedSettingsRow == 3 && locationCount > 0) {
                if ((kDown & HidNpadButton_Left) && selectedLocation > 0) {
                    --selectedLocation;
                }
                if ((kDown & HidNpadButton_Right) &&
                    selectedLocation + 1 < locationCount) {
                    ++selectedLocation;
                }
            }
            if ((kDown & HidNpadButton_Y) && !serverConfigInFlight &&
                !serverSaveInFlight) startServerConfigLoad();
            if (selectedSettingsRow == 3 &&
                (kDown & HidNpadButton_A) && serverConfigReady &&
                !serverConfigInFlight && !serverSaveInFlight) {
                serverSettings.preferredLocationId = selectedLocation == 0
                    ? 0 : serverLocations[selectedLocation - 1].id;
                serverSettings.preferredLocationLabel = selectedLocation == 0
                    ? "Automatico"
                    : serverLocations[selectedLocation - 1].title;
                save_server_settings(serverSettings);
                startServerSave();
            }
            if (selectedSettingsRow == 4 &&
                (kDown & (HidNpadButton_Left | HidNpadButton_Right |
                          HidNpadButton_A))) {
                serverSettings.language = serverSettings.language == 0 ? 1 : 0;
                save_server_settings(serverSettings);
                libraryNotice = ui("Idioma cambiado a Espanol.",
                                   "Language changed to English.");
            }
            if (selectedSettingsRow == 5 && (kDown & HidNpadButton_A)) {
                logout();
            }
            if (kDown & HidNpadButton_B) settingsOpen = false;
        } else if (!launchPanel && hasSession && libraryReady &&
                   (kDown & HidNpadButton_Minus)) {
            settingsOpen = true;
            selectedSettingsRow = 0;
            if (!serverConfigReady && !serverConfigInFlight) {
                startServerConfigLoad();
            }
        }

        if (!settingsOpen && !launchPanel && hasSession && libraryReady &&
            (libraryOk || catalogOk)) {
            constexpr std::size_t kColumns = 4;
            constexpr std::size_t kPageSize = 8;
            if (kDown & HidNpadButton_ZL) {
                const int tab = static_cast<int>(libraryTab);
                libraryTab = static_cast<LibraryTab>((tab + 4) % 5);
                selectedGame = 0;
                rebuildVisibleGames();
            }
            if (kDown & HidNpadButton_ZR) {
                const int tab = static_cast<int>(libraryTab);
                libraryTab = static_cast<LibraryTab>((tab + 1) % 5);
                selectedGame = 0;
                rebuildVisibleGames();
            }
            if (kDown & HidNpadButton_Y) {
                searchQuery = open_search_keyboard(
                    searchQuery, serverSettings.language);
                selectedGame = 0;
                rebuildVisibleGames();
            }
            if (kDown & HidNpadButton_StickR) {
                startLibraryLoad();
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_X)) {
                ZERODROID::GameItem* game =
                    gameFromVisible(visibleGames[selectedGame]);
                if (!game) continue;
                const bool wasFavorite = contains_game_id(
                    serverSettings.favoriteGameIds, game->id);
                toggle_game_id(serverSettings.favoriteGameIds, game->id);
                save_server_settings(serverSettings);
                libraryNotice = wasFavorite
                    ? ui("Quitado de Favoritos.", "Removed from Favorites.")
                    : ui("Agregado a Favoritos.", "Added to Favorites.");
                rebuildVisibleGames();
            }
            if (!visibleGames.empty() &&
                (kDown & HidNpadButton_Left) && selectedGame % kColumns > 0) {
                --selectedGame;
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_Right) &&
                selectedGame + 1 < visibleGames.size() &&
                selectedGame % kColumns + 1 < kColumns) {
                ++selectedGame;
            }
            if (!visibleGames.empty() &&
                (kDown & HidNpadButton_Up) && selectedGame >= kColumns) {
                selectedGame -= kColumns;
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_Down) &&
                selectedGame + kColumns < visibleGames.size()) {
                selectedGame += kColumns;
            }
            if (!visibleGames.empty() &&
                (kDown & HidNpadButton_L) && selectedGame >= kPageSize) {
                selectedGame -= kPageSize;
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_R) &&
                selectedGame + kPageSize < visibleGames.size()) {
                selectedGame += kPageSize;
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_A)) {
                ZERODROID::GameItem* game =
                    gameFromVisible(visibleGames[selectedGame]);
                if (!game) continue;
                if (!game->isInstalled) {
                    startAddToLibrary(*game);
                    continue;
                }
                launchingTitle = game->title;
                launchingGameId = game->id;
                libraryNotice.clear();
                launchPanel = true;
                quickMenuState.open = false;
                quickMenuState.xboxFaceLayout =
                    serverSettings.xboxFaceLayout;
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                const auto dimensions = stream_dimensions(
                    serverSettings.resolutionMode);
                activeStreamGameId = game->id;
                activeStreamTitle = game->title;
                activeStreamWidth = dimensions.first;
                activeStreamHeight = dimensions.second;
                stream->start(game->id, dimensions.first, dimensions.second);
            }
        }

        covers->pump();

        // Begin Graphical Frame
        gfx.begin_frame();

        // Draw Header Glass Bar
        SDL_Rect headerRect = {0, 0, gnx::gfx::kWidth, 100};
        gfx.fill(headerRect, gnx::gfx::kBar);

        // Header Title
        gfx.text(std::string("ZERODROID - Boosteroid Cloud Gaming (v") + ZERODROID_VERSION + ")", 40, 30, gnx::gfx::FontSize::Title, gnx::gfx::kText);

        if (!hasSession) {
            // Main Card (Purple Glass)
            SDL_Rect cardRect = {160, 150, 1600, 780};
            gfx.fill(cardRect, gnx::gfx::kSurface);
            gfx.frame(cardRect, gnx::gfx::kAccent, 2);

            // Instructions Text
            gfx.text(ui("Escanea el codigo QR con tu movil",
                        "Scan the QR code with your phone"),
                     220, 200, gnx::gfx::FontSize::Title, gnx::gfx::kText);
            gfx.text(ui("Inicia sesion en Boosteroid y confirma la vinculacion.",
                        "Sign in to Boosteroid and confirm the connection."),
                     220, 290, gnx::gfx::FontSize::Body, gnx::gfx::kText);
            gfx.text(loginStatus, 220, 370, gnx::gfx::FontSize::Body, gnx::gfx::kWarn);
            gfx.text(ui("El QR se renueva automaticamente. Presiona (Y) para renovarlo ahora.",
                        "The QR refreshes automatically. Press (Y) to refresh now."),
                     220, 440, gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);

            // Render Crisp 2D QR Code directly onto Framebuffer
            draw_qr_code(gfx.renderer(), 1220, 280, 440, authArtifact.qrUrl);
        } else {
            if (settingsOpen) {
                SDL_Rect cardRect = {160, 125, 1600, 840};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kChipEdge, 2);
                gfx.text(ui("CONFIGURACION DE ZERODROID", "ZERODROID SETTINGS"),
                         220, 150, gnx::gfx::FontSize::Title,
                         gnx::gfx::kText);

                const auto drawSettingsRow = [&](const SDL_Rect& row,
                                                 int index,
                                                 gnx::gfx::Color selectedColor =
                                                     gnx::gfx::kSurfaceHi) {
                    gfx.fill(row, selectedSettingsRow == index
                        ? selectedColor : gnx::gfx::kChip);
                    gfx.frame(row, selectedSettingsRow == index
                        ? gnx::gfx::kFocus : gnx::gfx::kChipEdge,
                        selectedSettingsRow == index ? 3 : 1);
                };

                SDL_Rect distantRow = {205, 205, 1510, 82};
                drawSettingsRow(distantRow, 0);
                gfx.text(ui("Permitir regiones lejanas",
                            "Allow distant regions"),
                         245, 214, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                gfx.text(ui("Mas disponibilidad, con posible aumento de latencia.",
                            "More availability, potentially with higher latency."),
                         245, 252, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                SDL_Rect toggleTrack = {1550, 222, 100, 46};
                gfx.fill(toggleTrack, serverSettings.allowDistantRegions
                    ? gnx::gfx::Color{16, 150, 95}
                    : gnx::gfx::Color{70, 74, 88});
                SDL_Rect toggleKnob = {
                    serverSettings.allowDistantRegions ? 1608 : 1556,
                    228, 34, 34};
                gfx.fill(toggleKnob, gnx::gfx::kText);

                SDL_Rect controllerRow = {205, 300, 1510, 82};
                drawSettingsRow(controllerRow, 1);
                gfx.text(ui("Disposicion de botones A / B / X / Y",
                            "A / B / X / Y button layout"),
                         245, 309, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                gfx.text(serverSettings.xboxFaceLayout
                             ? ui("Xbox: coincide con la posicion fisica del mando.",
                                  "Xbox: matches the controller's physical positions.")
                             : ui("Nintendo: conserva las letras de la consola.",
                                  "Nintendo: keeps the console's printed labels."),
                         245, 347, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                SDL_Rect layoutChip = {1390, 311, 260, 58};
                gfx.fill(layoutChip, serverSettings.xboxFaceLayout
                    ? gnx::gfx::Color{16, 110, 80} : gnx::gfx::kBar);
                gfx.frame(layoutChip, gnx::gfx::kFocus, 2);
                gfx.text_centered(serverSettings.xboxFaceLayout
                                      ? "XBOX" : "NINTENDO",
                                  layoutChip.x + layoutChip.w / 2,
                                  layoutChip.y + 14,
                                  gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);

                SDL_Rect resolutionRow = {205, 395, 1510, 96};
                drawSettingsRow(resolutionRow, 2);
                gfx.text(ui("Resolucion de streaming",
                            "Streaming resolution"),
                         245, 404, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                gfx.text(ui("1440p recibe mas detalle y lo reduce a la pantalla; es experimental.",
                            "1440p receives extra detail and downsamples it; experimental."),
                         245, 444, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                SDL_Rect resolutionChip = {1030, 410, 620, 62};
                gfx.fill(resolutionChip, gnx::gfx::kBar);
                gfx.frame(resolutionChip, gnx::gfx::kFocus, 2);
                gfx.text("<", 1052, 421, gnx::gfx::FontSize::Body,
                         gnx::gfx::kFocus);
                gfx.text_centered(
                    resolution_label(serverSettings.resolutionMode,
                                     serverSettings.language == 1),
                    resolutionChip.x + resolutionChip.w / 2,
                    resolutionChip.y + 16,
                    gnx::gfx::FontSize::Small, gnx::gfx::kText);
                gfx.text(">", 1610, 421, gnx::gfx::FontSize::Body,
                         gnx::gfx::kFocus);

                SDL_Rect locationRow = {205, 505, 1510, 125};
                drawSettingsRow(locationRow, 3);
                gfx.text(ui("Ubicacion de servidor preferida",
                            "Preferred server location"),
                         245, 516, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                gfx.text(ui("Se aplica en el proximo inicio del juego.",
                            "Applies the next time a game starts."),
                         245, 556, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                gfx.text(ui("(Y) ACTUALIZAR", "(Y) REFRESH"), 1425, 516,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);
                SDL_Rect locationSelect = {880, 558, 770, 55};
                gfx.fill(locationSelect, gnx::gfx::kBar);
                gfx.frame(locationSelect, gnx::gfx::kChipEdge, 2);
                std::string locationLabel = ui("Automatico (recomendado)",
                                               "Automatic (recommended)");
                if (selectedLocation > 0 &&
                    selectedLocation <= serverLocations.size()) {
                    const auto& location = serverLocations[selectedLocation - 1];
                    locationLabel = location.title;
                    if (!location.country.empty()) {
                        locationLabel += " (" + location.country + ")";
                    }
                }
                gfx.text("<", 902, 565, gnx::gfx::FontSize::Body,
                         gnx::gfx::kFocus);
                gfx.text_centered(fit_text(gfx, locationLabel, 620),
                                  locationSelect.x + locationSelect.w / 2,
                                  locationSelect.y + 13,
                                  gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);
                gfx.text(">", 1610, 565, gnx::gfx::FontSize::Body,
                         gnx::gfx::kFocus);

                SDL_Rect languageRow = {205, 645, 1510, 82};
                drawSettingsRow(languageRow, 4);
                gfx.text(ui("Idioma de la interfaz", "Interface language"),
                         245, 654, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                gfx.text(ui("Se guarda para el proximo inicio.",
                            "Saved for the next launch."),
                         245, 692, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                SDL_Rect languageChip = {1390, 656, 260, 58};
                gfx.fill(languageChip, gnx::gfx::kBar);
                gfx.frame(languageChip, gnx::gfx::kFocus, 2);
                gfx.text_centered(serverSettings.language == 1
                                      ? "ENGLISH" : "ESPANOL",
                                  languageChip.x + languageChip.w / 2,
                                  languageChip.y + 14,
                                  gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);

                SDL_Rect logoutRow = {205, 742, 1510, 72};
                drawSettingsRow(logoutRow, 5,
                                gnx::gfx::Color{72, 32, 48});
                gfx.text(ui("Cerrar sesion", "Sign out"), 245, 754,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Favoritos y recientes permanecen en la consola.",
                            "Favorites and recent games remain on the console."),
                         520, 761, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                gfx.text("(A)", 1570, 754, gnx::gfx::FontSize::Body,
                         gnx::gfx::kError);

                if (serverConfigInFlight) {
                    gfx.text(ui("Cargando ubicaciones oficiales de Boosteroid...",
                                "Loading official Boosteroid locations..."),
                             220, 844, gnx::gfx::FontSize::Small,
                             gnx::gfx::kWarn);
                } else if (serverSaveInFlight) {
                    gfx.text(ui("Guardando en tu cuenta de Boosteroid...",
                                "Saving to your Boosteroid account..."),
                             220, 844, gnx::gfx::FontSize::Small,
                             gnx::gfx::kWarn);
                } else if (!serverConfigError.empty()) {
                    gfx.text(fit_text(gfx, serverConfigError, 1450), 220, 844,
                             gnx::gfx::FontSize::Small, gnx::gfx::kError);
                } else {
                    gfx.text(ui("Izquierda/derecha cambia; (A) selecciona o guarda.",
                                "Left/right changes; (A) selects or saves."),
                             220, 844, gnx::gfx::FontSize::Small,
                             gnx::gfx::kTextDim);
                }
            } else if (launchPanel) {
                SDL_Rect cardRect = {250, 190, 1420, 660};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                const bool failed =
                    stream->state() == gnx::stream::EngineState::Failed;
                gfx.frame(cardRect, failed ? gnx::gfx::kError
                                           : gnx::gfx::kFocus, 3);
                gfx.text(failed ? ui("No se pudo iniciar el juego",
                                     "Could not start the game")
                                : ui("Iniciando streaming",
                                     "Starting stream"),
                         330, 255, gnx::gfx::FontSize::Title,
                         failed ? gnx::gfx::kError : gnx::gfx::kFocus);
                gfx.text(fit_text(gfx, launchingTitle, 1240,
                                  gnx::gfx::FontSize::Body),
                         330, 355, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                const std::string streamStatus = failed
                    ? stream->error()
                    : stream->status();
                gfx.text(fit_text(gfx, streamStatus, 1240,
                                  gnx::gfx::FontSize::Body),
                         330, 465, gnx::gfx::FontSize::Body,
                         failed ? gnx::gfx::kError : gnx::gfx::kWarn);
                const std::string route = std::string(ui("Servidor: ", "Server: ")) +
                    serverSettings.preferredLocationLabel +
                    (serverSettings.allowDistantRegions
                        ? ui(" | regiones lejanas permitidas",
                             " | distant regions allowed")
                        : ui(" | solo mi region", " | my region only"));
                gfx.text(fit_text(gfx, route, 1240), 330, 570,
                         gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
                const auto dimensions = stream_dimensions(
                    serverSettings.resolutionMode);
                const std::string videoMode = std::string(
                    ui("Video solicitado: ", "Requested video: ")) +
                    std::to_string(dimensions.first) + "x" +
                    std::to_string(dimensions.second);
                gfx.text(videoMode, 330, 610,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);
                gfx.text(ui("La asignacion de una maquina puede tardar varios minutos.",
                            "Machine assignment may take several minutes."),
                         330, 665, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                gfx.text(ui("(B) Cancelar / volver a la biblioteca",
                            "(B) Cancel / return to library"), 330, 745,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
            } else if (libraryInFlight || !libraryReady) {
                SDL_Rect cardRect = {160, 150, 1600, 780};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kFocus, 2);

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                const int dotCount = static_cast<int>((elapsed / 450) % 4);
                gfx.text(ui("Sesion iniciada correctamente",
                            "Signed in successfully"), 220, 220,
                         gnx::gfx::FontSize::Title, gnx::gfx::kFocus);
                gfx.text(std::string(ui("Cargando Mis juegos y el catalogo completo",
                                        "Loading My games and the full catalog")) +
                             std::string(dotCount, '.'), 220, 310,
                          gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Cargando todas las paginas; puede tardar unos segundos.",
                            "Loading every page; this may take a few seconds."),
                         220, 390,
                         gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
                if (!libraryNotice.empty()) {
                    gfx.text(fit_text(gfx, libraryNotice, 1450), 220, 470,
                             gnx::gfx::FontSize::Small, gnx::gfx::kWarn);
                }
            } else if (!libraryOk && !catalogOk) {
                SDL_Rect cardRect = {160, 150, 1600, 780};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kError, 3);
                gfx.text(ui("No se pudo cargar la biblioteca",
                            "Could not load the library"), 220, 220,
                         gnx::gfx::FontSize::Title, gnx::gfx::kError);
                gfx.text(fit_text(gfx, libraryError, 1450,
                                  gnx::gfx::FontSize::Body),
                         220, 320, gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Presiona (Y) para reintentar o entra en Settings para cerrar sesion.",
                            "Press (Y) to retry or open Settings to sign out."),
                         220, 420, gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
            } else if (libraryGames.empty() && catalogGames.empty() &&
                       libraryOk && catalogOk) {
                SDL_Rect cardRect = {160, 150, 1600, 780};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kFocus, 2);
                gfx.text(ui("Tu biblioteca esta conectada",
                            "Your library is connected"), 220, 220,
                         gnx::gfx::FontSize::Title, gnx::gfx::kFocus);
                gfx.text(ui("Boosteroid no devolvio juegos ni catalogo para esta cuenta.",
                            "Boosteroid returned no games or catalog for this account."),
                         220, 320, gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Presiona R3 para volver a consultar todas las paginas.",
                            "Press R3 to query every page again."),
                         220, 400, gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
            } else {
                constexpr int kColumns = 4;
                constexpr int kRows = 2;
                constexpr int kPageSize = kColumns * kRows;
                constexpr int kCardWidth = 372;
                constexpr int kCardHeight = 310;
                constexpr int kGapX = 24;
                constexpr int kGapY = 24;
                constexpr int kGridX = 180;
                constexpr int kGridY = 276;

                const bool catalogView =
                    libraryTab == LibraryTab::Catalog ||
                    libraryTab == LibraryTab::InstallAndPlay;
                const std::size_t sourceTotal = catalogView
                    ? catalogGames.size() : libraryGames.size();
                const std::string owner = catalogView
                    ? ui("Catalogo completo de Boosteroid",
                         "Full Boosteroid catalog")
                    : (libraryUser.nickname.empty()
                        ? ui("Mis juegos", "My games")
                        : std::string(ui("Juegos de ", "Games of ")) +
                              libraryUser.nickname);
                gfx.text(owner, 180, 112, gnx::gfx::FontSize::Title,
                         gnx::gfx::kText);
                gfx.text(std::to_string(visibleGames.size()) + " / " +
                             std::to_string(sourceTotal) + " " +
                             ui("juegos", "games"),
                         1440, 118,
                         gnx::gfx::FontSize::Body, gnx::gfx::kFocus);

                const auto drawTab = [&](LibraryTab tab, int x,
                                         const std::string& label) {
                    SDL_Rect tabRect = {x, 160, 170, 48};
                    const bool active = libraryTab == tab;
                    gfx.fill(tabRect, active ? gnx::gfx::kSurfaceHi
                                             : gnx::gfx::kChip);
                    gfx.frame(tabRect, active ? gnx::gfx::kFocus
                                              : gnx::gfx::kChipEdge,
                              active ? 3 : 1);
                    gfx.text_centered(label, tabRect.x + tabRect.w / 2,
                                      tabRect.y + 10,
                                      gnx::gfx::FontSize::Small,
                                      active ? gnx::gfx::kText
                                             : gnx::gfx::kTextDim);
                };
                drawTab(LibraryTab::MyGames, 180,
                        ui("MIS JUEGOS", "MY GAMES"));
                drawTab(LibraryTab::Favorites, 365,
                        ui("FAVORITOS", "FAVORITES"));
                drawTab(LibraryTab::Recent, 550,
                        ui("RECIENTES", "RECENT"));
                drawTab(LibraryTab::Catalog, 735,
                        ui("CATALOGO", "CATALOG"));
                drawTab(LibraryTab::InstallAndPlay, 920,
                        ui("INSTALAR", "INSTALL"));

                SDL_Rect searchRect = {1120, 160, 620, 48};
                gfx.fill(searchRect, gnx::gfx::kBar);
                gfx.frame(searchRect, searchQuery.empty()
                    ? gnx::gfx::kChipEdge : gnx::gfx::kFocus, 2);
                const std::string searchLabel = searchQuery.empty()
                    ? ui("(Y) Buscar juegos", "(Y) Search games")
                    : std::string(ui("Busqueda: ", "Search: ")) + searchQuery;
                gfx.text(fit_text(gfx, searchLabel, searchRect.w - 35),
                         searchRect.x + 18, searchRect.y + 10,
                         gnx::gfx::FontSize::Small,
                         searchQuery.empty() ? gnx::gfx::kTextDim
                                             : gnx::gfx::kText);

                gfx.text(ui("ZL/ZR: seccion   L/R: pagina   A: jugar/agregar   X: favorito   R3: recargar",
                            "ZL/ZR: section   L/R: page   A: play/add   X: favorite   R3: refresh"),
                         180, 218, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                gfx.text(fit_text(gfx,
                            ui("MOUSE: trackpad relativo; toque rapido=clic; cursor visible 3 s; X tactil: STEAM/ALT+TAB",
                               "MOUSE: relative trackpad; quick tap=click; cursor visible 3 s; touch X: STEAM/ALT+TAB"),
                            1560, gnx::gfx::FontSize::Small),
                         180, 246, gnx::gfx::FontSize::Small,
                         gnx::gfx::kFocus);

                if (visibleGames.empty()) {
                    SDL_Rect emptyRect = {300, 350, 1320, 350};
                    gfx.fill(emptyRect, gnx::gfx::kSurface);
                    gfx.frame(emptyRect, gnx::gfx::kChipEdge, 2);
                    std::string emptyTitle;
                    std::string emptyHelp;
                    if (catalogView && !catalogOk) {
                        emptyTitle = ui("No se pudo cargar el catalogo completo",
                                        "Could not load the full catalog");
                        emptyHelp = catalogError.empty()
                            ? ui("Presiona R3 para reintentar.",
                                 "Press R3 to retry.")
                            : catalogError;
                    } else if (!catalogView && !libraryOk) {
                        emptyTitle = ui("No se pudo cargar Mis juegos",
                                        "Could not load My games");
                        emptyHelp = libraryError.empty()
                            ? ui("Presiona R3 para reintentar.",
                                 "Press R3 to retry.")
                            : libraryError;
                    } else if (!searchQuery.empty()) {
                        emptyTitle = ui("No encontramos juegos con esa busqueda",
                                        "No games matched your search");
                        emptyHelp = ui("Presiona (Y) y borra el texto para mostrar todo.",
                                       "Press (Y) and clear the text to show everything.");
                    } else if (libraryTab == LibraryTab::Favorites) {
                        emptyTitle = ui("Todavia no tienes favoritos",
                                        "You do not have favorites yet");
                        emptyHelp = ui("En cualquier seccion, presiona (X) sobre un juego.",
                                       "Press (X) on a game in any section.");
                    } else if (libraryTab == LibraryTab::Recent) {
                        emptyTitle = ui("Todavia no hay juegos recientes",
                                        "There are no recent games yet");
                        emptyHelp = ui("Los juegos apareceran aqui cuando los inicies.",
                                       "Games appear here after you launch them.");
                    } else if (libraryTab == LibraryTab::InstallAndPlay) {
                        emptyTitle = ui("No se encontro la coleccion Instalar y jugar",
                                        "No Install and Play collection was found");
                        emptyHelp = ui("El catalogo completo sigue disponible en CATALOGO.",
                                       "The full catalog remains available in CATALOG.");
                    } else if (libraryTab == LibraryTab::Catalog) {
                        emptyTitle = ui("El catalogo esta vacio",
                                        "The catalog is empty");
                        emptyHelp = ui("Presiona R3 para volver a consultar Boosteroid.",
                                       "Press R3 to query Boosteroid again.");
                    } else {
                        emptyTitle = ui("Todavia no hay juegos en Mis juegos",
                                        "There are no games in My games yet");
                        emptyHelp = ui("Abre CATALOGO y presiona A para agregar uno.",
                                       "Open CATALOG and press A to add one.");
                    }
                    gfx.text_centered(emptyTitle,
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 105,
                                      gnx::gfx::FontSize::Title,
                                      gnx::gfx::kText);
                    gfx.text_centered(emptyHelp,
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 215,
                                      gnx::gfx::FontSize::Body,
                                      gnx::gfx::kTextDim);
                } else {
                    const std::size_t pageStart =
                        (selectedGame / static_cast<std::size_t>(kPageSize)) *
                        kPageSize;
                    const std::size_t pageEnd = std::min(
                        visibleGames.size(),
                        pageStart + static_cast<std::size_t>(kPageSize));
                    for (std::size_t index = pageStart; index < pageEnd; ++index) {
                        const int slot = static_cast<int>(index - pageStart);
                        const int column = slot % kColumns;
                        const int row = slot / kColumns;
                        SDL_Rect card = {
                            kGridX + column * (kCardWidth + kGapX),
                            kGridY + row * (kCardHeight + kGapY),
                            kCardWidth,
                            kCardHeight,
                        };
                        const bool selected = index == selectedGame;
                        gfx.fill(card, selected ? gnx::gfx::kSurfaceHi
                                                : gnx::gfx::kSurface);
                        gfx.frame(card, selected ? gnx::gfx::kFocus
                                                 : gnx::gfx::kChipEdge,
                                  selected ? 4 : 2);

                        ZERODROID::GameItem* gamePtr =
                            gameFromVisible(visibleGames[index]);
                        if (!gamePtr) continue;
                        const ZERODROID::GameItem& game = *gamePtr;
                        // Cards are horizontal, so use Boosteroid's banner/hero
                        // artwork first. The v084 cache namespace prevents old
                        // low-resolution portrait downloads from being reused.
                        const std::string& coverUrl = game.bannerUrl.empty()
                            ? game.posterUrl
                            : game.bannerUrl;
                        const std::string coverKey =
                            "v084-banner-" + std::to_string(game.id);
                        SDL_Rect coverRect = {card.x + 8, card.y + 8,
                                              card.w - 16, 174};
                        gfx.fill(coverRect, gnx::gfx::kChip);
                        SDL_Texture* cover = covers->get(coverKey, coverUrl);
                        if (cover) {
                            draw_cover_texture(gfx.renderer(), cover, coverRect);
                        } else {
                            const bool unavailable = coverUrl.empty() ||
                                                     covers->has_result(coverKey);
                            gfx.text_centered(
                                unavailable ? ui("Sin caratula", "No cover")
                                            : ui("Cargando imagen", "Loading image"),
                                coverRect.x + coverRect.w / 2,
                                coverRect.y + 68,
                                gnx::gfx::FontSize::Small,
                                gnx::gfx::kFaint);
                        }

                        if (contains_game_id(serverSettings.favoriteGameIds,
                                             game.id)) {
                            SDL_Rect favoriteChip = {
                                card.x + card.w - 74, card.y + 16, 58, 38};
                            gfx.fill(favoriteChip, gnx::gfx::Color{104, 67, 145});
                            gfx.frame(favoriteChip, gnx::gfx::kFocus, 1);
                            gfx.text_centered("FAV",
                                              favoriteChip.x + favoriteChip.w / 2,
                                              favoriteChip.y + 7,
                                              gnx::gfx::FontSize::Small,
                                              gnx::gfx::kText);
                        }

                        if (catalogView) {
                            const char* chipText =
                                gameActionInFlight && game.id == gameActionId
                                    ? ui("AGREGANDO", "ADDING")
                                    : (game.isInstalled
                                        ? ui("MI JUEGO", "MY GAME")
                                        : (game.installAndPlay
                                            ? ui("INSTALAR", "INSTALL")
                                            : (game.isFree
                                                ? ui("GRATIS", "FREE")
                                                : ui("LICENCIA", "LICENSE"))));
                            SDL_Rect stateChip = {
                                card.x + 12, card.y + 144, 132, 34};
                            gfx.fill(stateChip, game.isInstalled
                                ? gnx::gfx::Color{64, 115, 101}
                                : gnx::gfx::Color{67, 55, 92});
                            gfx.frame(stateChip, game.isInstalled
                                ? gnx::gfx::kFocus : gnx::gfx::kChipEdge, 1);
                            gfx.text_centered(chipText,
                                              stateChip.x + stateChip.w / 2,
                                              stateChip.y + 5,
                                              gnx::gfx::FontSize::Small,
                                              gnx::gfx::kText);
                        }

                        const auto titleLines = wrap_text_lines(
                            gfx, game.title, card.w - 22, 2,
                            gnx::gfx::FontSize::Small);
                        for (std::size_t line = 0;
                             line < titleLines.size(); ++line) {
                            gfx.text(titleLines[line], card.x + 11,
                                     card.y + 192 + static_cast<int>(line) * 29,
                                     gnx::gfx::FontSize::Small,
                                     gnx::gfx::kText);
                        }
                        std::string platform = game.store.empty()
                            ? game.platform : game.store;
                        if (platform.empty()) platform = "Boosteroid Cloud";
                        gfx.text(fit_text(gfx, platform, card.w - 22),
                                 card.x + 11, card.y + 274,
                                 gnx::gfx::FontSize::Small,
                                 selected ? gnx::gfx::kFocus
                                          : gnx::gfx::kTextDim);
                    }

                    const std::size_t page = selectedGame / kPageSize + 1;
                    const std::size_t pages =
                        (visibleGames.size() + kPageSize - 1) / kPageSize;
                    gfx.text(std::string(ui("Pagina ", "Page ")) +
                                 std::to_string(page) + " " + ui("de", "of") +
                                 " " + std::to_string(pages),
                             180, 946, gnx::gfx::FontSize::Small,
                             gnx::gfx::kTextDim);
                }

                if (!libraryNotice.empty()) {
                    gfx.text(fit_text(gfx, libraryNotice, 1180), 560, 946,
                             gnx::gfx::FontSize::Small, gnx::gfx::kWarn);
                }
            }
        }

        // Draw Footer Glass Bar
        SDL_Rect footerRect = {0, gnx::gfx::kHeight - 70, gnx::gfx::kWidth, 70};
        gfx.fill(footerRect, gnx::gfx::kBar);
        gfx.text(hasSession
                      ? (settingsOpen
                           ? ui("Arriba/abajo: opcion   Izq./der.: cambiar   (A) Elegir   (B) Volver",
                                "Up/down: option   Left/right: change   (A) Select   (B) Back")
                           : (launchPanel
                                ? ui("(B) Cancelar inicio   (+) Salir",
                                     "(B) Cancel launch   (+) Exit")
                                : ui("(Y) Buscar   (X) Favorito   Mouse: tactil/(-)+stick   X tactil: sesion",
                                     "(Y) Search   (X) Favorite   Mouse: touch/(-)+stick   touch X: session")))
                     : ui("(Y) Renovar QR   (+) Salir al menu principal",
                          "(Y) Refresh QR   (+) Exit to HOME menu"),
                 40, gnx::gfx::kHeight - 50, gnx::gfx::FontSize::Body,
                 gnx::gfx::kTextDim);

        // End Graphical Frame & Present SDL2 Buffer
        gfx.end_frame();
    }

    if (pollInFlight) pollFuture.wait();
    if (libraryInFlight) libraryFuture.wait();
    if (gameActionInFlight) gameActionFuture.wait();
    if (serverConfigInFlight) serverConfigFuture.wait();
    if (serverSaveInFlight) serverSaveFuture.wait();
    std::fprintf(stderr, "ZERODROID shutting down\n");
    if (dekoStreaming) {
        stream->end_deko_output();
        stream->stop();
        gfx.resume();
    }
    stream.reset();
    gnx::stream::Engine::global_shutdown();
    covers.reset();
    gfx.shutdown();
    gnx::Http::global_cleanup();
    if (romfsReady) romfsExit();
    if (plReady) plExit();
    if (socketsReady) socketExit();
    return 0;
}
