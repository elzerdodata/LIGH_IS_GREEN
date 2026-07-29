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

// The glyphs live in the very top safe strip so they no longer collide with
// the Switch status clock.  Their 72x72 hit areas remain comfortable to tap,
// while the rasterized artwork inside them is deliberately much smaller.
inline constexpr QuickRect kQuickTextureRect{1160, 8, 672, 812};
inline constexpr QuickRect kQuickToggleRect{1760, 8, 72, 72};
inline constexpr QuickRect kGuideButtonRect{1840, 8, 72, 72};
inline constexpr QuickRect kQuickPanelRect{1160, 104, 508, 716};

inline constexpr int kQuickRowCount = 7;
inline constexpr int kQuickRowFirstY = 172;
inline constexpr int kQuickRowHeight = 58;
inline constexpr int kQuickRowPitch = 64;

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

inline constexpr QuickRect kQuickResetRect{1280, 744, 268, 54};

enum QuickRow {
    QuickPerformance = 0,
    QuickPacing = 1,
    QuickBrightness = 2,
    QuickContrast = 3,
    QuickSaturation = 4,
    QuickGamma = 5,
    QuickSharpness = 6,
};

struct QuickMenuState {
    bool open = false;
    bool performance = false;
    int pacing = 0;       // 0=Steady, 1=Smooth, 2=Motion (experimental)
    int brightness = 0;   // -20..+20
    int contrast = 100;   // 70..130 percent
    int saturation = 100; // 0..150 percent
    int gamma = 100;      // 50..200 percent; 100 = neutral (1.00)
    int sharpness = 0;    // 0=Off, 1=Low, 2=Medium, 3=High
};

inline QuickMenuState normalized_quick_menu(QuickMenuState state) {
    state.pacing = std::clamp(state.pacing, 0, 2);
    state.brightness = std::clamp(state.brightness, -20, 20);
    state.contrast = std::clamp(state.contrast, 70, 130);
    state.saturation = std::clamp(state.saturation, 0, 150);
    state.gamma = std::clamp(state.gamma, 50, 200);
    state.sharpness = std::clamp(state.sharpness, 0, 3);
    return state;
}

}  // namespace gnx::stream
