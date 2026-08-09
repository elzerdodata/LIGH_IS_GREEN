#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <unordered_map>

namespace gnx::gfx {

// Design-space resolution; SDL logical size scales it to 720p/1080p output.
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

struct Color {
    Uint8 r, g, b, a = 255;
};

// Light is Green v1.0: deep aurora glass with a restrained emerald focus.
// All glow is drawn with inexpensive SDL frames; the background remains one
// static texture so browsing the 6-by-3 catalogue stays memory-safe.
constexpr Color kBg{2, 8, 9};                // fallback background
constexpr Color kBar{3, 14, 15, 238};        // header/footer glass
constexpr Color kSurface{7, 24, 25, 226};    // translucent cards and rows
constexpr Color kSurfaceHi{12, 40, 40, 240}; // focused glass surface
constexpr Color kAccent{0, 166, 101};        // primary emerald action
constexpr Color kFocus{45, 244, 165};        // focus / glow
constexpr Color kText{237, 255, 247};        // primary text
constexpr Color kTextDim{164, 205, 191};     // secondary text, hints
constexpr Color kWarn{246, 196, 78};         // favorites, notices
constexpr Color kError{239, 109, 121};       // errors
constexpr Color kChip{13, 40, 40};           // button chips, separators
constexpr Color kChipEdge{43, 104, 94};      // chip border
constexpr Color kFaint{101, 147, 135};       // counters, idle tabs

// XS 24 hints/captions · Note(S) 30 metadata/status · Body(M) 38 tabs/rows ·
// Title(L) 54 screen+game titles · Huge(XL) 100 sign-in code, logo.
enum class FontSize { Small = 0, Body, Title, Huge, Note };

class Gfx {
public:
    bool init();
    void shutdown();

    SDL_Renderer* renderer() { return renderer_; }

    // Release the SDL window/renderer so deko3d can take over the single Switch
    // display during streaming; resume() rebuilds them afterwards. Cached
    // textures are destroyed with the renderer and regenerate on demand.
    void suspend();
    bool resume();

    void begin_frame();
    void end_frame();

    void fill(const SDL_Rect& rect, Color color);
    void frame(const SDL_Rect& rect, Color color, int thickness = 3);
    // Text draws return the rendered width.
    int text(const std::string& utf8, int x, int y, FontSize size, Color color);
    int text_centered(const std::string& utf8, int cx, int y, FontSize size,
                      Color color);
    int text_width(const std::string& utf8, FontSize size);

    SDL_Texture* texture_from_memory(const void* data, size_t size);
    void draw_texture(SDL_Texture* texture, const SDL_Rect& destination);
    void draw_texture_cover(SDL_Texture* texture, const SDL_Rect& destination);
    // Aspect-contain: show the complete texture, centered, without cropping
    // or stretching. Empty space keeps the card surface as letterbox.
    void draw_texture_contain(SDL_Texture* texture,
                              const SDL_Rect& destination);
    void draw_brand_icon(const SDL_Rect& destination);

    // Simple pulsing loading dot row.
    void spinner(int cx, int y, Uint32 ticks);

private:
    TTF_Font* font(FontSize size);
    SDL_Texture* render_text(const std::string& utf8, FontSize size,
                             Color color, int* width, int* height);
    bool create_window_renderer();  // window + renderer only (no subsystem init)
    void destroy_renderer_textures();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* background_ = nullptr;
    SDL_Texture* brand_icon_ = nullptr;
    TTF_Font* fonts_[5] = {};
    void* font_data_ = nullptr;  // shared system font blob (not owned)

    struct CachedText {
        SDL_Texture* texture;
        int width, height;
        Uint32 last_used;
    };
    std::unordered_map<std::string, CachedText> text_cache_;
    void trim_text_cache();
};

}  // namespace gnx::gfx
