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

// Palette — "OLED premium" (docs-design/green-nx-redesign.dc.html, card 1a)
constexpr Color kBg{4, 9, 13};              // v0.4 fallback background
constexpr Color kBar{6, 12, 18, 236};        // header/footer glass
constexpr Color kSurface{10, 20, 27, 224};   // translucent cards and rows
constexpr Color kSurfaceHi{18, 38, 43, 238}; // focused glass surface
constexpr Color kAccent{16, 124, 16};         // Xbox green
constexpr Color kFocus{57, 224, 103};         // emerald focus / glow
constexpr Color kText{240, 243, 248};     // #F0F3F8 primary text
constexpr Color kTextDim{152, 162, 179};  // #98A2B3 secondary text, hints
constexpr Color kWarn{240, 180, 60};      // #F0B43C favorites, notices
constexpr Color kError{232, 104, 104};    // #E86868 errors
constexpr Color kChip{28, 34, 48};        // #1C2230 button chips, separators
constexpr Color kChipEdge{42, 50, 66};    // #2A3242 chip border
constexpr Color kFaint{91, 100, 116};     // #5b6474 tertiary (counters, idle tabs)

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
