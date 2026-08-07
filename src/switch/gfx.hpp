#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <string>
#include <unordered_map>

namespace gnx::gfx {

// Design-space resolution; SDL logical size scales it to 720p/1080p output.
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

struct Color {
    Uint8 r, g, b, a = 255;
};

// ZERODROID 1.0: a calm navy base, cool surfaces and a deliberately limited
// electric-lilac focus state. Teal is semantic: it only describes a real
// healthy/connected state, never a decorative one.
constexpr Color kBg{5, 10, 20};
constexpr Color kBar{7, 15, 29, 246};
constexpr Color kSurface{14, 25, 44, 238};
constexpr Color kSurfaceHi{27, 35, 65, 246};
constexpr Color kAccent{139, 92, 246};
constexpr Color kFocus{196, 181, 253};
constexpr Color kConnected{45, 212, 191};
constexpr Color kText{241, 245, 249};       // cool white
constexpr Color kTextDim{148, 163, 184};    // slate secondary text
constexpr Color kWarn{251, 191, 36};        // warning amber
constexpr Color kError{248, 113, 113};      // error coral
constexpr Color kChip{17, 29, 50};
constexpr Color kChipEdge{49, 65, 91};
constexpr Color kFaint{100, 116, 139};      // tertiary text

enum class FontSize { Small = 0, Body, Title, Huge, Note };

class Gfx {
public:
    bool init();
    void shutdown();

    SDL_Renderer* renderer() { return renderer_; }

    void suspend();
    bool resume();

    void begin_frame();
    void end_frame();

    void fill(const SDL_Rect& rect, Color color);
    void frame(const SDL_Rect& rect, Color color, int thickness = 3);
    void rounded_fill(const SDL_Rect& rect, int radius, Color color);
    void rounded_panel(const SDL_Rect& rect, int radius, Color fillColor,
                       Color borderColor, int borderThickness = 1);
    void line(int x1, int y1, int x2, int y2, Color color,
              int thickness = 1);
    void circle(int cx, int cy, int radius, Color color);
    int text(const std::string& utf8, int x, int y, FontSize size, Color color);
    int text_centered(const std::string& utf8, int cx, int y, FontSize size,
                      Color color);
    int text_width(const std::string& utf8, FontSize size);

    SDL_Texture* texture_from_memory(const void* data, size_t size);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* font_small_ = nullptr;
    TTF_Font* font_body_ = nullptr;
    TTF_Font* font_title_ = nullptr;
    TTF_Font* font_huge_ = nullptr;
    TTF_Font* font_note_ = nullptr;
    std::unordered_map<std::string, SDL_Texture*> text_cache_;
};

} // namespace gnx::gfx
