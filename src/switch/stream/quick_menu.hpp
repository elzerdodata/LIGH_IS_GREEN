#pragma once

#include <algorithm>

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

inline constexpr QuickRect kQuickTextureRect{1160, 48, 512, 640};
inline constexpr QuickRect kQuickToggleRect{1596, 48, 72, 72};
inline constexpr QuickRect kQuickPanelRect{1160, 144, 508, 520};

inline constexpr int kQuickRowCount = 5;
inline constexpr int kQuickRowFirstY = 220;
inline constexpr int kQuickRowHeight = 58;
inline constexpr int kQuickRowPitch = 68;

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

inline constexpr QuickRect kQuickResetRect{1280, 580, 268, 54};

enum QuickRow {
    QuickPerformance = 0,
    QuickBrightness = 1,
    QuickContrast = 2,
    QuickSaturation = 3,
    QuickSharpness = 4,
};

struct QuickMenuState {
    bool open = false;
    bool performance = false;
    int brightness = 0;   // -20..+20
    int contrast = 100;   // 70..130 percent
    int saturation = 100; // 0..150 percent
    int sharpness = 0;    // 0=Off, 1=Low, 2=Medium, 3=High
};

inline QuickMenuState normalized_quick_menu(QuickMenuState state) {
    state.brightness = std::clamp(state.brightness, -20, 20);
    state.contrast = std::clamp(state.contrast, 70, 130);
    state.saturation = std::clamp(state.saturation, 0, 150);
    state.sharpness = std::clamp(state.sharpness, 0, 3);
    return state;
}

}  // namespace gnx::stream
