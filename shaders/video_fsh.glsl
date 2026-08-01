#version 460

// H.264 NV12 -> RGB. plane0 is the R8 luma image, plane1 is the RG8 chroma
// image (U in .r, V in .g), both zero-copy views of the NVTEGRA decoder
// surface. Separate visible-area transforms keep each plane away from NVDEC's
// aligned padding rows, which otherwise bleed into the bottom/right edge.
layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D plane0;
layout (binding = 1) uniform sampler2D plane1;
layout (binding = 2) uniform sampler2D motionPlane0;
layout (binding = 3) uniform sampler2D motionPlane1;

layout (std140, binding = 0) uniform Transformation
{
    mat3 yuvmat;
    vec3 offset;
    vec4 luma_uv_data;
    vec4 chroma_uv_data;
    vec4 sharp_data;
    // x=brightness, y=contrast, z=saturation, w=gamma (1.0 is neutral).
    vec4 picture_data;
    // x=blend factor, y=enabled. Experimental Motion uses 0.5 for the generated
    // midpoint between adjacent 30 fps source frames.
    vec4 motion_data;
    // x=temperature (-1.0 to 1.0)
    vec4 color_tune_data;
} u;

void main()
{
    vec2 lumaUv = u.luma_uv_data.xy +
                  vTextureCoord * u.luma_uv_data.zw;
    vec2 chromaUv = u.chroma_uv_data.xy +
                    vTextureCoord * u.chroma_uv_data.zw;
    vec2 lumaMin = u.luma_uv_data.xy;
    vec2 lumaMax = u.luma_uv_data.xy + u.luma_uv_data.zw;

    // Luma-only unsharp mask (chroma untouched, so no color fringing). The
    // base stream is soft -- H.264 at streaming bitrates smears fine detail.
    // sharp_data.x is the strength; .y bounds the overshoot: the result may
    // exceed the local neighborhood min/max only by that allowance, which is
    // what keeps hard edges from growing halos.
    float y = texture(plane0, lumaUv).r;
    vec2 chroma = texture(plane1, chromaUv).rg;
    if (u.motion_data.y > 0.5) {
        y = mix(y, texture(motionPlane0, lumaUv).r, u.motion_data.x);
    }
    if (u.sharp_data.x > 0.0) {
        vec2 px = 1.0 / vec2(textureSize(plane0, 0));
        vec2 uvLeft = clamp(lumaUv - vec2(px.x, 0.0), lumaMin, lumaMax);
        vec2 uvRight = clamp(lumaUv + vec2(px.x, 0.0), lumaMin, lumaMax);
        vec2 uvUp = clamp(lumaUv - vec2(0.0, px.y), lumaMin, lumaMax);
        vec2 uvDown = clamp(lumaUv + vec2(0.0, px.y), lumaMin, lumaMax);
        float yl = texture(plane0, uvLeft).r;
        float yr = texture(plane0, uvRight).r;
        float yu = texture(plane0, uvUp).r;
        float yd = texture(plane0, uvDown).r;
        if (u.motion_data.y > 0.5) {
            yl = mix(yl, texture(motionPlane0, uvLeft).r, u.motion_data.x);
            yr = mix(yr, texture(motionPlane0, uvRight).r, u.motion_data.x);
            yu = mix(yu, texture(motionPlane0, uvUp).r, u.motion_data.x);
            yd = mix(yd, texture(motionPlane0, uvDown).r, u.motion_data.x);
        }
        float average = 0.25 * (yl + yr + yu + yd);
        float lo = min(y, min(min(yl, yr), min(yu, yd)));
        float hi = max(y, max(max(yl, yr), max(yu, yd)));
        y = clamp(y + (y - average) * u.sharp_data.x,
                  max(0.0, lo - u.sharp_data.y),
                  min(1.0, hi + u.sharp_data.y));
    }

    vec3 yuv = vec3(y, chroma.r, chroma.g) - u.offset;
    vec3 rgb = u.yuvmat * yuv;

    // Lightweight post-conversion picture controls. Defaults (0, 1, 1) are
    // exactly neutral; these add only a handful of ALU operations and no
    // extra texture samples.
    rgb = (rgb - vec3(0.5)) * u.picture_data.y + vec3(0.5);
    float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luminance), rgb, u.picture_data.z);
    rgb += vec3(u.picture_data.x);
    // User-facing gamma follows display convention: values above 1.0 lift
    // midtones, values below 1.0 deepen them while preserving black and white.
    rgb = pow(clamp(rgb, 0.0, 1.0),
              vec3(1.0 / max(u.picture_data.w, 0.01)));

    float temperature = clamp(u.color_tune_data.x, -1.0, 1.0);
    rgb += vec3(
        temperature * 0.035,
        temperature * 0.008,
       -temperature * 0.035
    );

    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
