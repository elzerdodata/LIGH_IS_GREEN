#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
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
    Catalog = 1,
    Favorites = 2,
    Recent = 3,
};

// One order for every library entry point: sidebar, top tabs, ZL/ZR and touch.
// L/R intentionally remain page controls, not section controls.
constexpr std::array<LibraryTab, 4> kLibraryTabOrder{{
    LibraryTab::MyGames,
    LibraryTab::Catalog,
    LibraryTab::Favorites,
    LibraryTab::Recent,
}};

// These are design-space touch targets (1920x1080) shared by rendering and
// hit testing. 66 design pixels map to the Switch's 44-pixel touch minimum.
constexpr std::array<SDL_Rect, 4> kLibrarySidebarTabRects{{
    {48, 164, 198, 66},
    {48, 242, 198, 66},
    {48, 320, 198, 66},
    {48, 398, 198, 66},
}};
constexpr std::array<SDL_Rect, 4> kLibraryTopTabRects{{
    {300, 255, 160, 48},
    {472, 255, 160, 48},
    {644, 255, 160, 48},
    {816, 255, 160, 48},
}};
constexpr SDL_Rect kLibrarySidebarSearchRect{48, 476, 198, 66};
constexpr SDL_Rect kLibrarySidebarSettingsRect{48, 554, 198, 66};
constexpr SDL_Rect kLibrarySearchRect{1245, 255, 585, 48};

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
        {"version", 7},
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
    if (maxWidth <= 0 || text.empty()) return {};
    if (gfx.text_width(text, size) <= maxWidth) return text;

    std::string shortened = text;
    constexpr const char* suffix = "...";
    if (gfx.text_width(suffix, size) > maxWidth) return {};
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

bool point_in_rect(int x, int y, const SDL_Rect& rect) {
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

    // Crop the source, never stretch it: the target is completely filled
    // without the black letterboxing produced by a portrait image inside a
    // landscape slot. Card artwork itself now uses a portrait-sized slot.
    SDL_Rect source = {0, 0, width, height};
    const long long sourceRatio = static_cast<long long>(width) * destination.h;
    const long long targetRatio = static_cast<long long>(height) * destination.w;
    if (sourceRatio > targetRatio) {
        source.w = std::max(1, height * destination.w / destination.h);
        source.x = (width - source.w) / 2;
    } else if (sourceRatio < targetRatio) {
        source.h = std::max(1, width * destination.h / destination.w);
        source.y = (height - source.h) / 2;
    }
    SDL_RenderCopy(renderer, texture, &source, &destination);
}

// The library/login renderer intentionally stays simple enough for the Switch:
// composited navy surfaces and a few alpha layers create depth without a blur
// pass, a second render target, or a runtime dependency on the mockup images.
void draw_ambient_background(gnx::gfx::Gfx& gfx) {
    using gnx::gfx::Color;
    for (int band = 0; band < 9; ++band) {
        const int y = band * gnx::gfx::kHeight / 9;
        const Uint8 alpha = static_cast<Uint8>(8 + (8 - band) * 2);
        gfx.fill({0, y, gnx::gfx::kWidth, gnx::gfx::kHeight / 9 + 1},
                 Color{22, 36, 72, alpha});
    }
    // Subtle nebula bands reserve lilac for meaning while making empty states
    // feel like part of the product rather than a bare debug screen.
    gfx.fill({0, 110, gnx::gfx::kWidth, 3}, Color{139, 92, 246, 18});
    gfx.fill({0, 996, gnx::gfx::kWidth, 2}, Color{45, 212, 191, 16});
    gfx.fill({1560, 130, 330, 660}, Color{74, 45, 138, 10});
    gfx.fill({85, 610, 260, 290}, Color{18, 104, 124, 9});
}

void draw_brand_mark(gnx::gfx::Gfx& gfx, int x, int y) {
    using namespace gnx::gfx;
    const SDL_Rect glow = {x - 8, y - 8, 76, 76};
    gfx.rounded_fill(glow, 24, Color{139, 92, 246, 22});
    const SDL_Rect mark = {x, y, 60, 60};
    gfx.rounded_panel(mark, 19, Color{37, 23, 73, 255}, kAccent, 2);
    gfx.text_centered("ZD", mark.x + mark.w / 2, mark.y + 14,
                      FontSize::Body, kText);
}

void draw_navigation_item(gnx::gfx::Gfx& gfx, const SDL_Rect& rect,
                          const std::string& label, bool active) {
    using namespace gnx::gfx;
    if (active) {
        gfx.rounded_fill({rect.x - 6, rect.y - 5, rect.w + 12, rect.h + 10},
                         17, Color{139, 92, 246, 22});
        gfx.rounded_panel(rect, 14, Color{42, 27, 80, 248}, kAccent, 2);
        gfx.fill({rect.x + 8, rect.y + 13, 4, rect.h - 26}, kFocus);
        gfx.circle(rect.x + 30, rect.y + rect.h / 2, 7, kFocus);
    } else {
        gfx.rounded_fill(rect, 14, Color{10, 20, 37, 92});
        gfx.circle(rect.x + 30, rect.y + rect.h / 2, 6, kFaint);
    }
    gfx.text(fit_text(gfx, label, rect.w - 62, FontSize::Small),
             rect.x + 50, rect.y + 18, FontSize::Small,
             active ? kText : kTextDim);
}

void draw_footer_control(gnx::gfx::Gfx& gfx, int x, const char* button,
                         const std::string& label, bool emphasized = false) {
    using namespace gnx::gfx;
    gfx.circle(x + 13, kHeight - 38, 13,
               emphasized ? kFocus : Color{148, 163, 184, 255});
    gfx.text_centered(button, x + 13, kHeight - 49, FontSize::Small,
                      kBg);
    gfx.text(label, x + 36, kHeight - 52, FontSize::Small,
             emphasized ? kText : kTextDim);
}

void draw_state_pill(gnx::gfx::Gfx& gfx, const SDL_Rect& rect,
                     const std::string& label, bool positive) {
    using namespace gnx::gfx;
    const Color edge = positive ? kConnected : kChipEdge;
    const Color fill = positive ? Color{9, 60, 67, 230}
                                : Color{17, 29, 50, 230};
    gfx.rounded_panel(rect, rect.h / 2, fill, edge, 1);
    gfx.circle(rect.x + 22, rect.y + rect.h / 2, 6, edge);
    gfx.text(fit_text(gfx, label, rect.w - 50, FontSize::Small),
             rect.x + 38, rect.y + 10, FontSize::Small,
             positive ? kConnected : kTextDim);
}

void draw_section_title(gnx::gfx::Gfx& gfx, const std::string& eyebrow,
                        const std::string& title, const std::string& subtitle) {
    using namespace gnx::gfx;
    gfx.text(eyebrow, 300, 129, FontSize::Small, kAccent);
    gfx.text(fit_text(gfx, title, 860, FontSize::Title),
             300, 160, FontSize::Title, kText);
    if (!subtitle.empty()) {
        gfx.text(fit_text(gfx, subtitle, 900, FontSize::Small),
                 300, 215, FontSize::Small, kTextDim);
    }
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
    // Library UI touch is deliberately independent from the streaming
    // trackpad state above. The stream branch always continues before this
    // state is evaluated, so a stream gesture can never activate the sidebar.
    bool uiTouchWasDown = false;
    int uiTouchLastX = 0;
    int uiTouchLastY = 0;
    int uiTouchTravel = 0;
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
        } else if (libraryTab == LibraryTab::Catalog) {
            for (std::size_t index = 0; index < catalogGames.size(); ++index) {
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

    // A single state transition keeps physical controls, top tabs and the
    // sidebar consistent. Selecting a section deliberately exits search so a
    // visible tab always means the complete selected section is on screen.
    const auto selectLibraryTab = [&](LibraryTab tab) {
        libraryTab = tab;
        searchQuery.clear();
        selectedGame = 0;
        rebuildVisibleGames();
    };

    const auto cycleLibraryTab = [&](int direction) {
        const auto found = std::find(kLibraryTabOrder.begin(),
                                     kLibraryTabOrder.end(), libraryTab);
        const std::size_t current = found == kLibraryTabOrder.end()
            ? 0
            : static_cast<std::size_t>(
                std::distance(kLibraryTabOrder.begin(), found));
        const int count = static_cast<int>(kLibraryTabOrder.size());
        const int next = (static_cast<int>(current) + direction + count) % count;
        selectLibraryTab(kLibraryTabOrder[static_cast<std::size_t>(next)]);
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

    const auto openLibrarySearch = [&]() {
        searchQuery = open_search_keyboard(searchQuery, serverSettings.language);
        selectedGame = 0;
        rebuildVisibleGames();
    };

    const auto openSettings = [&]() {
        settingsOpen = true;
        selectedSettingsRow = 0;
        if (!serverConfigReady && !serverConfigInFlight) {
            startServerConfigLoad();
        }
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

        // Native library navigation is tap-on-release, rather than action on
        // touchdown, so a finger moving through the rail cannot change the
        // active section accidentally. This lives outside dekoStreaming: the
        // stream branch always continues before reaching it.
        HidTouchScreenState uiTouchState{};
        const bool uiTouching =
            hidGetTouchScreenStates(&uiTouchState, 1) > 0 &&
            uiTouchState.count > 0;
        if (uiTouching) {
            const int x = uiTouchState.touches[0].x * gnx::gfx::kWidth / 1280;
            const int y = uiTouchState.touches[0].y * gnx::gfx::kHeight / 720;
            if (!uiTouchWasDown) {
                uiTouchWasDown = true;
                uiTouchLastX = x;
                uiTouchLastY = y;
                uiTouchTravel = 0;
            } else if (x != uiTouchLastX || y != uiTouchLastY) {
                uiTouchTravel += std::abs(x - uiTouchLastX) +
                                 std::abs(y - uiTouchLastY);
                uiTouchLastX = x;
                uiTouchLastY = y;
            }
        } else if (uiTouchWasDown) {
            uiTouchWasDown = false;
            constexpr int kUiTapTravelLimit = 32;
            const bool isTap = uiTouchTravel <= kUiTapTravelLimit;
            const int tapX = uiTouchLastX;
            const int tapY = uiTouchLastY;
            uiTouchTravel = 0;

            if (isTap && hasSession && !launchPanel) {
                if (point_in_rect(tapX, tapY, kLibrarySidebarSettingsRect)) {
                    openSettings();
                } else if (libraryReady && (libraryOk || catalogOk)) {
                    bool selectedSection = false;
                    for (std::size_t index = 0;
                         index < kLibrarySidebarTabRects.size(); ++index) {
                        if (!point_in_rect(tapX, tapY,
                                           kLibrarySidebarTabRects[index]) &&
                            !point_in_rect(tapX, tapY,
                                           kLibraryTopTabRects[index])) {
                            continue;
                        }
                        settingsOpen = false;
                        selectLibraryTab(kLibraryTabOrder[index]);
                        selectedSection = true;
                        break;
                    }
                    if (!selectedSection &&
                        (point_in_rect(tapX, tapY, kLibrarySidebarSearchRect) ||
                         point_in_rect(tapX, tapY, kLibrarySearchRect))) {
                        settingsOpen = false;
                        openLibrarySearch();
                    }
                }
            }
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
            openSettings();
        }

        if (!settingsOpen && !launchPanel && hasSession && libraryReady &&
            (libraryOk || catalogOk)) {
            constexpr std::size_t kColumns = 4;
            constexpr std::size_t kPageSize = 8;
            if (kDown & HidNpadButton_ZL) {
                cycleLibraryTab(-1);
            } else if (kDown & HidNpadButton_ZR) {
                cycleLibraryTab(1);
            }
            if (kDown & HidNpadButton_Y) {
                openLibrarySearch();
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

        draw_ambient_background(gfx);
        gfx.rounded_panel({18, 14, 1884, 92}, 24,
                          gnx::gfx::Color{7, 15, 29, 246},
                          gnx::gfx::Color{35, 53, 79, 190}, 1);
        draw_brand_mark(gfx, 42, 30);
        gfx.text("ZERODROID", 122, 35, gnx::gfx::FontSize::Title,
                 gnx::gfx::kText);
        gfx.text(std::string("CLOUD GAMING FOR NINTENDO SWITCH  |  v") +
                     ZERODROID_VERSION,
                 124, 80, gnx::gfx::FontSize::Small,
                 gnx::gfx::kTextDim);
        draw_state_pill(gfx, {520, 36, 214, 44},
                        hasSession
                            ? ui("SESION LISTA", "SESSION READY")
                            : ui("INICIO SEGURO", "SECURE SIGN IN"),
                        hasSession);
        gfx.text(hasSession ? ui("Cloud Ready", "Cloud Ready")
                            : ui("Vincula tu cuenta", "Link your account"),
                 760, 46, gnx::gfx::FontSize::Small,
                 hasSession ? gnx::gfx::kConnected : gnx::gfx::kTextDim);
        if (hasSession) {
            const std::string account = libraryUser.nickname.empty()
                ? ui("Cuenta Boosteroid", "Boosteroid account")
                : fit_text(gfx, libraryUser.nickname, 370,
                           gnx::gfx::FontSize::Body);
            gfx.text(account, 1480, 35, gnx::gfx::FontSize::Body,
                     gnx::gfx::kText);
            gfx.text(ui("(-) Configuracion", "(-) Settings"), 1480, 74,
                     gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
        }

        if (!hasSession) {
            const SDL_Rect cardRect = {190, 170, 1540, 730};
            gfx.rounded_fill({cardRect.x - 12, cardRect.y - 12,
                              cardRect.w + 24, cardRect.h + 24},
                             34, gnx::gfx::Color{139, 92, 246, 18});
            gfx.rounded_panel(cardRect, 28, gnx::gfx::kSurface,
                              gnx::gfx::kAccent, 2);
            gfx.text(ui("INICIO DE SESION", "SIGN IN"), 275, 245,
                     gnx::gfx::FontSize::Small, gnx::gfx::kAccent);
            gfx.text(ui("Conecta tu cuenta", "Connect your account"),
                     275, 285, gnx::gfx::FontSize::Huge, gnx::gfx::kText);
            gfx.text(ui("Escanea el codigo desde tu movil y confirma en Boosteroid.",
                        "Scan the code with your phone and confirm in Boosteroid."),
                     278, 390, gnx::gfx::FontSize::Body,
                     gnx::gfx::kTextDim);
            draw_state_pill(gfx, {275, 475, 610, 58},
                            fit_text(gfx, loginStatus, 540,
                                     gnx::gfx::FontSize::Small),
                            false);
            gfx.text(ui("1. Escanea el QR", "1. Scan the QR"), 280, 600,
                     gnx::gfx::FontSize::Body, gnx::gfx::kText);
            gfx.text(ui("2. Inicia sesion en el navegador", "2. Sign in in your browser"),
                     280, 654, gnx::gfx::FontSize::Body, gnx::gfx::kText);
            gfx.text(ui("3. Confirma la vinculacion", "3. Confirm the link"),
                     280, 708, gnx::gfx::FontSize::Body, gnx::gfx::kText);
            gfx.text(ui("El codigo se renueva solo. Pulsa (Y) para renovarlo ahora.",
                        "The code refreshes automatically. Press (Y) to refresh it now."),
                     280, 800, gnx::gfx::FontSize::Small,
                     gnx::gfx::kTextDim);

            const SDL_Rect qrPanel = {1195, 245, 390, 490};
            gfx.rounded_panel(qrPanel, 24, gnx::gfx::Color{12, 20, 35, 255},
                              gnx::gfx::Color{87, 67, 141, 255}, 2);
            gfx.text_centered(ui("ESCANEA PARA CONTINUAR", "SCAN TO CONTINUE"),
                              qrPanel.x + qrPanel.w / 2, qrPanel.y + 28,
                              gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
            draw_qr_code(gfx.renderer(), qrPanel.x + 43, qrPanel.y + 76, 304,
                         authArtifact.qrUrl);
            gfx.text_centered(ui("Boosteroid secure link", "Boosteroid secure link"),
                              qrPanel.x + qrPanel.w / 2, qrPanel.y + 410,
                              gnx::gfx::FontSize::Small, gnx::gfx::kConnected);
        } else {
            // Persistent controller-first navigation. The same four tabs are
            // drawn, touched and cycled by ZL/ZR in the same order.
            const SDL_Rect sidebar = {34, 132, 226, 804};
            gfx.rounded_panel(sidebar, 24, gnx::gfx::Color{8, 18, 33, 230},
                              gnx::gfx::Color{35, 53, 79, 210}, 1);
            const std::array<std::string, 4> sidebarLabels{{
                ui("MIS JUEGOS", "MY GAMES"),
                ui("CATALOGO", "CATALOG"),
                ui("FAVORITOS", "FAVORITES"),
                ui("RECIENTES", "RECENT"),
            }};
            for (std::size_t index = 0; index < kLibraryTabOrder.size(); ++index) {
                draw_navigation_item(
                    gfx, kLibrarySidebarTabRects[index], sidebarLabels[index],
                    !settingsOpen && searchQuery.empty() &&
                        libraryTab == kLibraryTabOrder[index]);
            }
            draw_navigation_item(gfx, kLibrarySidebarSearchRect,
                                 ui("BUSCAR", "SEARCH"),
                                 !settingsOpen && !searchQuery.empty());
            draw_navigation_item(gfx, kLibrarySidebarSettingsRect,
                                 ui("AJUSTES", "SETTINGS"), settingsOpen);
            gfx.line(sidebar.x + 20, 694, sidebar.x + sidebar.w - 20, 694,
                     gnx::gfx::Color{49, 65, 91, 180});
            gfx.text(ui("REGION", "REGION"), sidebar.x + 26, 720,
                     gnx::gfx::FontSize::Small, gnx::gfx::kFaint);
            gfx.text(fit_text(gfx, serverSettings.preferredLocationLabel,
                              sidebar.w - 52),
                     sidebar.x + 26, 752, gnx::gfx::FontSize::Small,
                     gnx::gfx::kTextDim);
            draw_state_pill(gfx, {sidebar.x + 22, 810, sidebar.w - 44, 42},
                            ui("LISTO", "READY"), true);

            if (settingsOpen) {
                draw_section_title(gfx, "SETTINGS / SYSTEM",
                                   ui("Configuracion", "Settings"),
                                   ui("Ajusta solo opciones disponibles en tu cuenta y consola.",
                                      "Adjust only options available to your account and console."));
                const auto drawSettingsTile = [&](const SDL_Rect& rect,
                                                  int index,
                                                  const std::string& heading,
                                                  const std::string& help,
                                                  gnx::gfx::Color emphasis =
                                                      gnx::gfx::kAccent) {
                    const bool selected = selectedSettingsRow == index;
                    if (selected) {
                        gfx.rounded_fill({rect.x - 6, rect.y - 6,
                                          rect.w + 12, rect.h + 12}, 22,
                                         gnx::gfx::Color{139, 92, 246, 18});
                    }
                    gfx.rounded_panel(rect, 18,
                                      selected ? gnx::gfx::kSurfaceHi
                                               : gnx::gfx::kSurface,
                                      selected ? emphasis : gnx::gfx::kChipEdge,
                                      selected ? 2 : 1);
                    gfx.text(heading, rect.x + 24, rect.y + 22,
                             gnx::gfx::FontSize::Body, gnx::gfx::kText);
                    gfx.text(fit_text(gfx, help, rect.w - 48), rect.x + 24,
                             rect.y + 66, gnx::gfx::FontSize::Small,
                             gnx::gfx::kTextDim);
                    if (selected) {
                        gfx.text(ui("ENFOQUE", "FOCUS"),
                                 rect.x + rect.w - 98, rect.y + 24,
                                 gnx::gfx::FontSize::Small,
                                 gnx::gfx::kFocus);
                    }
                };

                const SDL_Rect regionsTile = {300, 285, 475, 205};
                const SDL_Rect videoTile = {300, 520, 475, 330};
                const SDL_Rect controllerTile = {800, 285, 475, 205};
                const SDL_Rect serverTile = {800, 520, 475, 330};
                const SDL_Rect languageTile = {1300, 285, 530, 205};
                const SDL_Rect logoutTile = {1300, 520, 530, 330};

                drawSettingsTile(regionsTile, 0,
                                 ui("REGION Y DISPONIBILIDAD", "REGION & AVAILABILITY"),
                                 ui("Amplia la busqueda de maquinas disponibles.",
                                    "Expand the search for available machines."),
                                 gnx::gfx::kConnected);
                draw_state_pill(gfx, {regionsTile.x + 24, regionsTile.y + 124,
                                      260, 50},
                                serverSettings.allowDistantRegions
                                    ? ui("REGIONES LEJANAS: ON", "DISTANT REGIONS: ON")
                                    : ui("SOLO MI REGION", "MY REGION ONLY"),
                                serverSettings.allowDistantRegions);
                gfx.text(ui("(A) cambiar", "(A) change"), regionsTile.x + 305,
                         regionsTile.y + 140, gnx::gfx::FontSize::Small,
                         gnx::gfx::kFocus);

                drawSettingsTile(videoTile, 2,
                                 ui("CALIDAD DE STREAM", "STREAM QUALITY"),
                                 ui("La resolucion se aplica al iniciar el proximo juego.",
                                    "Resolution applies when the next game starts."));
                const char* compactRes[4] = {"AUTO", "720P", "1080P", "1440P"};
                for (int mode = 0; mode < 4; ++mode) {
                    SDL_Rect chip = {videoTile.x + 24 + mode * 108,
                                     videoTile.y + 126, 96, 50};
                    const bool active = serverSettings.resolutionMode == mode;
                    gfx.rounded_panel(chip, 12,
                                      active ? gnx::gfx::Color{86, 47, 183, 255}
                                             : gnx::gfx::kChip,
                                      active ? gnx::gfx::kFocus
                                             : gnx::gfx::kChipEdge,
                                      active ? 2 : 1);
                    gfx.text_centered(compactRes[mode], chip.x + chip.w / 2,
                                      chip.y + 12, gnx::gfx::FontSize::Small,
                                      active ? gnx::gfx::kText : gnx::gfx::kTextDim);
                }
                const char* presetNames[6] = {
                    "Natural", "Sharp", "Vivid", "Cinema", "Soft", "Custom"};
                gfx.text(std::string(ui("Perfil: ", "Profile: ")) +
                             presetNames[std::clamp(quickMenuState.picturePreset, 0, 5)],
                         videoTile.x + 24, videoTile.y + 205,
                         gnx::gfx::FontSize::Small, gnx::gfx::kConnected);
                gfx.text(ui("1440p es experimental y se reescala a la pantalla.",
                            "1440p is experimental and downscales to the display."),
                         videoTile.x + 24, videoTile.y + 251,
                         gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);

                drawSettingsTile(controllerTile, 1,
                                 ui("PERFIL DEL MANDO", "CONTROLLER PROFILE"),
                                 ui("Alterna las etiquetas Nintendo y la posicion Xbox.",
                                    "Toggle Nintendo labels and physical Xbox positions."));
                draw_state_pill(gfx, {controllerTile.x + 24,
                                      controllerTile.y + 124, 244, 50},
                                serverSettings.xboxFaceLayout ? "XBOX" : "NINTENDO",
                                serverSettings.xboxFaceLayout);
                gfx.text(ui("(A) cambiar", "(A) change"),
                         controllerTile.x + 300, controllerTile.y + 140,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);

                drawSettingsTile(serverTile, 3,
                                 ui("SERVIDOR PREFERIDO", "PREFERRED SERVER"),
                                 ui("Se guarda en Boosteroid para el proximo inicio.",
                                    "Saved in Boosteroid for the next launch."),
                                 gnx::gfx::kConnected);
                std::string locationLabel = ui("Automatico (recomendado)",
                                               "Automatic (recommended)");
                if (selectedLocation > 0 && selectedLocation <= serverLocations.size()) {
                    const auto& location = serverLocations[selectedLocation - 1];
                    locationLabel = location.title;
                    if (!location.country.empty()) locationLabel += " (" + location.country + ")";
                }
                gfx.rounded_panel({serverTile.x + 24, serverTile.y + 126,
                                   serverTile.w - 48, 58}, 13,
                                  gnx::gfx::kChip, gnx::gfx::kChipEdge, 1);
                gfx.text("<", serverTile.x + 42, serverTile.y + 140,
                         gnx::gfx::FontSize::Body, gnx::gfx::kFocus);
                gfx.text_centered(fit_text(gfx, locationLabel, serverTile.w - 160),
                                  serverTile.x + serverTile.w / 2,
                                  serverTile.y + 142, gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);
                gfx.text(">", serverTile.x + serverTile.w - 52,
                         serverTile.y + 140, gnx::gfx::FontSize::Body,
                         gnx::gfx::kFocus);
                gfx.text(ui("(Y) actualizar regiones oficiales", "(Y) refresh official regions"),
                         serverTile.x + 24, serverTile.y + 213,
                         gnx::gfx::FontSize::Small, gnx::gfx::kConnected);

                drawSettingsTile(languageTile, 4,
                                 ui("IDIOMA DE LA INTERFAZ", "INTERFACE LANGUAGE"),
                                 ui("Cambia los textos locales de ZERODROID.",
                                    "Change ZERODROID's local text."));
                draw_state_pill(gfx, {languageTile.x + 24,
                                      languageTile.y + 124, 234, 50},
                                serverSettings.language == 1 ? "ENGLISH" : "ESPANOL",
                                true);
                gfx.text(ui("(A) cambiar", "(A) change"),
                         languageTile.x + 290, languageTile.y + 140,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);

                drawSettingsTile(logoutTile, 5,
                                 ui("CUENTA Y SESION", "ACCOUNT & SESSION"),
                                 ui("Cerrar sesion elimina el acceso local, no tus favoritos.",
                                    "Signing out removes local access, not your favorites."),
                                 gnx::gfx::kError);
                gfx.rounded_panel({logoutTile.x + 24, logoutTile.y + 126,
                                   logoutTile.w - 48, 64}, 13,
                                  gnx::gfx::Color{61, 26, 44, 245},
                                  gnx::gfx::kError, 1);
                gfx.text_centered(ui("CERRAR SESION", "SIGN OUT"),
                                  logoutTile.x + logoutTile.w / 2,
                                  logoutTile.y + 142,
                                  gnx::gfx::FontSize::Body,
                                  gnx::gfx::kText);
                gfx.text(ui("(A) confirma esta accion", "(A) confirms this action"),
                         logoutTile.x + 24, logoutTile.y + 218,
                         gnx::gfx::FontSize::Small, gnx::gfx::kError);

                const std::string settingsStatus = serverConfigInFlight
                    ? ui("Cargando ubicaciones oficiales de Boosteroid...",
                         "Loading official Boosteroid locations...")
                    : (serverSaveInFlight
                        ? ui("Guardando la configuracion en Boosteroid...",
                             "Saving configuration in Boosteroid...")
                        : (!serverConfigError.empty()
                            ? serverConfigError
                            : ui("Arriba/abajo: enfocar  |  Izquierda/derecha: cambiar  |  (B): volver",
                                 "Up/down: focus  |  Left/right: change  |  (B): back")));
                gfx.text(fit_text(gfx, settingsStatus, 1500), 300, 890,
                         gnx::gfx::FontSize::Small,
                         serverConfigError.empty() ? gnx::gfx::kTextDim
                                                   : gnx::gfx::kError);
            } else if (launchPanel) {
                const bool failed =
                    stream->state() == gnx::stream::EngineState::Failed;
                const gnx::gfx::Color stateColor = failed ? gnx::gfx::kError
                                                           : gnx::gfx::kFocus;
                const SDL_Rect cardRect = {470, 250, 1100, 520};
                gfx.rounded_fill({cardRect.x - 10, cardRect.y - 10,
                                  cardRect.w + 20, cardRect.h + 20}, 30,
                                 gnx::gfx::Color{139, 92, 246,
                                                 static_cast<Uint8>(failed ? 10 : 20)});
                gfx.rounded_panel(cardRect, 28, gnx::gfx::kSurface,
                                  stateColor, 2);
                gfx.text(failed ? ui("NO SE PUDO INICIAR", "COULD NOT START")
                                : ui("PREPARANDO STREAM", "PREPARING STREAM"),
                         545, 315, gnx::gfx::FontSize::Small, stateColor);
                gfx.text(failed ? ui("Revisa la conexion", "Check the connection")
                                : ui("Conectando tu sesion", "Connecting your session"),
                         545, 355, gnx::gfx::FontSize::Title,
                         gnx::gfx::kText);
                gfx.text(fit_text(gfx, launchingTitle, 800,
                                  gnx::gfx::FontSize::Body),
                         545, 440, gnx::gfx::FontSize::Body,
                         gnx::gfx::kFocus);
                const std::string streamStatus = failed
                    ? stream->error()
                    : stream->status();
                draw_state_pill(gfx, {545, 515, 755, 56},
                                fit_text(gfx, streamStatus, 675,
                                         gnx::gfx::FontSize::Small),
                                false);
                const std::string route = std::string(ui("Servidor: ", "Server: ")) +
                    serverSettings.preferredLocationLabel +
                    (serverSettings.allowDistantRegions
                        ? ui(" | regiones lejanas permitidas",
                             " | distant regions allowed")
                        : ui(" | solo mi region", " | my region only"));
                gfx.text(fit_text(gfx, route, 900), 545, 610,
                         gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
                const auto dimensions = stream_dimensions(
                    serverSettings.resolutionMode);
                const std::string videoMode = std::string(
                    ui("Video solicitado: ", "Requested video: ")) +
                    std::to_string(dimensions.first) + "x" +
                    std::to_string(dimensions.second);
                gfx.text(videoMode, 545, 652,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);
                gfx.text(ui("La asignacion de una maquina puede tardar unos minutos.",
                            "Machine assignment can take a few minutes."),
                         545, 698, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                gfx.text(ui("(B) Cancelar y volver a la biblioteca",
                            "(B) Cancel and return to the library"), 545, 740,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
            } else if (libraryInFlight || !libraryReady) {
                draw_section_title(gfx, "LIBRARY / SYNC",
                                   ui("Preparando tu biblioteca", "Preparing your library"),
                                   ui("Consultamos Mis juegos y el catalogo completo de Boosteroid.",
                                      "We are querying My games and the full Boosteroid catalog."));
                SDL_Rect cardRect = {300, 300, 1530, 560};
                gfx.rounded_panel(cardRect, 24, gnx::gfx::kSurface,
                                  gnx::gfx::kChipEdge, 1);

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                const int dotCount = static_cast<int>((elapsed / 450) % 4);
                gfx.text(std::string(ui("Sincronizando contenido", "Syncing content")) +
                             std::string(dotCount, '.'), 340, 335,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Cargando todas las paginas sin limitar el catalogo a 50 juegos.",
                            "Loading every page without limiting the catalog to 50 games."),
                         340, 380, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        const SDL_Rect ghost = {340 + column * 360,
                                                450 + row * 175, 330, 145};
                        gfx.rounded_panel(ghost, 16,
                                          gnx::gfx::Color{21, 35, 59,
                                                          static_cast<Uint8>(155 +
                                                              ((row + column + dotCount) % 3) * 20)},
                                          gnx::gfx::Color{49, 65, 91, 110}, 1);
                        gfx.rounded_fill({ghost.x + 18, ghost.y + 18,
                                          ghost.w - 36, 72}, 10,
                                         gnx::gfx::Color{44, 59, 84, 160});
                        gfx.rounded_fill({ghost.x + 18, ghost.y + 106,
                                          ghost.w - 118, 18}, 7,
                                         gnx::gfx::Color{58, 76, 105, 130});
                    }
                }
                if (!libraryNotice.empty()) {
                    gfx.text(fit_text(gfx, libraryNotice, 1400), 340, 800,
                             gnx::gfx::FontSize::Small, gnx::gfx::kWarn);
                }
            } else if (!libraryOk && !catalogOk) {
                const SDL_Rect cardRect = {485, 325, 1140, 420};
                gfx.rounded_panel(cardRect, 24, gnx::gfx::kSurface,
                                  gnx::gfx::kError, 2);
                gfx.circle(cardRect.x + 88, cardRect.y + 92, 34,
                           gnx::gfx::kError);
                gfx.text("!", cardRect.x + 80, cardRect.y + 61,
                         gnx::gfx::FontSize::Title, gnx::gfx::kBg);
                gfx.text(ui("No se pudo cargar la biblioteca",
                            "Could not load the library"),
                         cardRect.x + 160, cardRect.y + 58,
                         gnx::gfx::FontSize::Title, gnx::gfx::kText);
                gfx.text(fit_text(gfx, libraryError, 850,
                                  gnx::gfx::FontSize::Small),
                         cardRect.x + 160, cardRect.y + 142,
                         gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
                gfx.text(ui("(Y) Reintentar     (-) Configuracion     (+) Salir",
                            "(Y) Retry     (-) Settings     (+) Exit"),
                         cardRect.x + 160, cardRect.y + 300,
                         gnx::gfx::FontSize::Body, gnx::gfx::kFocus);
            } else if (libraryGames.empty() && catalogGames.empty() &&
                       libraryOk && catalogOk) {
                const SDL_Rect cardRect = {485, 325, 1140, 420};
                gfx.rounded_panel(cardRect, 24, gnx::gfx::kSurface,
                                  gnx::gfx::kFocus, 2);
                draw_state_pill(gfx, {cardRect.x + 70, cardRect.y + 70,
                                      310, 52},
                                ui("CUENTA CONECTADA", "ACCOUNT CONNECTED"), true);
                gfx.text(ui("Todavia no hay contenido disponible",
                            "No content is available yet"),
                         cardRect.x + 70, cardRect.y + 155,
                         gnx::gfx::FontSize::Title, gnx::gfx::kText);
                gfx.text(ui("Boosteroid no devolvio juegos ni catalogo para esta cuenta.",
                            "Boosteroid returned no games or catalog for this account."),
                         cardRect.x + 70, cardRect.y + 238,
                         gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
                gfx.text(ui("Pulsa R3 para volver a consultar todas las paginas.",
                            "Press R3 to query every page again."),
                         cardRect.x + 70, cardRect.y + 315,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);
            } else {
                constexpr int kColumns = 4;
                constexpr int kPageSize = kColumns * 2;
                constexpr int kCardWidth = 360;
                constexpr int kCardHeight = 282;
                constexpr int kGapX = 24;
                constexpr int kGapY = 20;
                constexpr int kGridX = 300;
                constexpr int kGridY = 348;

                const bool catalogView = libraryTab == LibraryTab::Catalog;
                const std::size_t sourceTotal = catalogView
                    ? catalogGames.size() : libraryGames.size();
                const std::string owner = catalogView
                    ? ui("Catalogo completo de Boosteroid",
                         "Full Boosteroid catalog")
                    : (libraryUser.nickname.empty()
                        ? ui("Mis juegos", "My games")
                        : std::string(ui("Juegos de ", "Games of ")) +
                              libraryUser.nickname);
                const std::string sectionEyebrow = libraryTab == LibraryTab::Catalog
                    ? ui("CATALOGO / BOOSTEROID", "CATALOG / BOOSTEROID")
                    : (libraryTab == LibraryTab::Favorites
                        ? ui("BIBLIOTECA / FAVORITOS", "LIBRARY / FAVORITES")
                        : (libraryTab == LibraryTab::Recent
                            ? ui("BIBLIOTECA / RECIENTES", "LIBRARY / RECENT")
                            : ui("INICIO / MIS JUEGOS", "HOME / MY GAMES")));
                draw_section_title(
                    gfx, sectionEyebrow,
                    fit_text(gfx, owner, 720, gnx::gfx::FontSize::Title),
                    std::to_string(visibleGames.size()) + " / " +
                        std::to_string(sourceTotal) + " " +
                        ui("juegos disponibles", "games available"));

                // A real banner from the selected title makes the library
                // feel like a console dashboard without inventing games,
                // screenshots, achievements or a profile level.
                const SDL_Rect featureRect = {1245, 128, 585, 105};
                gfx.rounded_panel(featureRect, 18,
                                  gnx::gfx::Color{15, 25, 47, 245},
                                  gnx::gfx::Color{80, 58, 133, 210}, 1);
                if (!visibleGames.empty()) {
                    const ZERODROID::GameItem* featured =
                        gameFromVisible(visibleGames[selectedGame]);
                    if (featured) {
                        const std::string& featureUrl = featured->bannerUrl.empty()
                            ? featured->posterUrl : featured->bannerUrl;
                        const std::string featureKey =
                            "v101-banner-" + std::to_string(featured->id);
                        SDL_Texture* feature = covers->get(featureKey, featureUrl);
                        if (feature) {
                            const SDL_Rect featureImageRect = {
                                featureRect.x + 6, featureRect.y + 6,
                                featureRect.w - 12, featureRect.h - 12};
                            draw_cover_texture(gfx.renderer(), feature, featureImageRect);
                            gfx.fill(featureImageRect,
                                     gnx::gfx::Color{4, 10, 20, 104});
                        }
                        gfx.text(ui("SELECCION", "SELECTED"),
                                 featureRect.x + 18, featureRect.y + 16,
                                 gnx::gfx::FontSize::Small, gnx::gfx::kFocus);
                        gfx.text(fit_text(gfx, featured->title,
                                          featureRect.w - 36,
                                          gnx::gfx::FontSize::Small),
                                 featureRect.x + 18, featureRect.y + 55,
                                 gnx::gfx::FontSize::Small, gnx::gfx::kText);
                    }
                }

                const auto drawTab = [&](LibraryTab tab, const SDL_Rect& tabRect,
                                         const std::string& label) {
                    const bool active = libraryTab == tab && searchQuery.empty();
                    if (active) {
                        gfx.rounded_fill({tabRect.x - 4, tabRect.y - 4,
                                          tabRect.w + 8, tabRect.h + 8},
                                         15, gnx::gfx::Color{139, 92, 246, 20});
                    }
                    gfx.rounded_panel(tabRect, 13,
                                      active ? gnx::gfx::Color{74, 43, 154, 255}
                                             : gnx::gfx::kChip,
                                      active ? gnx::gfx::kFocus
                                             : gnx::gfx::kChipEdge,
                                      active ? 2 : 1);
                    gfx.text_centered(fit_text(gfx, label, tabRect.w - 20,
                                                gnx::gfx::FontSize::Small),
                                      tabRect.x + tabRect.w / 2,
                                      tabRect.y + 11,
                                      gnx::gfx::FontSize::Small,
                                      active ? gnx::gfx::kText
                                             : gnx::gfx::kTextDim);
                };
                const std::array<std::string, 4> topTabLabels{{
                    ui("MIS JUEGOS", "MY GAMES"),
                    ui("CATALOGO", "CATALOG"),
                    ui("FAVORITOS", "FAVORITES"),
                    ui("RECIENTES", "RECENT"),
                }};
                for (std::size_t index = 0; index < kLibraryTabOrder.size(); ++index) {
                    drawTab(kLibraryTabOrder[index], kLibraryTopTabRects[index],
                            topTabLabels[index]);
                }

                const SDL_Rect searchRect = kLibrarySearchRect;
                gfx.rounded_panel(searchRect, 13, gnx::gfx::kBar,
                                  searchQuery.empty() ? gnx::gfx::kChipEdge
                                                      : gnx::gfx::kFocus,
                                  searchQuery.empty() ? 1 : 2);
                gfx.circle(searchRect.x + 25, searchRect.y + 23, 8,
                           gnx::gfx::kTextDim);
                gfx.circle(searchRect.x + 25, searchRect.y + 23, 4,
                           gnx::gfx::kBar);
                gfx.line(searchRect.x + 31, searchRect.y + 29,
                         searchRect.x + 38, searchRect.y + 36,
                         gnx::gfx::kTextDim, 2);
                const std::string searchLabel = searchQuery.empty()
                    ? ui("(Y) Buscar juegos", "(Y) Search games")
                    : std::string(ui("Busqueda: ", "Search: ")) + searchQuery;
                gfx.text(fit_text(gfx, searchLabel, searchRect.w - 72),
                         searchRect.x + 52, searchRect.y + 11,
                         gnx::gfx::FontSize::Small,
                         searchQuery.empty() ? gnx::gfx::kTextDim
                                             : gnx::gfx::kText);

                gfx.text(fit_text(gfx,
                                  ui("ZL / ZR cambia seccion  |  L / R cambia pagina  |  R3 actualiza el catalogo",
                                     "ZL / ZR changes section  |  L / R changes page  |  R3 refreshes the catalog"),
                                  900, gnx::gfx::FontSize::Small),
                         300, 315, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);

                if (visibleGames.empty()) {
                    SDL_Rect emptyRect = {475, 445, 1180, 300};
                    gfx.rounded_panel(emptyRect, 24, gnx::gfx::kSurface,
                                      gnx::gfx::kChipEdge, 1);
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
                    gfx.text_centered(fit_text(gfx, emptyTitle,
                                                emptyRect.w - 120,
                                                gnx::gfx::FontSize::Title),
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 82,
                                      gnx::gfx::FontSize::Title,
                                      gnx::gfx::kText);
                    gfx.text_centered(fit_text(gfx, emptyHelp,
                                                emptyRect.w - 120,
                                                gnx::gfx::FontSize::Body),
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 180,
                                      gnx::gfx::FontSize::Body,
                                      gnx::gfx::kTextDim);
                    gfx.text_centered(ui("(Y) Buscar  |  (R3) Actualizar  |  (B) Volver",
                                         "(Y) Search  |  (R3) Refresh  |  (B) Back"),
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 245,
                                      gnx::gfx::FontSize::Small,
                                      gnx::gfx::kFocus);
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
                        if (selected) {
                            gfx.rounded_fill({card.x - 7, card.y - 7,
                                              card.w + 14, card.h + 14}, 23,
                                             gnx::gfx::Color{139, 92, 246, 24});
                        }
                        gfx.rounded_panel(card, 18,
                                          selected ? gnx::gfx::kSurfaceHi
                                                   : gnx::gfx::kSurface,
                                          selected ? gnx::gfx::kFocus
                                                   : gnx::gfx::kChipEdge,
                                          selected ? 3 : 1);

                        ZERODROID::GameItem* gamePtr =
                            gameFromVisible(visibleGames[index]);
                        if (!gamePtr) continue;
                        const ZERODROID::GameItem& game = *gamePtr;
                        // Boosteroid's poster endpoint is normally vertical.
                        // Give it a true poster slot instead of forcing it into
                        // a wide banner and creating black side bars.
                        const std::string& coverUrl = game.posterUrl.empty()
                            ? game.bannerUrl
                            : game.posterUrl;
                        const std::string coverKey =
                            "v101-poster-" + std::to_string(game.id);
                        const SDL_Rect coverRect = {card.x + 12, card.y + 12,
                                                    116, 178};
                        const SDL_Rect coverImageRect = {coverRect.x + 6,
                                                         coverRect.y + 6,
                                                         coverRect.w - 12,
                                                         coverRect.h - 12};
                        const int contentX = coverRect.x + coverRect.w + 16;
                        const int contentW = card.x + card.w - 14 - contentX;
                        gfx.rounded_panel(coverRect, 15,
                                          gnx::gfx::Color{13, 25, 45, 255},
                                          selected ? gnx::gfx::kFocus
                                                   : gnx::gfx::kChipEdge,
                                          selected ? 2 : 1);
                        SDL_Texture* cover = covers->get(coverKey, coverUrl);
                        if (cover) {
                            draw_cover_texture(gfx.renderer(), cover, coverImageRect);
                        } else {
                            const bool unavailable = coverUrl.empty() ||
                                                     covers->has_result(coverKey);
                            gfx.text_centered(fit_text(
                                                  gfx,
                                                  unavailable
                                                      ? ui("Sin caratula", "No cover")
                                                      : ui("Cargando imagen", "Loading image"),
                                                  coverImageRect.w - 12,
                                                  gnx::gfx::FontSize::Small),
                                coverRect.x + coverRect.w / 2,
                                coverRect.y + coverRect.h / 2 - 12,
                                gnx::gfx::FontSize::Small,
                                gnx::gfx::kFaint);
                        }

                        if (contains_game_id(serverSettings.favoriteGameIds,
                                             game.id)) {
                            SDL_Rect favoriteChip = {
                                coverRect.x + 8, coverRect.y + 8, 56, 30};
                            gfx.rounded_panel(favoriteChip, 10,
                                              gnx::gfx::Color{82, 45, 160, 245},
                                              gnx::gfx::kFocus, 1);
                            gfx.text_centered("FAV",
                                              favoriteChip.x + favoriteChip.w / 2,
                                              favoriteChip.y + 3,
                                              gnx::gfx::FontSize::Small,
                                              gnx::gfx::kText);
                        }

                        int titleY = card.y + 26;
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
                                contentX, card.y + 14, contentW, 32};
                            gfx.rounded_panel(stateChip, 10,
                                              game.isInstalled
                                                  ? gnx::gfx::Color{10, 78, 78, 240}
                                                  : gnx::gfx::Color{47, 37, 78, 240},
                                              game.isInstalled
                                                  ? gnx::gfx::kConnected
                                                  : gnx::gfx::kChipEdge, 1);
                            gfx.text_centered(fit_text(gfx, chipText,
                                                        stateChip.w - 18,
                                                        gnx::gfx::FontSize::Small),
                                              stateChip.x + stateChip.w / 2,
                                              stateChip.y + 4,
                                              gnx::gfx::FontSize::Small,
                                              gnx::gfx::kText);
                            titleY = card.y + 58;
                        }

                        const auto titleLines = wrap_text_lines(
                            gfx, game.title, contentW, 3,
                            gnx::gfx::FontSize::Small);
                        for (std::size_t line = 0;
                             line < titleLines.size(); ++line) {
                            gfx.text(titleLines[line], contentX,
                                     titleY + static_cast<int>(line) * 25,
                                     gnx::gfx::FontSize::Small,
                                     gnx::gfx::kText);
                        }
                        std::string platform = game.store.empty()
                            ? game.platform : game.store;
                        if (platform.empty()) platform = "Boosteroid Cloud";
                        gfx.text(fit_text(gfx, platform, contentW),
                                 contentX, card.y + 166,
                                 gnx::gfx::FontSize::Small,
                                 selected ? gnx::gfx::kFocus
                                          : gnx::gfx::kTextDim);
                        if (selected) {
                            const SDL_Rect actionRect = {
                                contentX, card.y + 216, contentW, 46};
                            gfx.rounded_panel(actionRect, 12,
                                              gnx::gfx::Color{53, 34, 106, 245},
                                              gnx::gfx::kFocus, 1);
                            gfx.text_centered(
                                game.isInstalled
                                    ? ui("(A) JUGAR", "(A) PLAY")
                                    : ui("(A) AGREGAR", "(A) ADD"),
                                actionRect.x + actionRect.w / 2,
                                actionRect.y + 10,
                                gnx::gfx::FontSize::Small,
                                gnx::gfx::kText);
                        }
                    }

                    const std::size_t page = selectedGame / kPageSize + 1;
                    const std::size_t pages =
                        (visibleGames.size() + kPageSize - 1) / kPageSize;
                    gfx.text(std::string(ui("Pagina ", "Page ")) +
                                 std::to_string(page) + " " + ui("de", "of") +
                                 " " + std::to_string(pages),
                             300, 946, gnx::gfx::FontSize::Small,
                             gnx::gfx::kTextDim);
                }

                if (!libraryNotice.empty()) {
                    gfx.text(fit_text(gfx, libraryNotice, 1140), 600, 946,
                             gnx::gfx::FontSize::Small, gnx::gfx::kWarn);
                }
            }
        }

        // A stable shortcut rail makes the controller contract visible on
        // every SDL-owned screen. It deliberately describes only controls the
        // application already supports.
        const SDL_Rect footerRect = {18, gnx::gfx::kHeight - 86, 1884, 68};
        gfx.rounded_panel(footerRect, 20, gnx::gfx::Color{7, 15, 29, 246},
                          gnx::gfx::Color{35, 53, 79, 190}, 1);
        if (!hasSession) {
            draw_footer_control(gfx, 690, "Y", ui("Renovar QR", "Refresh QR"), true);
            draw_footer_control(gfx, 1030, "+", ui("Salir", "Exit"));
        } else if (settingsOpen) {
            draw_footer_control(gfx, 430, "D", ui("Mover", "Move"));
            draw_footer_control(gfx, 730, "A", ui("Cambiar", "Change"), true);
            draw_footer_control(gfx, 1090, "B", ui("Volver", "Back"));
            draw_footer_control(gfx, 1390, "Y", ui("Actualizar region", "Refresh region"));
        } else if (launchPanel) {
            draw_footer_control(gfx, 700, "B", ui("Cancelar", "Cancel"));
            draw_footer_control(gfx, 1040, "+", ui("Salir", "Exit"));
        } else {
            draw_footer_control(gfx, 330, "Y", ui("Buscar", "Search"));
            draw_footer_control(gfx, 585, "X", ui("Favorito", "Favorite"));
            draw_footer_control(gfx, 865, "A", ui("Jugar / Agregar", "Play / Add"), true);
            draw_footer_control(gfx, 1190, "-", ui("Configuracion", "Settings"));
            draw_footer_control(gfx, 1510, "+", ui("Salir", "Exit"));
        }

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
