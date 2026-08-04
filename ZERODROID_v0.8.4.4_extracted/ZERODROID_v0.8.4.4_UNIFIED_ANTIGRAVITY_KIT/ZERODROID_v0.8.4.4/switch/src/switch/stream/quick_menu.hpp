#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace gnx::stream {

// Shared 1920x1080 design-space geometry for the in-stream touch menu. The
// renderer uses the same rectangles for its deko3d quad and CPU-rasterized
// texture, so the visual controls and libnx touch targets stay aligned.
struct QuickRect {
    int x;
    int y;
    int w;
    int h;
};

// v0.8.4.4 keeps the picture settings panel and replaces the compact
// session-actions popup with a full session-control overlay opened from the
// always-visible lilac Xbox/X touch icon.
inline constexpr QuickRect kQuickTextureRect{1160, 8, 672, 928};
inline constexpr QuickRect kQuickToggleRect{1760, 8, 72, 72};
inline constexpr QuickRect kGuideButtonRect{1840, 8, 72, 72};
inline constexpr QuickRect kQuickPanelRect{1160, 104, 508, 820};

inline constexpr int kQuickRowCount = 10;
inline constexpr int kQuickRowFirstY = 146;
inline constexpr int kQuickRowHeight = 50;
inline constexpr int kQuickRowPitch = 54;

inline constexpr QuickRect quick_row_rect(int row) {
    return {1180, kQuickRowFirstY + row * kQuickRowPitch, 468,
            kQuickRowHeight};
}

inline constexpr QuickRect quick_minus_rect(int row) {
    QuickRect r = quick_row_rect(row);
    return {1380, r.y, 64, r.h};
}

inline constexpr QuickRect quick_plus_rect(int row) {
    QuickRect r = quick_row_rect(row);
    return {1584, r.y, 64, r.h};
}

inline constexpr QuickRect kQuickNoticeRect{1180, 700, 468, 76};
inline constexpr QuickRect kQuickResetRect{1280, 798, 268, 54};
inline constexpr QuickRect kQuickMouseHelpRect{1180, 862, 468, 46};

// Touching the top-right lilac Xbox/X icon opens a control-centre inspired by
// the handheld streaming overlay supplied as a visual reference. The geometry
// remains inside the existing 672x928 quick texture, avoiding an additional GPU
// texture and keeping the change safe for handheld memory limits.
inline constexpr QuickRect kSessionPanelRect{1172, 92, 648, 838};
inline constexpr QuickRect kSessionStatusRect{1192, 154, 296, 590};
inline constexpr QuickRect kSessionActionsRect{1502, 154, 298, 590};
inline constexpr QuickRect kSessionGuideRect{1520, 204, 262, 58};
inline constexpr QuickRect kSessionAltTabRect{1520, 276, 262, 58};
inline constexpr QuickRect kSessionKeyboardRect{1520, 348, 262, 58};
inline constexpr QuickRect kSessionMouseRect{1520, 420, 262, 58};
inline constexpr QuickRect kSessionReconnectRect{1520, 492, 262, 58};
inline constexpr QuickRect kSessionSettingsRect{1520, 564, 262, 58};
inline constexpr QuickRect kSessionCloseRect{1520, 636, 262, 58};
inline constexpr QuickRect kSessionHelpRect{1192, 762, 590, 138};

// Reconnect is destructive to the local transport and therefore requires an
// explicit second tap. The remote Boosteroid machine is intentionally not
// terminated by this path.
inline constexpr QuickRect kReconnectConfirmPanelRect{1260, 280, 500, 300};
inline constexpr QuickRect kReconnectCancelRect{1290, 500, 190, 54};
inline constexpr QuickRect kReconnectConfirmRect{1540, 500, 190, 54};

enum QuickRow {
    QuickPerformance = 0,
    QuickController = 1,
    QuickMouseSpeed = 2,
    QuickResolution = 3,
    QuickPreset = 4,
    QuickBrightness = 5,
    QuickContrast = 6,
    QuickSaturation = 7,
    QuickGamma = 8,
    QuickSharpness = 9,
};

enum StreamResolutionMode {
    ResolutionAuto = 0,
    Resolution720p = 1,
    Resolution1080p = 2,
    Resolution1440p = 3,
};

enum PicturePreset {
    PresetNatural = 0,
    PresetSharp = 1,
    PresetVivid = 2,
    PresetCinema = 3,
    PresetSoft = 4,
    PresetCustom = 5,
};

enum MouseSpeed {
    MousePrecise = 0,
    MouseNormal = 1,
    MouseFast = 2,
};

struct QuickMenuState {
    bool open = false;
    bool sessionActionsOpen = false;
    bool reconnectConfirmOpen = false;
    bool mouseModeEnabled = true;
    bool performance = false;
    bool xboxFaceLayout = false;
    int mouseSpeed = MouseNormal;
    int resolutionMode = ResolutionAuto;
    int picturePreset = PresetNatural;
    int brightness = 0;   // -20..+20
    int contrast = 100;   // 70..130 percent
    int saturation = 100; // 0..150 percent
    int gamma = 100;      // 50..200 percent; 100 = neutral (1.00)
    int sharpness = 0;    // 0=Off, 1=Low, 2=Medium, 3=High

    // Live session diagnostics copied from Engine by the UI loop. These fields
    // are display-only and never participate in stream negotiation.
    std::string sessionStatus{"CONNECTING"};
    std::string currentGame;
    std::string gatewayLabel;
    int streamWidth = 1280;
    int streamHeight = 720;
    int outputWidth = 1280;
    int outputHeight = 720;
    uint64_t sessionSeconds = 0;
    uint32_t droppedGroups = 0;
    uint32_t recoveredGroups = 0;
    uint32_t recoveryRequests = 0;
    uint64_t mouseMoves = 0;
    uint64_t mouseClicks = 0;
    uint64_t keyboardEvents = 0;
};

inline void apply_picture_preset(QuickMenuState& state, int preset) {
    state.picturePreset = std::clamp(preset, static_cast<int>(PresetNatural),
                                     static_cast<int>(PresetCustom));
    switch (state.picturePreset) {
        case PresetSharp:
            state.brightness = 0;
            state.contrast = 105;
            state.saturation = 105;
            state.gamma = 100;
            state.sharpness = 2;
            break;
        case PresetVivid:
            state.brightness = 0;
            state.contrast = 110;
            state.saturation = 120;
            state.gamma = 100;
            state.sharpness = 2;
            break;
        case PresetCinema:
            state.brightness = -5;
            state.contrast = 110;
            state.saturation = 105;
            state.gamma = 95;
            state.sharpness = 1;
            break;
        case PresetSoft:
            state.brightness = 0;
            state.contrast = 95;
            state.saturation = 100;
            state.gamma = 105;
            state.sharpness = 0;
            break;
        case PresetCustom:
            // Keep the user's current manual values.
            break;
        case PresetNatural:
        default:
            state.picturePreset = PresetNatural;
            state.brightness = 0;
            state.contrast = 100;
            state.saturation = 100;
            state.gamma = 100;
            state.sharpness = 0;
            break;
    }
}

inline QuickMenuState normalized_quick_menu(QuickMenuState state) {
    state.mouseSpeed = std::clamp(
        state.mouseSpeed, static_cast<int>(MousePrecise),
        static_cast<int>(MouseFast));
    state.resolutionMode = std::clamp(
        state.resolutionMode, static_cast<int>(ResolutionAuto),
        static_cast<int>(Resolution1440p));
    state.picturePreset = std::clamp(
        state.picturePreset, static_cast<int>(PresetNatural),
        static_cast<int>(PresetCustom));
    state.brightness = std::clamp(state.brightness, -20, 20);
    state.contrast = std::clamp(state.contrast, 70, 130);
    state.saturation = std::clamp(state.saturation, 0, 150);
    state.gamma = std::clamp(state.gamma, 50, 200);
    state.sharpness = std::clamp(state.sharpness, 0, 3);
    return state;
}

}  // namespace gnx::stream
