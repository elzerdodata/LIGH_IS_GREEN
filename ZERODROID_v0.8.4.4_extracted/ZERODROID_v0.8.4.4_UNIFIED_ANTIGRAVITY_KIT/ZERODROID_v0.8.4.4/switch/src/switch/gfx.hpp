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

// ZERODROID palette: graphite, cool gray and electric lilac.
constexpr Color kBg{13, 11, 22};
constexpr Color kBar{24, 21, 34, 238};
constexpr Color kSurface{38, 34, 50, 226};
constexpr Color kSurfaceHi{58, 49, 78, 240};
constexpr Color kAccent{167, 139, 250};
constexpr Color kFocus{196, 181, 253};
constexpr Color kText{243, 244, 246};       // #F3F4F6 primary text
constexpr Color kTextDim{156, 163, 175};    // #9CA3AF secondary text
constexpr Color kWarn{251, 191, 36};        // #FBBF24 notice yellow
constexpr Color kError{239, 68, 68};        // #EF4444 error red
constexpr Color kChip{39, 36, 48};
constexpr Color kChipEdge{91, 83, 112};
constexpr Color kFaint{107, 114, 128};      // #6B7280 tertiary text

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
