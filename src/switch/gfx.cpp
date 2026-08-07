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
// Sizes are expressed in the 1920x1080 design space. SDL scales the finished
// interface down to the handheld's 1280x720 output, so these deliberately stay
// compact enough for game cards while remaining readable on a television.
constexpr int kFontPx[5] = {22, 30, 44, 84, 25};
}

bool Gfx::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                 SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) return false;
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    window_ = SDL_CreateWindow("ZERODROID", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                               SDL_WINDOW_SHOWN);
    if (!window_) return false;
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;
    SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

#ifdef __SWITCH__
    PlFontData shared_font;
    if (R_SUCCEEDED(plGetSharedFontByType(&shared_font,
                                          PlSharedFontType_Standard))) {
        const auto open_font = [&](int px) -> TTF_Font* {
            SDL_RWops* rw = SDL_RWFromConstMem(shared_font.address,
                                               shared_font.size);
            return rw ? TTF_OpenFontRW(rw, 1, px) : nullptr;
        };
        font_small_ = open_font(kFontPx[0]);
        font_body_ = open_font(kFontPx[1]);
        font_title_ = open_font(kFontPx[2]);
        font_huge_ = open_font(kFontPx[3]);
        font_note_ = open_font(kFontPx[4]);
    }
#endif
    return true;
}

void Gfx::shutdown() {
    for (auto& [key, tex] : text_cache_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    text_cache_.clear();
    if (font_small_) TTF_CloseFont(font_small_), font_small_ = nullptr;
    if (font_body_) TTF_CloseFont(font_body_), font_body_ = nullptr;
    if (font_title_) TTF_CloseFont(font_title_), font_title_ = nullptr;
    if (font_huge_) TTF_CloseFont(font_huge_), font_huge_ = nullptr;
    if (font_note_) TTF_CloseFont(font_note_), font_note_ = nullptr;
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

bool Gfx::resume() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
    window_ = SDL_CreateWindow("ZERODROID", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                               SDL_WINDOW_SHOWN);
    if (!window_) return false;
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;
    SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    return true;
}

void Gfx::suspend() {
    for (auto& [key, tex] : text_cache_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    text_cache_.clear();
    if (renderer_) SDL_DestroyRenderer(renderer_), renderer_ = nullptr;
    if (window_) SDL_DestroyWindow(window_), window_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Gfx::begin_frame() {
    SDL_SetRenderDrawColor(renderer_, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(renderer_);
}

void Gfx::end_frame() {
    SDL_RenderPresent(renderer_);
}

void Gfx::fill(const SDL_Rect& rect, Color color) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer_, &rect);
}

void Gfx::frame(const SDL_Rect& rect, Color color, int thickness) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect r = {rect.x - i, rect.y - i, rect.w + 2 * i, rect.h + 2 * i};
        SDL_RenderDrawRect(renderer_, &r);
    }
}

void Gfx::rounded_fill(const SDL_Rect& rect, int radius, Color color) {
    if (!renderer_ || rect.w <= 0 || rect.h <= 0) return;
    const int r = std::clamp(radius, 0, std::min(rect.w, rect.h) / 2);
    if (r == 0) {
        fill(rect, color);
        return;
    }

    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_Rect horizontal = {rect.x + r, rect.y, rect.w - 2 * r, rect.h};
    SDL_Rect vertical = {rect.x, rect.y + r, rect.w, rect.h - 2 * r};
    SDL_RenderFillRect(renderer_, &horizontal);
    SDL_RenderFillRect(renderer_, &vertical);

    // Four corners are drawn as scanlines. This is intentionally lightweight:
    // the library draws at most a few dozen surfaces in a 1920x1080 space and
    // needs smooth cards even on the Switch's handheld GPU.
    const float rr = static_cast<float>(r) * static_cast<float>(r);
    for (int row = 0; row < r; ++row) {
        const float dy = static_cast<float>(r - row) - 0.5f;
        const int inset = std::clamp(
            r - static_cast<int>(std::sqrt(std::max(0.0f, rr - dy * dy))),
            0, r);
        SDL_RenderDrawLine(renderer_, rect.x + inset, rect.y + row,
                           rect.x + rect.w - inset - 1, rect.y + row);
        SDL_RenderDrawLine(renderer_, rect.x + inset,
                           rect.y + rect.h - row - 1,
                           rect.x + rect.w - inset - 1,
                           rect.y + rect.h - row - 1);
    }
}

void Gfx::rounded_panel(const SDL_Rect& rect, int radius, Color fillColor,
                        Color borderColor, int borderThickness) {
    if (rect.w <= 0 || rect.h <= 0) return;
    const int thickness = std::max(0, borderThickness);
    if (thickness == 0) {
        rounded_fill(rect, radius, fillColor);
        return;
    }
    rounded_fill(rect, radius, borderColor);
    SDL_Rect inner = {rect.x + thickness, rect.y + thickness,
                      rect.w - 2 * thickness, rect.h - 2 * thickness};
    if (inner.w > 0 && inner.h > 0) {
        rounded_fill(inner, std::max(0, radius - thickness), fillColor);
    }
}

void Gfx::line(int x1, int y1, int x2, int y2, Color color, int thickness) {
    if (!renderer_) return;
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    const int count = std::max(1, thickness);
    for (int offset = 0; offset < count; ++offset) {
        SDL_RenderDrawLine(renderer_, x1, y1 + offset, x2, y2 + offset);
    }
}

void Gfx::circle(int cx, int cy, int radius, Color color) {
    if (!renderer_ || radius <= 0) return;
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    const int squared = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        const int span = static_cast<int>(std::sqrt(
            std::max(0, squared - y * y)));
        SDL_RenderDrawLine(renderer_, cx - span, cy + y, cx + span, cy + y);
    }
}

int Gfx::text(const std::string& utf8, int x, int y, FontSize size, Color color) {
    if (utf8.empty() || !renderer_) return 0;
    
    std::string cache_key = utf8 + "_" + std::to_string(static_cast<int>(size)) + "_" +
                            std::to_string(color.r) + std::to_string(color.g) +
                            std::to_string(color.b) + std::to_string(color.a);
                            
    SDL_Texture* tex = nullptr;
    auto it = text_cache_.find(cache_key);
    if (it != text_cache_.end()) {
        tex = it->second;
    } else {
        TTF_Font* font = font_body_;
        if (size == FontSize::Small && font_small_) font = font_small_;
        else if (size == FontSize::Title && font_title_) font = font_title_;
        else if (size == FontSize::Huge && font_huge_) font = font_huge_;
        else if (size == FontSize::Note && font_note_) font = font_note_;
        
        if (!font && font_body_) font = font_body_; // fallback
        if (!font) return 0;

        SDL_Color sdl_color = {color.r, color.g, color.b, color.a};
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, utf8.c_str(), sdl_color);
        if (surf) {
            tex = SDL_CreateTextureFromSurface(renderer_, surf);
            SDL_FreeSurface(surf);
            if (tex) {
                text_cache_[cache_key] = tex;
            }
        }
    }
    
    if (tex) {
        int w = 0, h = 0;
        SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
        SDL_Rect dst = {x, y, w, h};
        SDL_RenderCopy(renderer_, tex, nullptr, &dst);
        return w;
    }
    return 0;
}

int Gfx::text_centered(const std::string& utf8, int cx, int y, FontSize size, Color color) {
    int w = text_width(utf8, size);
    return text(utf8, cx - w / 2, y, size, color);
}

int Gfx::text_width(const std::string& utf8, FontSize size) {
    TTF_Font* font = font_body_;
    if (size == FontSize::Small && font_small_) font = font_small_;
    else if (size == FontSize::Title && font_title_) font = font_title_;
    else if (size == FontSize::Huge && font_huge_) font = font_huge_;
    else if (size == FontSize::Note && font_note_) font = font_note_;
    
    if (!font && font_body_) font = font_body_; // fallback
    if (!font) return 0;

    int w = 0, h = 0;
    TTF_SizeUTF8(font, utf8.c_str(), &w, &h);
    return w;
}

SDL_Texture* Gfx::texture_from_memory(const void* data, size_t size) {
    if (!data || size == 0) return nullptr;
    SDL_RWops* rw = SDL_RWFromConstMem(data, static_cast<int>(size));
    if (!rw) return nullptr;
    SDL_Surface* surface = IMG_Load_RW(rw, 1);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);
    return texture;
}

} // namespace gnx::gfx
