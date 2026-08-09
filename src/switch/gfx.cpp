#include "gfx.hpp"

#include <SDL2/SDL_image.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gnx::gfx {

namespace {
constexpr int kFontPx[5] = {24, 38, 54, 100, 30};
}

bool Gfx::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                 SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) return false;
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    // SDL_Init already brought up the video subsystem; just make the window.
    if (!create_window_renderer()) return false;

#ifdef __SWITCH__
    PlFontData shared_font;
    if (R_SUCCEEDED(plGetSharedFontByType(&shared_font,
                                          PlSharedFontType_Standard))) {
        font_data_ = shared_font.address;
        for (int i = 0; i < 5; ++i) {
            SDL_RWops* rw =
                SDL_RWFromConstMem(shared_font.address, shared_font.size);
            fonts_[i] = TTF_OpenFontRW(rw, 1, kFontPx[i]);
        }
    }
#else
    const char* path = "/System/Library/Fonts/Helvetica.ttc";
    for (int i = 0; i < 5; ++i) fonts_[i] = TTF_OpenFont(path, kFontPx[i]);
#endif
    return fonts_[0] != nullptr;
}

void Gfx::shutdown() {
    destroy_renderer_textures();
    for (TTF_Font*& handle : fonts_)
        if (handle) TTF_CloseFont(handle), handle = nullptr;
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

bool Gfx::create_window_renderer() {
    window_ = SDL_CreateWindow("Light is Green", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                               SDL_WINDOW_SHOWN);
    if (!window_) return false;
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;
    SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
#ifdef __SWITCH__
    background_ = IMG_LoadTexture(renderer_, "romfs:/ui/light_is_green_bg.png");
    brand_icon_ = IMG_LoadTexture(renderer_, "romfs:/ui/app_icon.png");
    if (!background_) background_ = IMG_LoadTexture(renderer_, "romfs:/ui/v1/lig_aurora_bg_1280x720.jpg");
    if (!brand_icon_) brand_icon_ = IMG_LoadTexture(renderer_, "romfs:/ui/v1/lig_mark_128.png");
#else
    background_ = IMG_LoadTexture(renderer_, "romfs/ui/light_is_green_bg.png");
    brand_icon_ = IMG_LoadTexture(renderer_, "romfs/ui/app_icon.png");
    if (!background_) background_ = IMG_LoadTexture(renderer_, "romfs/ui/v1/lig_aurora_bg_1280x720.jpg");
    if (!brand_icon_) brand_icon_ = IMG_LoadTexture(renderer_, "romfs/ui/v1/lig_mark_128.png");
#endif
    if (background_) SDL_SetTextureBlendMode(background_, SDL_BLENDMODE_BLEND);
    if (brand_icon_) SDL_SetTextureBlendMode(brand_icon_, SDL_BLENDMODE_BLEND);
    return true;
}

bool Gfx::resume() {
    // Re-init the video subsystem we fully quit in suspend() -- only that
    // releases the default nwindow so deko3d could own it, and only bringing it
    // back reconnects SDL as the window's producer.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
    return create_window_renderer();
}

void Gfx::suspend() {
    destroy_renderer_textures();
    if (renderer_) SDL_DestroyRenderer(renderer_), renderer_ = nullptr;
    if (window_) SDL_DestroyWindow(window_), window_ = nullptr;
    // Destroying the window is not enough: SDL's mesa/EGL backend keeps the
    // default nwindow bound until the whole video subsystem is torn down. Fully
    // quit it so deko3d can become the window's buffer producer. SDL_Init took
    // one ref at startup, so this single Quit actually shuts the subsystem down.
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

TTF_Font* Gfx::font(FontSize size) {
    return fonts_[static_cast<int>(size)];
}

void Gfx::begin_frame() {
    SDL_SetRenderDrawColor(renderer_, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(renderer_);
    if (background_) {
        SDL_Rect full = {0, 0, kWidth, kHeight};
        SDL_RenderCopy(renderer_, background_, nullptr, &full);
        // A dark scrim keeps cover art and small text readable in handheld mode.
        fill(full, {2, 7, 10, 148});
    }
}

void Gfx::end_frame() {
    SDL_RenderPresent(renderer_);
    trim_text_cache();
}

void Gfx::fill(const SDL_Rect& rect, Color color) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer_, &rect);
}

void Gfx::frame(const SDL_Rect& rect, Color color, int thickness) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect outline = {rect.x - i, rect.y - i, rect.w + 2 * i,
                            rect.h + 2 * i};
        SDL_RenderDrawRect(renderer_, &outline);
    }
}

SDL_Texture* Gfx::render_text(const std::string& utf8, FontSize size,
                              Color color, int* width, int* height) {
    std::string key = std::to_string(static_cast<int>(size)) + "|" +
                      std::to_string(color.r) + "," +
                      std::to_string(color.g) + "," +
                      std::to_string(color.b) + "|" + utf8;
    auto found = text_cache_.find(key);
    if (found != text_cache_.end()) {
        found->second.last_used = SDL_GetTicks();
        *width = found->second.width;
        *height = found->second.height;
        return found->second.texture;
    }

    SDL_Color sdl_color = {color.r, color.g, color.b, color.a};
    SDL_Surface* surface =
        TTF_RenderUTF8_Blended(font(size), utf8.c_str(), sdl_color);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    *width = surface->w;
    *height = surface->h;
    SDL_FreeSurface(surface);
    if (texture)
        text_cache_[key] = {texture, *width, *height, SDL_GetTicks()};
    return texture;
}

int Gfx::text(const std::string& utf8, int x, int y, FontSize size,
              Color color) {
    if (utf8.empty()) return 0;
    int width = 0, height = 0;
    SDL_Texture* texture = render_text(utf8, size, color, &width, &height);
    if (!texture) return 0;
    SDL_Rect destination = {x, y, width, height};
    SDL_RenderCopy(renderer_, texture, nullptr, &destination);
    return width;
}

int Gfx::text_centered(const std::string& utf8, int cx, int y, FontSize size,
                       Color color) {
    return text(utf8, cx - text_width(utf8, size) / 2, y, size, color);
}

int Gfx::text_width(const std::string& utf8, FontSize size) {
    int width = 0, height = 0;
    TTF_SizeUTF8(font(size), utf8.c_str(), &width, &height);
    return width;
}

SDL_Texture* Gfx::texture_from_memory(const void* data, size_t size) {
    SDL_RWops* rw = SDL_RWFromConstMem(data, size);
    SDL_Surface* surface = IMG_Load_RW(rw, 1);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);
    return texture;
}

void Gfx::draw_texture(SDL_Texture* texture, const SDL_Rect& destination) {
    SDL_RenderCopy(renderer_, texture, nullptr, &destination);
}

void Gfx::draw_texture_cover(SDL_Texture* texture,
                             const SDL_Rect& destination) {
    if (!texture || destination.w <= 0 || destination.h <= 0) return;
    int width = 0, height = 0;
    if (SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) != 0 ||
        width <= 0 || height <= 0) {
        SDL_RenderCopy(renderer_, texture, nullptr, &destination);
        return;
    }
    const float source_aspect = static_cast<float>(width) / height;
    const float target_aspect =
        static_cast<float>(destination.w) / destination.h;
    SDL_Rect source = {0, 0, width, height};
    if (source_aspect > target_aspect) {
        source.w = static_cast<int>(height * target_aspect);
        source.x = (width - source.w) / 2;
    } else {
        source.h = static_cast<int>(width / target_aspect);
        source.y = (height - source.h) / 2;
    }
    SDL_RenderCopy(renderer_, texture, &source, &destination);
}

void Gfx::draw_texture_contain(SDL_Texture* texture,
                               const SDL_Rect& destination) {
    if (!texture || destination.w <= 0 || destination.h <= 0) return;

    int source_width = 0;
    int source_height = 0;
    if (SDL_QueryTexture(texture, nullptr, nullptr, &source_width,
                         &source_height) != 0 ||
        source_width <= 0 || source_height <= 0) {
        return;
    }

    const float scale_x =
        static_cast<float>(destination.w) / source_width;
    const float scale_y =
        static_cast<float>(destination.h) / source_height;
    const float scale = std::min(scale_x, scale_y);

    const int render_width = std::max(
        1, static_cast<int>(std::lround(source_width * scale)));
    const int render_height = std::max(
        1, static_cast<int>(std::lround(source_height * scale)));
    SDL_Rect centered = {
        destination.x + (destination.w - render_width) / 2,
        destination.y + (destination.h - render_height) / 2,
        render_width,
        render_height,
    };
    SDL_RenderCopy(renderer_, texture, nullptr, &centered);
}

void Gfx::draw_brand_icon(const SDL_Rect& destination) {
    if (brand_icon_)
        SDL_RenderCopy(renderer_, brand_icon_, nullptr, &destination);
}

void Gfx::spinner(int cx, int y, Uint32 ticks) {
    // 3 pulsing 14x14 squares, 30px apart (redesign card 1c).
    for (int i = 0; i < 3; ++i) {
        float phase = std::sin((ticks / 200.0f) - i * 0.9f);
        Uint8 alpha = static_cast<Uint8>(64 + 191 * (phase > 0 ? phase : 0));
        SDL_Rect dot = {cx - 37 + i * 30, y, 14, 14};
        fill(dot, {kText.r, kText.g, kText.b, alpha});
    }
}

void Gfx::trim_text_cache() {
    if (text_cache_.size() < 512) return;
    Uint32 now = SDL_GetTicks();
    for (auto it = text_cache_.begin(); it != text_cache_.end();) {
        if (now - it->second.last_used > 5000) {
            SDL_DestroyTexture(it->second.texture);
            it = text_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void Gfx::destroy_renderer_textures() {
    for (auto& [key, cached] : text_cache_)
        SDL_DestroyTexture(cached.texture);
    text_cache_.clear();
    if (background_) SDL_DestroyTexture(background_), background_ = nullptr;
    if (brand_icon_) SDL_DestroyTexture(brand_icon_), brand_icon_ = nullptr;
}

}  // namespace gnx::gfx
