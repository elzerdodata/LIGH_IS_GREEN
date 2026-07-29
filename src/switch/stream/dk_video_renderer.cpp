#include "dk_video_renderer.hpp"

#include <switch.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>
#include <libavutil/nvtegra.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace gnx::stream {

namespace {

constexpr uint32_t kCodeSize = 96 * 1024;  // video vsh + video fsh + hud fsh
constexpr uint32_t kCmdSize = 64 * 1024;
constexpr uint32_t kDataSize = 0x1000;

// data_memblock_ layout (all offsets aligned for their use):
constexpr uint32_t kUniformOff = 0x000;   // Transformation UBO (256B aligned)
constexpr uint32_t kSamplerOff = 0x100;   // sampler descriptor set
constexpr uint32_t kImageOff = 0x200;     // image descriptor set (luma, chroma)
constexpr uint32_t kVtxOff = 0x300;       // quad vertex buffer
constexpr uint32_t kHudVtxOff = 0x380;    // HUD overlay corner quad
constexpr uint32_t kGuideVtxOff = 0x400;  // top-right Guide/Home touch button
constexpr uint32_t kQuickVtxOff = 0x480;  // two-dot handle + quick panel

struct Vertex {
    float position[3];
    float uv[2];
};

// Fullscreen quad. deko3d clip space, UV top-left origin.
constexpr Vertex kQuad[] = {
    {{-1.0f, +1.0f, 0.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
    {{+1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
    {{+1.0f, +1.0f, 0.0f}, {1.0f, 0.0f}},
};

// Top-left HUD overlay quad (deko3d clip space, UV top-left). Its aspect matches
// the 512x160 HUD texture on a 16:9 output so the stats text isn't stretched.
constexpr Vertex kHudQuad[] = {
    {{-0.98f, +0.98f, 0.0f}, {0.0f, 0.0f}},
    {{-0.98f, +0.66f, 0.0f}, {0.0f, 1.0f}},
    {{-0.40f, +0.66f, 0.0f}, {1.0f, 1.0f}},
    {{-0.40f, +0.98f, 0.0f}, {1.0f, 0.0f}},
};

// Invisible 72x72 touch target at 1840,8; the texture itself only paints a
// compact Xbox glyph in its centre.  The top strip clears the status clock.
constexpr Vertex kGuideQuad[] = {
    {{+0.9167f, +0.9852f, 0.0f}, {0.0f, 0.0f}},
    {{+0.9167f, +0.8519f, 0.0f}, {0.0f, 1.0f}},
    {{+0.9917f, +0.8519f, 0.0f}, {1.0f, 1.0f}},
    {{+0.9917f, +0.9852f, 0.0f}, {1.0f, 0.0f}},
};

// Transparent 672x724 quick-menu texture in shared 1920x1080 design space.
// It spans from the panel to the relocated two-dot handle.
constexpr Vertex kQuickQuad[] = {
    {{+0.2083f, +0.9852f, 0.0f}, {0.0f, 0.0f}},
    {{+0.2083f, -0.3556f, 0.0f}, {0.0f, 1.0f}},
    {{+0.9083f, -0.3556f, 0.0f}, {1.0f, 1.0f}},
    {{+0.9083f, +0.9852f, 0.0f}, {1.0f, 0.0f}},
};

// std140 layout for the fragment shader's video conversion and controls.
struct Transformation {
    alignas(16) float yuvmat_col0[4];
    alignas(16) float yuvmat_col1[4];
    alignas(16) float yuvmat_col2[4];
    alignas(16) float offset[4];
    alignas(16) float uv_data[4];
    alignas(16) float sharp_data[4];  // x=strength, y=overshoot allowance
    alignas(16) float picture_data[4];  // brightness, contrast, saturation, gamma
    alignas(16) float motion_data[4];  // x=blend factor, y=enabled
};

static_assert(7 * sizeof(DkImageDescriptor) <= kVtxOff - kImageOff,
              "image descriptors overlap the vertex buffer");
static_assert(sizeof(Transformation) == 128, "std140 Transformation");

// Column-major YUV->RGB matrices (matching Moonlight-Switch / BT.xxx).
const float kBt601Lim[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f,    -0.3917f,
                            2.0172f, 1.5960f, -0.8129f, 0.0f};
const float kBt601Full[9] = {1.0f, 1.0f, 1.0f, 0.0f,     -0.3441f,
                             1.7720f, 1.4020f, -0.7141f, 0.0f};
const float kBt709Lim[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f,    -0.2132f,
                            2.1124f, 1.7927f, -0.5329f, 0.0f};
const float kBt709Full[9] = {1.0f, 1.0f, 1.0f, 0.0f,     -0.1873f,
                             1.8556f, 1.5748f, -0.4681f, 0.0f};
const float kBt2020Lim[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f,    -0.1874f,
                             2.1418f, 1.6781f, -0.6505f, 0.0f};
const float kBt2020Full[9] = {1.0f, 1.0f, 1.0f, 0.0f,     -0.1646f,
                              1.8814f, 1.4746f, -0.5714f, 0.0f};

const float* color_matrix(int space, bool full) {
    switch (space) {
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
            return full ? kBt601Full : kBt601Lim;
        case AVCOL_SPC_BT709:
            return full ? kBt709Full : kBt709Lim;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return full ? kBt2020Full : kBt2020Lim;
        default:
            return full ? kBt601Full : kBt601Lim;
    }
}

}  // namespace

DkVideoRenderer::~DkVideoRenderer() { shutdown(); }

void DkVideoRenderer::set_quick_menu_state(const QuickMenuState& state) {
    QuickMenuState next = normalized_quick_menu(state);
    bool picture_changed =
        next.brightness != quick_state_.brightness ||
        next.contrast != quick_state_.contrast ||
        next.saturation != quick_state_.saturation ||
        next.gamma != quick_state_.gamma ||
        next.sharpness != quick_state_.sharpness;
    bool menu_changed =
        next.open != quick_state_.open ||
        next.performance != quick_state_.performance || picture_changed;
    quick_state_ = next;
    hud_enabled_ = next.performance;
    if (picture_changed) transform_dirty_ = true;
    if (menu_changed) quick_dirty_ = true;
}

void DkVideoRenderer::logf(const char* fmt, ...) {
    if (!log_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_(buf);
}

bool DkVideoRenderer::create_swapchain() {
    dk::ImageLayout fb_layout;
    dk::ImageLayoutMaker{dev_}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                  DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(fb_width_, fb_height_)
        .initialize(fb_layout);
    uint32_t fb_size = fb_layout.getSize();
    uint32_t fb_align = fb_layout.getAlignment();
    fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);

    fb_memblock_ = dk::MemBlockMaker{dev_, kFbNum * fb_size}
                       .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                       .create();
    const DkImage* swap_images[kFbNum];
    for (unsigned i = 0; i < kFbNum; ++i) {
        framebuffers_[i].initialize(fb_layout, fb_memblock_, i * fb_size);
        swap_images[i] = &framebuffers_[i];
    }
    swapchain_ = dk::SwapchainMaker{dev_, nwindowGetDefault(), swap_images, kFbNum}
                     .create();
    if (!swapchain_) {
        logf("deko3d: swapchain create failed (window busy?)");
        return false;
    }
    logf("deko3d: swapchain %ux%u (%s), %u fb", fb_width_, fb_height_,
         fb_mode_ == AppletOperationMode_Console ? "docked" : "handheld", kFbNum);
    return true;
}

void DkVideoRenderer::destroy_swapchain() {
    if (dev_ && queue_) queue_.waitIdle();
    swapchain_ = nullptr;
    fb_memblock_ = nullptr;
}

void DkVideoRenderer::maybe_rebuild_swapchain() {
    int mode = appletGetOperationMode();
    if (mode == fb_mode_) return;  // no dock/undock since last check
    uint32_t w = mode == AppletOperationMode_Console ? 1920 : 1280;
    uint32_t h = mode == AppletOperationMode_Console ? 1080 : 720;
    fb_mode_ = mode;
    if (w == fb_width_ && h == fb_height_) return;  // size unchanged (e.g. 720p dock)
    logf("deko3d: output mode changed -> rebuilding swapchain %ux%u", w, h);
    destroy_swapchain();
    fb_width_ = w;
    fb_height_ = h;
    create_swapchain();
}

bool DkVideoRenderer::init() {
    if (initialized_) return true;

    dev_ = dk::DeviceMaker{}.create();
    if (!dev_) {
        logf("deko3d: device create failed");
        return false;
    }
    queue_ = dk::QueueMaker{dev_}.setFlags(DkQueueFlags_Graphics).create();
    cmdbuf_ = dk::CmdBufMaker{dev_}.create();

    // Size the framebuffer to the current output mode (720p handheld / 1080p
    // docked) and build the swapchain on the default window.
    fb_mode_ = appletGetOperationMode();
    if (fb_mode_ == AppletOperationMode_Console) {
        fb_width_ = 1920;
        fb_height_ = 1080;
    } else {
        fb_width_ = 1280;
        fb_height_ = 720;
    }
    if (!create_swapchain()) return false;

    // Command + data memory.
    cmd_memblock_ = dk::MemBlockMaker{dev_, kCmdSize}
                        .setFlags(DkMemBlockFlags_CpuUncached |
                                  DkMemBlockFlags_GpuCached)
                        .create();
    data_memblock_ = dk::MemBlockMaker{dev_, kDataSize}
                         .setFlags(DkMemBlockFlags_CpuUncached |
                                   DkMemBlockFlags_GpuCached)
                         .create();
    data_cpu_ = data_memblock_.getCpuAddr();
    data_gpu_ = data_memblock_.getGpuAddr();

    // Shaders.
    code_memblock_ = dk::MemBlockMaker{dev_, kCodeSize}
                         .setFlags(DkMemBlockFlags_CpuUncached |
                                   DkMemBlockFlags_GpuCached |
                                   DkMemBlockFlags_Code)
                         .create();
    auto load_shader = [&](dk::Shader& shader, const char* path,
                           uint32_t code_off) -> bool {
        FILE* f = std::fopen(path, "rb");
        if (!f) {
            logf("deko3d: open %s failed", path);
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::rewind(f);
        void* dst = static_cast<uint8_t*>(code_memblock_.getCpuAddr()) + code_off;
        size_t rd = std::fread(dst, 1, size, f);
        std::fclose(f);
        if (rd != static_cast<size_t>(size)) {
            logf("deko3d: read %s short", path);
            return false;
        }
        dk::ShaderMaker{code_memblock_, code_off}.initialize(shader);
        return true;
    };
    if (!load_shader(vertex_shader_, "romfs:/shaders/video_vsh.dksh", 0) ||
        !load_shader(fragment_shader_, "romfs:/shaders/video_fsh.dksh",
                     0x8000) ||
        !load_shader(hud_fsh_, "romfs:/shaders/hud_fsh.dksh", 0x10000)) {
        return false;
    }

    // Vertex buffer (static).
    std::memcpy(static_cast<uint8_t*>(data_cpu_) + kVtxOff, kQuad, sizeof(kQuad));
    std::memcpy(static_cast<uint8_t*>(data_cpu_) + kHudVtxOff, kHudQuad,
                sizeof(kHudQuad));
    std::memcpy(static_cast<uint8_t*>(data_cpu_) + kGuideVtxOff, kGuideQuad,
                sizeof(kGuideQuad));
    std::memcpy(static_cast<uint8_t*>(data_cpu_) + kQuickVtxOff, kQuickQuad,
                sizeof(kQuickQuad));

    // Sampler descriptor (linear, clamp to edge). Written into the descriptor
    // set from the command buffer each frame (canonical deko3d pattern).
    dk::Sampler sampler;
    sampler.setFilter(DkFilter_Linear, DkFilter_Linear);
    sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge,
                        DkWrapMode_ClampToEdge);
    dk::SamplerDescriptor sdesc;
    sdesc.initialize(sampler);
    sampler_desc_ = sdesc;

    // --- Debug HUD text texture (stage 1): a pitch-linear RGBA surface we
    // compose on the CPU (panel background + stats) and sample in the overlay
    // pass. Pitch-linear lets the CPU write pixels straight into the memblock
    // (no staging copy), the same trick the linear video path uses. ---
    hud_pixels_.assign(kHudTexW * kHudTexH, 0);
    hud_text_cache_.clear();
    fps_tick_ = 0;
    fps_frames_ = 0;
    output_frames_ = 0;
    generated_frames_ = 0;
    fps_ = 0.0f;
    output_fps_ = 0.0f;
    generated_fps_ = 0.0f;
    fps_last_data_ = nullptr;
    net_valid_ = false;  // don't show the previous stream's numbers
    {
        uint32_t stride = kHudTexW * 4;
        dk::ImageLayout hud_layout;
        dk::ImageLayoutMaker{dev_}
            .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine |
                      DkImageFlags_PitchLinear)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(kHudTexW, kHudTexH)
            .setPitchStride(stride)
            .initialize(hud_layout);
        uint32_t sz = (hud_layout.getSize() + 0xFFF) & ~0xFFFu;
        hud_memblock_ = dk::MemBlockMaker{dev_, sz}
                            .setFlags(DkMemBlockFlags_CpuUncached |
                                      DkMemBlockFlags_GpuCached |
                                      DkMemBlockFlags_Image)
                            .create();
        hud_cpu_ = hud_memblock_.getCpuAddr();
        hud_image_.initialize(hud_layout, hud_memblock_, 0);
        hud_desc_.initialize(hud_image_);
    }

    // Always-on Guide/Home overlay texture. It is separate from the optional
    // debug HUD so disabling diagnostics never removes the touch control.
    guide_pixels_.assign(kGuideTexW * kGuideTexH, 0);
    {
        uint32_t stride = kGuideTexW * 4;
        dk::ImageLayout guide_layout;
        dk::ImageLayoutMaker{dev_}
            .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine |
                      DkImageFlags_PitchLinear)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(kGuideTexW, kGuideTexH)
            .setPitchStride(stride)
            .initialize(guide_layout);
        uint32_t sz = (guide_layout.getSize() + 0xFFF) & ~0xFFFu;
        guide_memblock_ = dk::MemBlockMaker{dev_, sz}
                              .setFlags(DkMemBlockFlags_CpuUncached |
                                        DkMemBlockFlags_GpuCached |
                                        DkMemBlockFlags_Image)
                              .create();
        guide_cpu_ = guide_memblock_.getCpuAddr();
        guide_image_.initialize(guide_layout, guide_memblock_, 0);
        guide_desc_.initialize(guide_image_);
    }

    // Two-dot handle + expanded quick settings panel. Keeping both in one
    // transparent texture makes open/close a cheap CPU re-rasterization and a
    // single overlay draw call.
    quick_pixels_.assign(kQuickTexW * kQuickTexH, 0);
    {
        uint32_t stride = kQuickTexW * 4;
        dk::ImageLayout quick_layout;
        dk::ImageLayoutMaker{dev_}
            .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine |
                      DkImageFlags_PitchLinear)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(kQuickTexW, kQuickTexH)
            .setPitchStride(stride)
            .initialize(quick_layout);
        uint32_t sz = (quick_layout.getSize() + 0xFFF) & ~0xFFFu;
        quick_memblock_ = dk::MemBlockMaker{dev_, sz}
                              .setFlags(DkMemBlockFlags_CpuUncached |
                                        DkMemBlockFlags_GpuCached |
                                        DkMemBlockFlags_Image)
                              .create();
        quick_cpu_ = quick_memblock_.getCpuAddr();
        quick_image_.initialize(quick_layout, quick_memblock_, 0);
        quick_desc_.initialize(quick_image_);
    }
    // System shared font (pl was initialized in main). A null font falls back to
    // a panel with no text -- still proves the overlay, and never crashes.
    if (!hud_font_) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
            SDL_RWops* rw = SDL_RWFromConstMem(fd.address, fd.size);
            if (rw) hud_font_ = TTF_OpenFontRW(rw, 1, 28);
        }
        if (!hud_font_) logf("deko3d: HUD font unavailable (panel only)");
    }
    rasterize_guide();
    rasterize_quick_menu();

    initialized_ = true;
    logf("deko3d: initialized (fb %ux%u, %u framebuffers)", fb_width_, fb_height_,
         kFbNum);
    return true;
}

void DkVideoRenderer::shutdown() {
    if (dev_ && queue_) queue_.waitIdle();
    if (hud_font_) {
        TTF_CloseFont(hud_font_);  // also frees the RWops (opened with freesrc=1)
        hud_font_ = nullptr;
    }
    mappings_.clear();
    current_mapping_ = -1;
    swapchain_ = nullptr;
    cmdbuf_ = nullptr;
    queue_ = nullptr;
    code_memblock_ = nullptr;
    cmd_memblock_ = nullptr;
    data_memblock_ = nullptr;
    fb_memblock_ = nullptr;
    hud_memblock_ = nullptr;
    hud_cpu_ = nullptr;
    guide_memblock_ = nullptr;
    guide_cpu_ = nullptr;
    guide_pixels_.clear();
    quick_memblock_ = nullptr;
    quick_cpu_ = nullptr;
    quick_pixels_.clear();
    dev_ = nullptr;
    initialized_ = false;
    frame_w_ = frame_h_ = 0;
    color_space_ = -1;
}

bool DkVideoRenderer::ensure_layouts(AVFrame* frame, bool is_linear) {
    // Describe the surface exactly as ffmpeg/NVDEC allocated it (VIC-aligned
    // dims: width to the pitch, height to 32 for luma / 16 for chroma), then
    // crop to the visible area in the shader. Declaring the visible dims and
    // hoping the tiling lines up is what scrambled the picture.
    auto align_up = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
    uint32_t yw = std::max<uint32_t>(frame->linesize[0], frame->width);
    uint32_t yh = align_up(frame->height, 32);
    uint32_t cw = std::max<uint32_t>(frame->linesize[1] / 2, frame->width / 2);
    uint32_t ch = align_up((frame->height + 1) / 2, 16);

    if (frame->width == frame_w_ && frame->height == frame_h_ &&
        yw == luma_w_ && yh == luma_h_ && cw == chroma_w_ && ch == chroma_h_ &&
        is_linear == linear_)
        return true;

    queue_.waitIdle();
    frame_w_ = frame->width;
    frame_h_ = frame->height;
    luma_w_ = yw;
    luma_h_ = yh;
    chroma_w_ = cw;
    chroma_h_ = ch;
    linear_ = is_linear;
    mappings_.clear();
    current_mapping_ = -1;
    transform_dirty_ = true;

    uint32_t flags = DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine |
                     DkImageFlags_UsageVideo;
    flags |= is_linear ? DkImageFlags_PitchLinear : DkImageFlags_CustomTileSize;

    dk::ImageLayoutMaker luma_maker{dev_};
    luma_maker.setFlags(flags)
        .setFormat(DkImageFormat_R8_Unorm)
        .setDimensions(luma_w_, luma_h_);
    dk::ImageLayoutMaker chroma_maker{dev_};
    chroma_maker.setFlags(flags)
        .setFormat(DkImageFormat_RG8_Unorm)
        .setDimensions(chroma_w_, chroma_h_);
    if (is_linear) {
        // Pitch-linear: stride comes straight from the frame.
        luma_maker.setPitchStride(frame->linesize[0]);
        chroma_maker.setPitchStride(frame->linesize[1]);
    } else {
        // Block-linear NVDEC output is always GOB height 2 (16-row blocks).
        // Set it explicitly instead of relying on the UsageVideo default.
        luma_maker.setTileSize(DkTileSize_TwoGobs);
        chroma_maker.setTileSize(DkTileSize_TwoGobs);
    }
    luma_maker.initialize(luma_layout_);
    chroma_maker.initialize(chroma_layout_);

    logf("deko3d: layouts %s vis=%dx%d luma=%ux%u(sz=%u) chroma=%ux%u(sz=%u)",
         is_linear ? "pitch" : "block", frame_w_, frame_h_, luma_w_, luma_h_,
         (uint32_t)luma_layout_.getSize(), chroma_w_, chroma_h_,
         (uint32_t)chroma_layout_.getSize());
    return true;
}

void DkVideoRenderer::update_transform(AVFrame* frame) {
    int space = frame->colorspace;
    bool full = frame->color_range == AVCOL_RANGE_JPEG;
    // NVTEGRA frames often mislabel limited range as JPEG; trust negotiation.
    if (frame->format == AV_PIX_FMT_NVTEGRA &&
        frame->color_range == AVCOL_RANGE_JPEG) {
        full = false;
    }
    if (space == color_space_ && full == color_full_ && !transform_dirty_)
        return;
    color_space_ = space;
    color_full_ = full;
    transform_dirty_ = false;

    Transformation t{};
    const float* m = color_matrix(space, full);
    for (int i = 0; i < 3; ++i) {
        t.yuvmat_col0[i] = m[i];
        t.yuvmat_col1[i] = m[3 + i];
        t.yuvmat_col2[i] = m[6 + i];
    }
    t.offset[0] = full ? 0.0f : 16.0f / 255.0f;
    t.offset[1] = 128.0f / 255.0f;
    t.offset[2] = 128.0f / 255.0f;
    // Crop the aligned surface to the visible area: uv = vTex * (vis/aligned).
    // The luma and chroma ratios are identical (chroma dims are exactly half),
    // so one scale serves both planes.
    t.uv_data[0] = 0.0f;
    t.uv_data[1] = 0.0f;
    t.uv_data[2] = luma_w_ ? (float)frame_w_ / (float)luma_w_ : 1.0f;
    t.uv_data[3] = luma_h_ ? (float)frame_h_ / (float)luma_h_ : 1.0f;
    // Off / Low / Medium / High. Strength scales the unsharp mask; overshoot
    // is how far past the local min/max an edge may ring before it clamps.
    static constexpr float kSharpStrength[4] = {0.0f, 0.60f, 1.20f, 2.0f};
    static constexpr float kSharpOvershoot[4] = {0.0f, 0.02f, 0.035f, 0.05f};
    t.sharp_data[0] = kSharpStrength[quick_state_.sharpness];
    t.sharp_data[1] = kSharpOvershoot[quick_state_.sharpness];
    t.picture_data[0] = static_cast<float>(quick_state_.brightness) / 100.0f;
    t.picture_data[1] = static_cast<float>(quick_state_.contrast) / 100.0f;
    t.picture_data[2] = static_cast<float>(quick_state_.saturation) / 100.0f;
    t.picture_data[3] = static_cast<float>(quick_state_.gamma) / 100.0f;
    std::memcpy(static_cast<uint8_t*>(data_cpu_) + kUniformOff, &t, sizeof(t));
    logf("deko3d: color=%d full=%d crop=%.4fx%.4f picture=%+d/%d/%d gamma=%.2f sharp=%d",
         space, (int)full, t.uv_data[2], t.uv_data[3],
         quick_state_.brightness, quick_state_.contrast,
         quick_state_.saturation, t.picture_data[3], quick_state_.sharpness);
}

DkVideoRenderer::FrameMapping* DkVideoRenderer::map_frame(AVFrame* frame,
                                                          void* base,
                                                          uint32_t handle,
                                                          uint32_t size) {
    // Plane offsets relative to the map base -- do not assume luma sits at 0.
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(base);
    uintptr_t y_addr = reinterpret_cast<uintptr_t>(frame->data[0]);
    uintptr_t uv_addr = reinterpret_cast<uintptr_t>(frame->data[1]);
    if (y_addr < base_addr || uv_addr <= y_addr ||
        uv_addr >= base_addr + size) {
        logf("deko3d: frame planes outside the backing map");
        return nullptr;
    }
    uint32_t luma_off = static_cast<uint32_t>(y_addr - base_addr);
    uint32_t chroma_off = static_cast<uint32_t>(uv_addr - base_addr);

    for (auto& m : mappings_) {
        if (m.handle == handle && m.cpu_addr == base && m.size == size &&
            m.luma_offset == luma_off && m.chroma_offset == chroma_off)
            return &m;
    }

    FrameMapping fm;
    fm.handle = handle;
    fm.cpu_addr = base;
    fm.size = size;
    fm.luma_offset = luma_off;
    fm.chroma_offset = chroma_off;
    fm.memblock = dk::MemBlockMaker{dev_, size}
                      .setFlags(DkMemBlockFlags_CpuUncached |
                                DkMemBlockFlags_GpuCached |
                                DkMemBlockFlags_Image)
                      .setStorage(base)
                      .create();
    if (!fm.memblock) {
        logf("deko3d: import memblock failed (size=%u)", size);
        return nullptr;
    }
    fm.luma.initialize(luma_layout_, fm.memblock, luma_off);
    fm.chroma.initialize(chroma_layout_, fm.memblock, chroma_off);
    fm.luma_desc.initialize(fm.luma);
    fm.chroma_desc.initialize(fm.chroma);
    mappings_.push_back(std::move(fm));
    logf("deko3d: mapped surface handle=%u size=%u yOff=%u uvOff=%u (total %zu)",
         handle, size, luma_off, chroma_off, mappings_.size());
    return &mappings_.back();
}

void DkVideoRenderer::blit_text(const char* s, int x, int y) {
    if (!hud_font_ || !s || !*s) return;
    SDL_Color white{255, 255, 255, 255};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(hud_font_, s, white);
    if (!surf) return;
    SDL_LockSurface(surf);
    int bpp = surf->format->BytesPerPixel;
    for (int j = 0; j < surf->h; ++j) {
        int ty = y + j;
        if (ty < 0 || ty >= static_cast<int>(kHudTexH)) continue;
        auto* rowp = static_cast<uint8_t*>(surf->pixels) + j * surf->pitch;
        for (int i = 0; i < surf->w; ++i) {
            int tx = x + i;
            if (tx < 0 || tx >= static_cast<int>(kHudTexW)) continue;
            uint32_t p = 0;
            std::memcpy(&p, rowp + i * bpp, bpp < 4 ? bpp : 4);
            uint8_t r, g, b, a;
            SDL_GetRGBA(p, surf->format, &r, &g, &b, &a);
            if (a == 0) continue;
            uint32_t& dst = hud_pixels_[ty * kHudTexW + tx];
            uint8_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF,
                    db = (dst >> 16) & 0xFF, da = (dst >> 24) & 0xFF;
            float af = a / 255.0f, ia = 1.0f - af;
            uint8_t nr = static_cast<uint8_t>(r * af + dr * ia);
            uint8_t ng = static_cast<uint8_t>(g * af + dg * ia);
            uint8_t nb = static_cast<uint8_t>(b * af + db * ia);
            uint8_t na = static_cast<uint8_t>(a + da * ia);
            dst = nr | (ng << 8) | (nb << 16) | (static_cast<uint32_t>(na) << 24);
        }
    }
    SDL_UnlockSurface(surf);
    SDL_FreeSurface(surf);
}

void DkVideoRenderer::blit_quick_text(const char* s, int x, int y) {
    if (!hud_font_ || !s || !*s) return;
    SDL_Color white{255, 255, 255, 255};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(hud_font_, s, white);
    if (!surf) return;
    SDL_LockSurface(surf);
    int bpp = surf->format->BytesPerPixel;
    for (int j = 0; j < surf->h; ++j) {
        int ty = y + j;
        if (ty < 0 || ty >= static_cast<int>(kQuickTexH)) continue;
        auto* rowp = static_cast<uint8_t*>(surf->pixels) + j * surf->pitch;
        for (int i = 0; i < surf->w; ++i) {
            int tx = x + i;
            if (tx < 0 || tx >= static_cast<int>(kQuickTexW)) continue;
            uint32_t p = 0;
            std::memcpy(&p, rowp + i * bpp, bpp < 4 ? bpp : 4);
            uint8_t r, g, b, a;
            SDL_GetRGBA(p, surf->format, &r, &g, &b, &a);
            if (a == 0) continue;
            uint32_t& dst = quick_pixels_[ty * kQuickTexW + tx];
            uint8_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF,
                    db = (dst >> 16) & 0xFF, da = (dst >> 24) & 0xFF;
            float af = a / 255.0f, ia = 1.0f - af;
            uint8_t nr = static_cast<uint8_t>(r * af + dr * ia);
            uint8_t ng = static_cast<uint8_t>(g * af + dg * ia);
            uint8_t nb = static_cast<uint8_t>(b * af + db * ia);
            uint8_t na = static_cast<uint8_t>(a + da * ia);
            dst = nr | (ng << 8) | (nb << 16) |
                  (static_cast<uint32_t>(na) << 24);
        }
    }
    SDL_UnlockSurface(surf);
    SDL_FreeSurface(surf);
}

void DkVideoRenderer::rasterize_guide() {
    auto rgba = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(a) << 24);
    };
    const uint32_t shadow = rgba(0, 0, 0, guide_pressed_ ? 180 : 125);
    const uint32_t icon = guide_pressed_ ? rgba(255, 255, 255, 255)
                                          : rgba(16, 124, 16, 255);
    const uint32_t mark = guide_pressed_ ? rgba(16, 124, 16, 255)
                                         : rgba(255, 255, 255, 255);
    std::fill(guide_pixels_.begin(), guide_pixels_.end(), 0);

    // Symbol only: a soft circular shadow replaces the old green square tile.
    constexpr int cx = 48, cy = 48, shadow_radius = 26, radius = 22;
    for (int y = cy - shadow_radius; y <= cy + shadow_radius; ++y) {
        for (int x = cx - shadow_radius; x <= cx + shadow_radius; ++x) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= shadow_radius * shadow_radius)
                guide_pixels_[y * kGuideTexW + x] = shadow;
        }
    }
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius)
                guide_pixels_[y * kGuideTexW + x] = icon;
        }
    }
    // A high-contrast X inside the circular Xbox badge.
    for (int y = cy - 12; y <= cy + 12; ++y) {
        for (int x = cx - 12; x <= cx + 12; ++x) {
            int dx = x - cx, dy = y - cy;
            if (std::abs(std::abs(dx) - std::abs(dy)) <= 2)
                guide_pixels_[y * kGuideTexW + x] = mark;
        }
    }
    if (guide_cpu_)
        std::memcpy(guide_cpu_, guide_pixels_.data(), guide_pixels_.size() * 4);
    guide_rasterized_pressed_ = guide_pressed_;
}

void DkVideoRenderer::rasterize_quick_menu() {
    auto rgba = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(a) << 24);
    };
    auto fill = [&](int x, int y, int w, int h, uint32_t color) {
        int x0 = std::clamp(x, 0, static_cast<int>(kQuickTexW));
        int y0 = std::clamp(y, 0, static_cast<int>(kQuickTexH));
        int x1 = std::clamp(x + w, 0, static_cast<int>(kQuickTexW));
        int y1 = std::clamp(y + h, 0, static_cast<int>(kQuickTexH));
        for (int py = y0; py < y1; ++py)
            std::fill(quick_pixels_.begin() + py * kQuickTexW + x0,
                      quick_pixels_.begin() + py * kQuickTexW + x1, color);
    };
    auto frame = [&](int x, int y, int w, int h, int thickness,
                     uint32_t color) {
        fill(x, y, w, thickness, color);
        fill(x, y + h - thickness, w, thickness, color);
        fill(x, y, thickness, h, color);
        fill(x + w - thickness, y, thickness, h, color);
    };
    auto circle = [&](int cx, int cy, int radius, uint32_t color) {
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= radius * radius &&
                    x >= 0 && x < static_cast<int>(kQuickTexW) &&
                    y >= 0 && y < static_cast<int>(kQuickTexH))
                    quick_pixels_[y * kQuickTexW + x] = color;
            }
        }
    };
    auto local = [](const QuickRect& r) {
        return QuickRect{r.x - kQuickTextureRect.x,
                         r.y - kQuickTextureRect.y, r.w, r.h};
    };

    const uint32_t transparent = 0;
    const uint32_t panel = rgba(5, 12, 17, 240);
    const uint32_t surface = rgba(12, 25, 30, 226);
    const uint32_t edge = rgba(42, 74, 72, 255);
    const uint32_t accent = rgba(57, 224, 103, 255);
    const uint32_t active = rgba(16, 124, 16, 240);
    const uint32_t glyph_shadow = rgba(0, 0, 0, 150);
    const uint32_t text = rgba(255, 255, 255, 255);

    std::fill(quick_pixels_.begin(), quick_pixels_.end(), transparent);

    QuickRect toggle = local(kQuickToggleRect);
    // Keep the full 72x72 hit target transparent. Only two compact glyph dots
    // are visible; active state is conveyed by color, never by a square frame.
    circle(toggle.x + 23, toggle.y + toggle.h / 2 + 2, 9, glyph_shadow);
    circle(toggle.x + 49, toggle.y + toggle.h / 2 + 2, 9, glyph_shadow);
    const uint32_t dot = quick_state_.open ? accent : text;
    circle(toggle.x + 23, toggle.y + toggle.h / 2, 6, dot);
    circle(toggle.x + 49, toggle.y + toggle.h / 2, 6, dot);

    if (quick_state_.open) {
        QuickRect menu = local(kQuickPanelRect);
        fill(menu.x, menu.y, menu.w, menu.h, panel);
        frame(menu.x, menu.y, menu.w, menu.h, 3, accent);
        blit_quick_text("IMAGE & STATS", menu.x + 20, menu.y + 16);

        const char* labels[kQuickRowCount] = {
            "Performance", "Brightness", "Contrast", "Saturation", "Gamma",
            "Sharpness"};
        const char* sharp_labels[4] = {"Off", "Low", "Medium", "High"};

        for (int row = 0; row < kQuickRowCount; ++row) {
            QuickRect rr = local(quick_row_rect(row));
            fill(rr.x, rr.y, rr.w, rr.h, surface);
            fill(rr.x + 14, rr.y + rr.h - 1, rr.w - 28, 1, edge);
            blit_quick_text(labels[row], rr.x + 14, rr.y + 12);

            if (row == QuickPerformance) {
                fill(rr.x + 350, rr.y + 6, 104, rr.h - 12,
                     quick_state_.performance ? active : rgba(42, 49, 60, 240));
                frame(rr.x + 350, rr.y + 6, 104, rr.h - 12, 2,
                      quick_state_.performance ? accent : edge);
                blit_quick_text(quick_state_.performance ? "ON" : "OFF",
                                rr.x + 378, rr.y + 12);
                continue;
            }

            QuickRect minus = local(quick_minus_rect(row));
            QuickRect plus = local(quick_plus_rect(row));
            fill(minus.x, minus.y + 4, minus.w, minus.h - 8, rgba(42, 49, 60, 240));
            fill(plus.x, plus.y + 4, plus.w, plus.h - 8, rgba(42, 49, 60, 240));
            frame(minus.x, minus.y + 4, minus.w, minus.h - 8, 2, edge);
            frame(plus.x, plus.y + 4, plus.w, plus.h - 8, 2, edge);
            blit_quick_text("-", minus.x + 25, minus.y + 10);
            blit_quick_text("+", plus.x + 22, plus.y + 10);

            char value[24];
            if (row == QuickBrightness)
                std::snprintf(value, sizeof(value), "%+d", quick_state_.brightness);
            else if (row == QuickContrast)
                std::snprintf(value, sizeof(value), "%d%%", quick_state_.contrast);
            else if (row == QuickSaturation)
                std::snprintf(value, sizeof(value), "%d%%", quick_state_.saturation);
            else if (row == QuickGamma)
                std::snprintf(value, sizeof(value), "%.2f",
                              quick_state_.gamma / 100.0f);
            else
                std::snprintf(value, sizeof(value), "%s",
                              sharp_labels[quick_state_.sharpness]);
            blit_quick_text(value, rr.x + 300, rr.y + 12);
        }

        QuickRect reset = local(kQuickResetRect);
        fill(reset.x, reset.y, reset.w, reset.h, surface);
        frame(reset.x, reset.y, reset.w, reset.h, 2, accent);
        blit_quick_text("RESET IMAGE", reset.x + 55, reset.y + 10);
    }

    if (quick_cpu_)
        std::memcpy(quick_cpu_, quick_pixels_.data(),
                    quick_pixels_.size() * sizeof(uint32_t));
    quick_dirty_ = false;
}

void DkVideoRenderer::rasterize_hud() {
    // deko3d RGBA8_Unorm byte order is R,G,B,A -> pack little-endian. Fill the
    // panel background (black, ~0.55 alpha), then composite the white text.
    const uint32_t bg = static_cast<uint32_t>(140) << 24;
    std::fill(hud_pixels_.begin(), hud_pixels_.end(), bg);
    if (hud_font_) {
        int y = 8;
        int skip = TTF_FontLineSkip(hud_font_);
        size_t start = 0;
        while (start <= hud_text_cache_.size()) {
            size_t nl = hud_text_cache_.find('\n', start);
            std::string line = hud_text_cache_.substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            blit_text(line.c_str(), 12, y);
            y += skip;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    if (hud_cpu_)
        std::memcpy(hud_cpu_, hud_pixels_.data(), hud_pixels_.size() * 4);
}

void DkVideoRenderer::update_hud(AVFrame* frame) {
    uint64_t now = armGetSystemTick();
    uint64_t freq = armGetSystemTickFreq();
    if (fps_tick_ == 0) fps_tick_ = now;
    // Count distinct source frames, not present ticks: the renderer re-presents
    // the held frame every ~60 Hz tick, so counting render passes would read ~60
    // even for a 30 fps source. A newly decoded frame carries a new surface in
    // data[0]; a re-presented frame keeps the previous pointer.
    if (frame->data[0] != fps_last_data_) {
        ++fps_frames_;
        fps_last_data_ = frame->data[0];
    }
    uint64_t dt = now - fps_tick_;
    if (dt >= freq / 2) {  // recompute FPS over ~0.5 s windows
        fps_ = static_cast<float>(fps_frames_) * static_cast<float>(freq) /
               static_cast<float>(dt);
        output_fps_ = static_cast<float>(output_frames_) *
                      static_cast<float>(freq) / static_cast<float>(dt);
        generated_fps_ = static_cast<float>(generated_frames_) *
                         static_cast<float>(freq) / static_cast<float>(dt);
        fps_frames_ = 0;
        output_frames_ = 0;
        generated_frames_ = 0;
        fps_tick_ = now;
    }
    char buf[220];
    if (net_valid_.load(std::memory_order_relaxed)) {
        std::snprintf(buf, sizeof(buf),
                      "%dx%d\nsrc %.0f  out %.0f  gen %.0f fps\n"
                      "%.1f Mbps  loss %.1f%%\nbuf %dms",
                      frame->width, frame->height, fps_, output_fps_,
                      generated_fps_,
                      net_mbps_.load(std::memory_order_relaxed),
                      net_loss_.load(std::memory_order_relaxed),
                      net_buffer_ms_.load(std::memory_order_relaxed));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "%dx%d\nsrc %.0f  out %.0f  gen %.0f fps",
                      frame->width, frame->height, fps_, output_fps_,
                      generated_fps_);
    }
    if (hud_text_cache_ == buf) return;  // unchanged -> keep the current texture
    hud_text_cache_ = buf;
    rasterize_hud();
}

bool DkVideoRenderer::render(AVFrame* frame, AVFrame* motion_frame,
                             float motion_blend) {
    if (!initialized_) return false;
    maybe_rebuild_swapchain();  // follow dock/undock (720p <-> 1080p)
    if (!swapchain_) return false;
    if (frame->format != AV_PIX_FMT_NVTEGRA) {
        if (!warned_not_hw_) {
            logf("deko3d: frame is not NVTEGRA (fmt=%d) -- cannot zero-copy",
                 frame->format);
            warned_not_hw_ = true;
        }
        return false;
    }

    AVNVTegraMap* map = av_nvtegra_frame_get_fbuf_map(frame);
    if (!map) {
        logf("deko3d: frame has no nvtegra map");
        return false;
    }
    void* base = av_nvtegra_map_get_addr(map);
    uint32_t handle = av_nvtegra_map_get_handle(map);
    uint32_t size = av_nvtegra_map_get_size(map);
    if (!base || !size) {
        logf("deko3d: nvtegra map not CPU-visible");
        return false;
    }

    if (!logged_surface_) {
        logged_surface_ = true;
        const char* swname = "?";
        if (frame->hw_frames_ctx) {
            auto* fctx =
                reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
            const char* n = av_get_pix_fmt_name(fctx->sw_format);
            if (n) swname = n;
        }
        logf("deko3d: surface sw_fmt=%s layout=%s linesize=[%d,%d] "
             "yOff=%u uvOff=%u mapSize=%u",
             swname, map->is_linear ? "pitch-linear" : "block-linear",
             frame->linesize[0], frame->linesize[1],
             (uint32_t)(frame->data[0] -
                        static_cast<uint8_t*>(base)),
             (uint32_t)(frame->data[1] -
                        static_cast<uint8_t*>(base)),
             size);
    }

    ensure_layouts(frame, map->is_linear);
    update_transform(frame);
    FrameMapping* fm = map_frame(frame, base, handle, size);
    if (!fm) return false;

    // Motion mode presents a lightweight midpoint between consecutive decoded
    // surfaces. It intentionally reuses the same zero-copy mapping path: no CPU
    // readback and no extra decoded-frame upload are introduced.
    FrameMapping* motion_fm = nullptr;
    if (motion_frame && motion_blend > 0.0f &&
        motion_frame->format == AV_PIX_FMT_NVTEGRA &&
        motion_frame->width == frame->width &&
        motion_frame->height == frame->height) {
        AVNVTegraMap* motion_map = av_nvtegra_frame_get_fbuf_map(motion_frame);
        if (motion_map && motion_map->is_linear == map->is_linear &&
            motion_frame->linesize[0] == frame->linesize[0] &&
            motion_frame->linesize[1] == frame->linesize[1]) {
            void* motion_base = av_nvtegra_map_get_addr(motion_map);
            uint32_t motion_handle = av_nvtegra_map_get_handle(motion_map);
            uint32_t motion_size = av_nvtegra_map_get_size(motion_map);
            if (motion_base && motion_size) {
                motion_fm = map_frame(motion_frame, motion_base, motion_handle,
                                      motion_size);
            }
        }
    }
    const float blend = motion_fm ? std::clamp(motion_blend, 0.0f, 1.0f) : 0.0f;
    auto* transform = reinterpret_cast<Transformation*>(
        static_cast<uint8_t*>(data_cpu_) + kUniformOff);
    transform->motion_data[0] = blend;
    transform->motion_data[1] = motion_fm ? 1.0f : 0.0f;
    transform->motion_data[2] = 0.0f;
    transform->motion_data[3] = 0.0f;

    // Recompute HUD stats + re-rasterize the text texture now, while the GPU is
    // idle (render() ends with waitIdle) and before we record any commands.
    if (hud_enabled_) {
        ++output_frames_;
        if (motion_fm) ++generated_frames_;
        update_hud(frame);
    }
    if (guide_pressed_ != guide_rasterized_pressed_) rasterize_guide();
    if (quick_dirty_) rasterize_quick_menu();

    int slot = queue_.acquireImage(swapchain_);

    cmdbuf_.clear();
    cmdbuf_.addMemory(cmd_memblock_, 0, kCmdSize);

    // Write sampler + image descriptors through the command buffer (canonical
    // deko3d path) so the writes are ordered on the GPU timeline before use.
    cmdbuf_.pushData(data_gpu_ + kSamplerOff, &sampler_desc_,
                     sizeof(DkSamplerDescriptor));
    cmdbuf_.pushData(data_gpu_ + kImageOff, &fm->luma_desc,
                     sizeof(DkImageDescriptor));
    cmdbuf_.pushData(data_gpu_ + kImageOff + sizeof(DkImageDescriptor),
                     &fm->chroma_desc, sizeof(DkImageDescriptor));
    // Image descriptor #2 = the HUD text texture (sampled by the overlay pass).
    cmdbuf_.pushData(data_gpu_ + kImageOff + 2 * sizeof(DkImageDescriptor),
                     &hud_desc_, sizeof(DkImageDescriptor));
    // Image descriptor #3 = the always-on Xbox Guide/Home touch overlay.
    cmdbuf_.pushData(data_gpu_ + kImageOff + 3 * sizeof(DkImageDescriptor),
                     &guide_desc_, sizeof(DkImageDescriptor));
    // Image descriptor #4 = two-dot quick menu handle + optional panel.
    cmdbuf_.pushData(data_gpu_ + kImageOff + 4 * sizeof(DkImageDescriptor),
                     &quick_desc_, sizeof(DkImageDescriptor));
    // Image descriptors #5/#6 = the next decoded frame used by Motion. When
    // Motion is inactive they alias the current frame, keeping all samplers
    // valid without requiring a separate shader variant.
    FrameMapping* next = motion_fm ? motion_fm : fm;
    cmdbuf_.pushData(data_gpu_ + kImageOff + 5 * sizeof(DkImageDescriptor),
                     &next->luma_desc, sizeof(DkImageDescriptor));
    cmdbuf_.pushData(data_gpu_ + kImageOff + 6 * sizeof(DkImageDescriptor),
                     &next->chroma_desc, sizeof(DkImageDescriptor));

    dk::ImageView view{framebuffers_[slot]};
    cmdbuf_.bindRenderTargets({&view});
    cmdbuf_.setViewports(0, {{0.0f, 0.0f, (float)fb_width_, (float)fb_height_,
                              0.0f, 1.0f}});
    cmdbuf_.setScissors(0, {{0, 0, fb_width_, fb_height_}});
    cmdbuf_.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

    dk::RasterizerState rasterizer;
    rasterizer.setCullMode(DkFace_None);
    dk::ColorState color;
    dk::ColorWriteState color_write;
    dk::DepthStencilState depth_stencil;
    depth_stencil.setDepthTestEnable(false);
    depth_stencil.setDepthWriteEnable(false);

    cmdbuf_.bindShaders(DkStageFlag_GraphicsMask,
                        {&vertex_shader_, &fragment_shader_});
    cmdbuf_.bindRasterizerState(rasterizer);
    cmdbuf_.bindColorState(color);
    cmdbuf_.bindColorWriteState(color_write);
    cmdbuf_.bindDepthStencilState(depth_stencil);

    cmdbuf_.bindSamplerDescriptorSet(data_gpu_ + kSamplerOff, 1);
    cmdbuf_.bindImageDescriptorSet(data_gpu_ + kImageOff, 7);
    cmdbuf_.barrier(DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

    cmdbuf_.bindTextures(DkStage_Fragment, 0,
                         {dkMakeTextureHandle(0, 0), dkMakeTextureHandle(1, 0),
                          dkMakeTextureHandle(5, 0), dkMakeTextureHandle(6, 0)});
    cmdbuf_.bindUniformBuffer(DkStage_Fragment, 0, data_gpu_ + kUniformOff, 256);

    cmdbuf_.bindVtxAttribState(
        {DkVtxAttribState{0, 0, offsetof(Vertex, position), DkVtxAttribSize_3x32,
                          DkVtxAttribType_Float, 0},
         DkVtxAttribState{0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32,
                          DkVtxAttribType_Float, 0}});
    cmdbuf_.bindVtxBufferState({DkVtxBufferState{sizeof(Vertex), 0}});
    cmdbuf_.bindVtxBuffer(0, data_gpu_ + kVtxOff, sizeof(kQuad));

    cmdbuf_.draw(DkPrimitive_Quads, 4, 1, 0, 0);

    // --- Alpha-blended overlays. The debug HUD is optional; the top-right
    // Xbox Guide/Home touch control is always drawn during native streaming.
    dk::BlendState overlay_blend;
    overlay_blend.setColorBlendOp(DkBlendOp_Add);
    overlay_blend.setSrcColorBlendFactor(DkBlendFactor_SrcAlpha);
    overlay_blend.setDstColorBlendFactor(DkBlendFactor_InvSrcAlpha);
    overlay_blend.setAlphaBlendOp(DkBlendOp_Add);
    overlay_blend.setSrcAlphaBlendFactor(DkBlendFactor_One);
    overlay_blend.setDstAlphaBlendFactor(DkBlendFactor_InvSrcAlpha);
    dk::ColorState overlay_color;
    overlay_color.setBlendEnable(0, true);
    cmdbuf_.bindBlendStates(0, {overlay_blend});
    cmdbuf_.bindColorState(overlay_color);
    cmdbuf_.bindShaders(DkStageFlag_GraphicsMask,
                        {&vertex_shader_, &hud_fsh_});
    if (hud_enabled_) {
        cmdbuf_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(2, 0)});
        cmdbuf_.bindVtxBuffer(0, data_gpu_ + kHudVtxOff, sizeof(kHudQuad));
        cmdbuf_.draw(DkPrimitive_Quads, 4, 1, 0, 0);
    }
    cmdbuf_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(4, 0)});
    cmdbuf_.bindVtxBuffer(0, data_gpu_ + kQuickVtxOff, sizeof(kQuickQuad));
    cmdbuf_.draw(DkPrimitive_Quads, 4, 1, 0, 0);
    cmdbuf_.bindTextures(DkStage_Fragment, 0, {dkMakeTextureHandle(3, 0)});
    cmdbuf_.bindVtxBuffer(0, data_gpu_ + kGuideVtxOff, sizeof(kGuideQuad));
    cmdbuf_.draw(DkPrimitive_Quads, 4, 1, 0, 0);

    queue_.submitCommands(cmdbuf_.finishList());
    queue_.presentImage(swapchain_, slot);
    queue_.waitIdle();
    return true;
}

}  // namespace gnx::stream
