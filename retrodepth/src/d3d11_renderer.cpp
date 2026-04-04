#include "d3d11_renderer.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>
#include <iostream>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Embedded HLSL shaders
// ---------------------------------------------------------------------------

static const char* k_shader_src = R"HLSL(

cbuffer PerDraw : register(b0) {
    float4x4 gVP;         // VP matrix for layers; full MVP for overlay/controller
    float    gDepth;      // layer depth in metres (0 for overlay/controller)
    float    gQuadW;      // quad width in metres  (1 for overlay/controller)
    float    gQuadH;      // quad height in metres (1 for overlay/controller)
    float    gQuadY;      // vertical centre in stage space (0 for overlay/controller)
    float4   gTint;
    float    gContrast;   // 1.0 = no change
    float    gSaturation; // 1.0 = full color, 0.0 = greyscale
    float    gGamma;      // 1.0 = no change, >1.0 = darker midtones (CRT-like)
    float    gRotate90;   // 1.0 = rotate UV 90° CW (portrait games shown landscape)
    float    gRoundness;  // sphere/cone effect: >0 = mid-copy bulge, <0 = closer-bigger cone
    float    gCopyCount;  // active depth copies for this layer
    float    gCopySpan;   // total depth span of the copies in metres
    float    gDepthMap;   // 1.0 = per-pixel depth map, 0.0 = flat duplicated slices
    float    gUpscale;    // 1.0 = sharpened upscale for game layers, 0.0 = nearest-neighbor
    float    gScreenCurve;// curved screen amount: + = concave, - = convex
    float2   gPad0;
};

struct VS_IN {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};
struct VS_OUT {
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float  copy_t  : TEXCOORD1;
    float  copy_id : TEXCOORD2;
};

VS_OUT VSMain(VS_IN i, uint inst : SV_InstanceID) {
    // inst == 0  → main quad (no offset)
    // inst  > 0  → copy in FRONT of main quad (closer to viewer).
    float copy_count = max(1.0f, gCopyCount);
    float t = min((float)inst, copy_count) / copy_count;
    float offset = t * gCopySpan;
    float d      = max(0.01f, gDepth - offset);

    // Rounded effect: scale each copy's quad size based on its position in the stack.
    // t=0 is the deepest copy (main quad), t=1 is the closest copy.
    float scale;
    if (gRoundness >= 0.0f) {
        // Positive: sphere bulge — middle copies are biggest (sin curve)
        scale = 1.0f + gRoundness * sin(t * 3.14159265f);
    } else {
        // Negative: cone ramp — closer copies are bigger (linear)
        scale = 1.0f + (-gRoundness) * t;
    }

    float curve_x = i.pos.x * 2.0f;
    float curve_depth = gScreenCurve * gQuadW * 0.18f * (curve_x * curve_x);
    d = max(0.01f, d + curve_depth);

    VS_OUT o;
    o.pos = mul(float4(i.pos.x * gQuadW * scale,
                       i.pos.y * gQuadH * scale + gQuadY,
                       -d, 1.0f), gVP);
    o.uv = i.uv;
    o.copy_t = t;
    o.copy_id = (float)inst;
    return o;
}

Texture2D    gTex           : register(t0);
SamplerState gPointSampler  : register(s0);
SamplerState gLinearSampler : register(s1);

float4 sample_game_tex(float2 uv) {
    [branch] if (gUpscale < 0.5f || gDepth <= 0.0f) {
        return gTex.SampleLevel(gPointSampler, uv, 0.0f);
    }

    uint tex_w, tex_h;
    gTex.GetDimensions(tex_w, tex_h);
    float2 tex_size = float2((float)tex_w, (float)tex_h);
    float2 inv_tex = 1.0f / tex_size;

    // Start from a "sharp bilinear" style sample: keep texel centers broad
    // and compress the transition zone so the image does not wash out.
    float2 texel = uv * tex_size - 0.5f;
    float2 texel_base = floor(texel);
    float2 frac = texel - texel_base;
    float2 sharp_frac = saturate(0.5f + (frac - 0.5f) * 1.55f);
    float2 sharp_uv = (texel_base + sharp_frac + 0.5f) * inv_tex;

    float4 center = gTex.SampleLevel(gLinearSampler, sharp_uv, 0.0f);

    // Mild unsharp mask on RGB only. This gives a perception of extra detail
    // without the full blur of bicubic filtering.
    float3 blur =
        gTex.SampleLevel(gLinearSampler, sharp_uv + float2(-inv_tex.x, 0.0f), 0.0f).rgb +
        gTex.SampleLevel(gLinearSampler, sharp_uv + float2( inv_tex.x, 0.0f), 0.0f).rgb +
        gTex.SampleLevel(gLinearSampler, sharp_uv + float2(0.0f, -inv_tex.y), 0.0f).rgb +
        gTex.SampleLevel(gLinearSampler, sharp_uv + float2(0.0f,  inv_tex.y), 0.0f).rgb;
    blur *= 0.25f;

    float3 rgb = saturate(center.rgb + (center.rgb - blur) * 0.32f);
    float alpha = max(center.a, gTex.SampleLevel(gPointSampler, uv, 0.0f).a);
    return float4(rgb, alpha);
}

float4 PSMain(VS_OUT i) : SV_Target {
    float2 uv = i.uv;
    [branch] if (gRotate90 > 0.5f) {
        uv = float2(1.0f - i.uv.y, i.uv.x); // 90° CW: landscape texture → portrait display
    }
    float4 raw_key = gTex.SampleLevel(gPointSampler, uv, 0.0f);
    float4 raw = sample_game_tex(uv);
    clip(raw_key.a - 0.001f);
    float4 c = raw * gTint;
    [branch] if (gDepth > 0.0f) {
        [branch] if (gDepthMap > 0.5f && i.copy_id > 0.0f) {
            // Keep depthmap subtle: map dark→bright across only 20% of the copy stack,
            // instead of the full stack, so it reads as relief rather than extrusion.
            float depth_luma = dot(saturate(raw.rgb), float3(0.2126f, 0.7152f, 0.0722f));
            depth_luma *= 0.20f;
            if (i.copy_t > depth_luma)
                discard;
        }
        // Gamma (darkens/lightens midtones; >1 = darker, like CRT shadow rolloff)
        c.rgb = pow(saturate(c.rgb), gGamma);
        // Saturation
        float lum = dot(c.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        c.rgb = lerp(float3(lum, lum, lum), c.rgb, gSaturation);
        // Contrast (scale around midpoint 0.5)
        c.rgb = saturate((c.rgb - 0.5f) * gContrast + 0.5f);
    }
    return c;
}

)HLSL";

// CPU-side maximum number of copy slices we ever submit for a layer.
static constexpr int N_COPIES = D3D11Renderer::k_max_depth_copies;
static constexpr int k_flat_vb_capacity = 1024;

static int copy_count_for_frame(const LayerFrame& f) {
    if (!f.copies.empty())
        return (int)(std::min)(f.copies.size(), (size_t)N_COPIES);
    return N_COPIES;
}

static float copy_span_for_frame(const LayerFrame& f, int active_copies) {
    if (active_copies <= 0) return 0.0f;
    if (!f.copies.empty())
        return (std::max)(0.0f, f.copies[active_copies - 1]);
    return (float)active_copies * D3D11Renderer::k_default_copy_step_m;
}

// ---------------------------------------------------------------------------
// Flat-color shader (for laser beams and reticles)
// Uses a separate constant buffer at register b1 so it doesn't clash with
// the existing PerDraw CB at b0.
// ---------------------------------------------------------------------------

static const char* k_flat_shader = R"HLSL(
cbuffer FlatCB : register(b1) {
    float4x4 gMVP;
    float4   gColor;
};
struct FI { float3 pos : POSITION; };
struct FO { float4 pos : SV_POSITION; };
FO FlatVS(FI i) {
    FO o;
    o.pos = mul(float4(i.pos, 1.0f), gMVP);
    return o;
}
float4 FlatPS(FO i) : SV_Target {
    return gColor;
}
)HLSL";

struct FlatCB {
    XMMATRIX mvp;    // 64 bytes (transposed before upload)
    XMFLOAT4 color;  // 16 bytes
    // total: 80 bytes (multiple of 16)
};

// ---------------------------------------------------------------------------
// Per-draw constant buffer layout (must be multiple of 16 bytes)
// ---------------------------------------------------------------------------
// 64 (gVP) + 16 (depth/w/h/y) + 16 (tint) + 32 (grading/copy params)
// + 16 (upscale/curve/pad) = 144 bytes
struct PerDrawCB {
    XMMATRIX  vp;         // 64 bytes — VP for layers, full MVP for overlay/controller
    float     depth;      // 4 bytes
    float     quad_w;     // 4 bytes
    float     quad_h;     // 4 bytes
    float     quad_y;     // 4 bytes
    XMFLOAT4  tint;       // 16 bytes
    float     contrast;   // 4 bytes
    float     saturation; // 4 bytes
    float     gamma;      // 4 bytes
    float     rotate90;   // 4 bytes — 1.0 = rotate UV 90° CW
    float     roundness;  // 4 bytes — sphere/cone effect (-1..+1)
    float     copy_count; // 4 bytes — active slices for this layer
    float     copy_span;  // 4 bytes — total copy depth span in metres
    float     depthmap;   // 4 bytes — 1.0 = per-pixel depth map, 0.0 = flat copies
    float     upscale;    // 4 bytes — 1.0 = sharpened upscale, 0.0 = nearest-neighbor
    float     screen_curve; // 4 bytes — curved screen amount
    float     pad0;       // 4 bytes
    float     pad1;       // 4 bytes
};

static std::vector<uint32_t> make_controller_icon(bool right_hand) {
    constexpr int w = 64;
    constexpr int h = 64;
    std::vector<uint32_t> img(w * h, 0x00000000u);

    auto plot = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < w && y >= 0 && y < h) img[y * w + x] = c;
    };
    auto fill_ellipse = [&](float cx, float cy, float rx, float ry, uint32_t c) {
        int min_x = (int)std::floor(cx - rx), max_x = (int)std::ceil(cx + rx);
        int min_y = (int)std::floor(cy - ry), max_y = (int)std::ceil(cy + ry);
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                float dx = (x + 0.5f - cx) / rx;
                float dy = (y + 0.5f - cy) / ry;
                if (dx * dx + dy * dy <= 1.0f) plot(x, y, c);
            }
        }
    };
    auto fill_rect = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                plot(x, y, c);
    };

    const uint32_t shell = right_hand ? 0xFF456ED9u : 0xFFD96E45u;
    const uint32_t accent = 0xFFF4F4F4u;
    const uint32_t dark = 0xFF202020u;

    float palm_cx = right_hand ? 38.0f : 26.0f;
    float ring_cx = right_hand ? 29.0f : 35.0f;
    fill_ellipse(palm_cx, 35.0f, 12.0f, 18.0f, shell);
    fill_ellipse(ring_cx, 18.0f, 13.0f, 13.0f, shell);
    fill_rect((int)(palm_cx - 7), 18, (int)(palm_cx + 1), 47, shell);
    fill_rect((int)(palm_cx - 2), 10, (int)(palm_cx + 3), 20, accent);
    fill_ellipse(palm_cx, 31.0f, 5.0f, 5.0f, dark);
    fill_ellipse(palm_cx + (right_hand ? 4.0f : -4.0f), 24.0f, 2.5f, 2.5f, accent);
    fill_ellipse(palm_cx + (right_hand ? -3.0f : 3.0f), 22.0f, 2.0f, 2.0f, accent);
    fill_rect((int)(palm_cx - 2), 40, (int)(palm_cx + 5), 49, dark);

    return img;
}

// ---------------------------------------------------------------------------
// Embedded 8x8 bitmap font, ASCII 32-127
// One byte per row (8 rows), bit7 = leftmost pixel.
// Public-domain PC BIOS / VGA style.
// ---------------------------------------------------------------------------
static const uint8_t k_font[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32  ' '
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // 33  '!'
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // 34  '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 35  '#'
    {0x18,0x7E,0x03,0x7E,0x60,0x7E,0x18,0x00}, // 36  '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // 37  '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 38  '&'
    {0x18,0x18,0x08,0x00,0x00,0x00,0x00,0x00}, // 39  '''
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 40  '('
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 41  ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42  '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43  '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x0C}, // 44  ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45  '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46  '.'
    {0x40,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // 47  '/'
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, // 48  '0'
    {0x18,0x1C,0x18,0x18,0x18,0x18,0x7E,0x00}, // 49  '1'
    {0x7C,0xC6,0xC0,0x60,0x30,0x18,0xFE,0x00}, // 50  '2'
    {0x7C,0xC6,0xC0,0x78,0xC0,0xC6,0x7C,0x00}, // 51  '3'
    {0x60,0x70,0x6C,0x66,0xFE,0x60,0x60,0x00}, // 52  '4'
    {0xFE,0x06,0x7E,0xC0,0xC0,0xC6,0x7C,0x00}, // 53  '5'
    {0x78,0x0C,0x06,0x7E,0xC6,0xC6,0x7C,0x00}, // 54  '6'
    {0xFE,0xC6,0xC0,0x60,0x30,0x18,0x18,0x00}, // 55  '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // 56  '8'
    {0x7C,0xC6,0xC6,0xFC,0xC0,0x60,0x3C,0x00}, // 57  '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // 58  ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x0C}, // 59  ';'
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // 60  '<'
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, // 61  '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 62  '>'
    {0x7C,0xC6,0xC0,0x60,0x30,0x00,0x30,0x00}, // 63  '?'
    {0x7C,0xC6,0xDE,0xDE,0xDE,0x06,0x7C,0x00}, // 64  '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 65  'A'
    {0x7E,0xC6,0xC6,0x7E,0xC6,0xC6,0x7E,0x00}, // 66  'B'
    {0x7C,0xC6,0x06,0x06,0x06,0xC6,0x7C,0x00}, // 67  'C'
    {0x3E,0x66,0xC6,0xC6,0xC6,0x66,0x3E,0x00}, // 68  'D'
    {0xFE,0x06,0x06,0x7E,0x06,0x06,0xFE,0x00}, // 69  'E'
    {0xFE,0x06,0x06,0x7E,0x06,0x06,0x06,0x00}, // 70  'F'
    {0x7C,0xC6,0x06,0xF6,0xC6,0xC6,0x7C,0x00}, // 71  'G'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 72  'H'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 73  'I'
    {0x60,0x60,0x60,0x60,0x60,0x66,0x3C,0x00}, // 74  'J'
    {0xC6,0x66,0x36,0x1E,0x36,0x66,0xC6,0x00}, // 75  'K'
    {0x06,0x06,0x06,0x06,0x06,0x06,0xFE,0x00}, // 76  'L'
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 77  'M'
    {0xC6,0xCE,0xDE,0xFE,0xF6,0xE6,0xC6,0x00}, // 78  'N'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 79  'O'
    {0x7E,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x00}, // 80  'P'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0x66,0xBC,0x00}, // 81  'Q'
    {0x7E,0xC6,0xC6,0x7E,0x36,0x66,0xC6,0x00}, // 82  'R'
    {0x7C,0xC6,0x06,0x7C,0xC0,0xC6,0x7C,0x00}, // 83  'S'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 84  'T'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 85  'U'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 86  'V'
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, // 87  'W'
    {0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00}, // 88  'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 89  'Y'
    {0xFE,0xC0,0x60,0x30,0x18,0x06,0xFE,0x00}, // 90  'Z'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 91  '['
    {0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x00}, // 92  '\'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 93  ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // 94  '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95  '_'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 96  '`'
    {0x00,0x00,0x78,0xCC,0xFC,0xCC,0x76,0x00}, // 97  'a'
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3B,0x00}, // 98  'b'
    {0x00,0x00,0x7C,0xC6,0x06,0xC6,0x7C,0x00}, // 99  'c'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, // 100 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0x06,0x7C,0x00}, // 101 'e'
    {0x38,0x6C,0x0C,0x3E,0x0C,0x0C,0x1E,0x00}, // 102 'f'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0xCC,0x78}, // 103 'g'
    {0x06,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 104 'h'
    {0x18,0x00,0x1C,0x18,0x18,0x18,0x3C,0x00}, // 105 'i'
    {0x60,0x00,0x7C,0x60,0x60,0x60,0x66,0x3C}, // 106 'j'
    {0x06,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 107 'k'
    {0x1C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 108 'l'
    {0x00,0x00,0x6C,0xFE,0xD6,0xD6,0xC6,0x00}, // 109 'm'
    {0x00,0x00,0x3E,0x66,0x66,0x66,0x66,0x00}, // 110 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 111 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x06}, // 112 'p'
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0x60}, // 113 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 114 'r'
    {0x00,0x00,0x7C,0x06,0x7C,0xC0,0x7C,0x00}, // 115 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x6C,0x38,0x00}, // 116 't'
    {0x00,0x00,0x66,0x66,0x66,0x66,0xDC,0x00}, // 117 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 118 'v'
    {0x00,0x00,0xC6,0xD6,0xFE,0xEE,0x6C,0x00}, // 119 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 120 'x'
    {0x00,0x00,0x66,0x66,0x66,0x7C,0x60,0x38}, // 121 'y'
    {0x00,0x00,0xFE,0x60,0x30,0x18,0xFE,0x00}, // 122 'z'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 123 '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 124 '|'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 125 '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // 126 '~'
    {0xFF,0x81,0xBD,0xA5,0xA5,0xBD,0x81,0xFF}, // 127 DEL (block)
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void check_hr(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(msg) +
            " (HRESULT 0x" + std::to_string(hr) + ")");
    }
}

// ---------------------------------------------------------------------------
// D3D11Renderer
// ---------------------------------------------------------------------------

struct Vertex {
    float x, y, z;
    float u, v;
};

// Unit quad, centered at origin, in XY plane.
// Vertices: TL, TR, BL, BR
static const Vertex k_quad_verts[4] = {
    { -0.5f,  0.5f, 0.0f,  0.0f, 0.0f }, // TL
    {  0.5f,  0.5f, 0.0f,  1.0f, 0.0f }, // TR
    { -0.5f, -0.5f, 0.0f,  0.0f, 1.0f }, // BL
    {  0.5f, -0.5f, 0.0f,  1.0f, 1.0f }, // BR
};
static const uint16_t k_quad_indices[6] = { 0, 1, 2, 1, 3, 2 };

// 8 background colors: alternating gray / blue shades, dark→light
static constexpr uint8_t k_palette_rgb[8][3] = {
    {  0,  0,  0},  // 0 black
    {  5, 10, 40},  // 1 near-black navy
    { 40, 40, 40},  // 2 dark gray
    { 15, 40,110},  // 3 dark navy blue
    {100,100,100},  // 4 medium gray
    { 60,120,220},  // 5 medium blue
    {200,200,200},  // 6 light gray
    {150,210,250},  // 7 light blue
};

enum class AmbilightEdge {
    Top = 0,
    Bottom,
    Left,
    Right
};

static XMFLOAT4 sample_display_rgba(const LayerFrame& frame, float u, float v, bool rotate_screen) {
    if (frame.rgba.empty() || frame.width <= 0 || frame.height <= 0)
        return {0, 0, 0, 0};

    float raw_u = u;
    float raw_v = v;
    if (rotate_screen) {
        raw_u = 1.0f - v;
        raw_v = u;
    }

    raw_u = (std::max)(0.0f, (std::min)(1.0f, raw_u));
    raw_v = (std::max)(0.0f, (std::min)(1.0f, raw_v));
    int x = (std::min)(frame.width - 1, (int)(raw_u * (float)frame.width));
    int y = (std::min)(frame.height - 1, (int)(raw_v * (float)frame.height));
    size_t j = ((size_t)y * frame.width + x) * 4;
    return {
        frame.rgba[j + 2] / 255.0f,
        frame.rgba[j + 1] / 255.0f,
        frame.rgba[j + 0] / 255.0f,
        frame.rgba[j + 3] / 255.0f
    };
}

static void composite_over(XMFLOAT4& dst, const XMFLOAT4& src) {
    float inv = 1.0f - src.w;
    dst.x = src.x * src.w + dst.x * inv;
    dst.y = src.y * src.w + dst.y * inv;
    dst.z = src.z * src.w + dst.z * inv;
    dst.w = src.w + dst.w * inv;
}

static XMFLOAT4 average_edge_color(const std::vector<LayerFrame>& frames,
                                   AmbilightEdge edge,
                                   bool rotate_screen) {
    constexpr int k_along_samples = 56;
    constexpr int k_inward_samples = 6;
    constexpr float k_band_span = 0.12f;

    XMFLOAT4 sum = {0, 0, 0, 0};
    int used = 0;

    for (int i = 0; i < k_along_samples; ++i) {
        float t = ((float)i + 0.5f) / (float)k_along_samples;
        for (int b = 0; b < k_inward_samples; ++b) {
            float d = (((float)b + 0.5f) / (float)k_inward_samples) * k_band_span;
            float u = 0.5f;
            float v = 0.5f;
            switch (edge) {
            case AmbilightEdge::Top:    u = t;         v = d;         break;
            case AmbilightEdge::Bottom: u = t;         v = 1.0f - d;  break;
            case AmbilightEdge::Left:   u = d;         v = t;         break;
            case AmbilightEdge::Right:  u = 1.0f - d;  v = t;         break;
            }

            XMFLOAT4 comp = {0, 0, 0, 0};
            for (const auto& frame : frames)
                composite_over(comp, sample_display_rgba(frame, u, v, rotate_screen));

            if (comp.w > 0.02f) {
                sum.x += comp.x;
                sum.y += comp.y;
                sum.z += comp.z;
                sum.w += comp.w;
                ++used;
            }
        }
    }

    if (used <= 0)
        return {0, 0, 0, 0};

    float inv_used = 1.0f / (float)used;
    return {sum.x * inv_used, sum.y * inv_used, sum.z * inv_used, sum.w * inv_used};
}

static float curved_depth_offset(float screen_curve, float quad_w, float x_norm) {
    return screen_curve * quad_w * 0.18f * (x_norm * x_norm);
}

D3D11Renderer::D3D11Renderer(ID3D11Device* device, ID3D11DeviceContext* ctx)
    : m_device(device), m_ctx(ctx)
{
    init_pipeline();
    init_flat_pipeline();
    init_colorpal_texture();
    update_overlay("");
    update_preset_overlay("");
    update_random_overlay("");
}

void D3D11Renderer::init_colorpal_texture() {
    // 4 cols × 8 rows = 32 swatches; texture 256×768 (1:3 matches panel AR)
    const int W = 256, H = 768;
    const int CW = W / k_colorpal_cols, CH = H / k_colorpal_rows; // 64×96 per cell

    std::vector<uint8_t> pixels(W * H * 4);
    for (int row = 0; row < k_colorpal_rows; ++row) {
        for (int col = 0; col < k_colorpal_cols; ++col) {
            int ci = row * k_colorpal_cols + col;
            uint8_t r = k_palette_rgb[ci][0];
            uint8_t g = k_palette_rgb[ci][1];
            uint8_t b = k_palette_rgb[ci][2];
            int x0 = col * CW, y0 = row * CH;
            for (int y = y0; y < y0 + CH; ++y) {
                for (int x = x0; x < x0 + CW; ++x) {
                    bool border = (x == x0 || x == x0+CW-1 || y == y0 || y == y0+CH-1);
                    int p = (y * W + x) * 4;
                    // BGRA layout
                    if (border) {
                        pixels[p+0] = (uint8_t)(std::min)(255, (int)b + 50);
                        pixels[p+1] = (uint8_t)(std::min)(255, (int)g + 50);
                        pixels[p+2] = (uint8_t)(std::min)(255, (int)r + 50);
                    } else {
                        pixels[p+0] = b; pixels[p+1] = g; pixels[p+2] = r;
                    }
                    pixels[p+3] = 255;
                }
            }
        }
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = { pixels.data(), (UINT)(W * 4), 0 };
    m_device->CreateTexture2D(&td, &srd, m_colorpal_tex.GetAddressOf());
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_colorpal_tex.Get(), &svd, m_colorpal_srv.GetAddressOf());
}

XMFLOAT4 D3D11Renderer::get_palette_color(int idx) const {
    if (idx < 0 || idx >= 8) return {0.02f, 0.02f, 0.05f, 1.0f};
    return { k_palette_rgb[idx][0] / 255.0f,
             k_palette_rgb[idx][1] / 255.0f,
             k_palette_rgb[idx][2] / 255.0f, 1.0f };
}

void D3D11Renderer::init_pipeline() {
    HRESULT hr;

    // --- Compile shaders ---
    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    hr = D3DCompile(k_shader_src, strlen(k_shader_src), "retrodepth",
                    nullptr, nullptr, "VSMain", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    vs_blob.GetAddressOf(), err_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::string msg = err_blob ? (char*)err_blob->GetBufferPointer() : "VS compile failed";
        throw std::runtime_error(msg);
    }

    hr = D3DCompile(k_shader_src, strlen(k_shader_src), "retrodepth",
                    nullptr, nullptr, "PSMain", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    ps_blob.GetAddressOf(), err_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::string msg = err_blob ? (char*)err_blob->GetBufferPointer() : "PS compile failed";
        throw std::runtime_error(msg);
    }

    check_hr(m_device->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        nullptr, m_vs.GetAddressOf()), "CreateVertexShader");

    check_hr(m_device->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
        nullptr, m_ps.GetAddressOf()), "CreatePixelShader");

    // --- Input layout ---
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    check_hr(m_device->CreateInputLayout(layout, 2,
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        m_input_layout.GetAddressOf()), "CreateInputLayout");

    // --- Vertex buffer ---
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth      = sizeof(k_quad_verts);
    vbd.Usage          = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vdata = { k_quad_verts };
    check_hr(m_device->CreateBuffer(&vbd, &vdata, m_vb.GetAddressOf()), "VB");

    // --- Index buffer ---
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth      = sizeof(k_quad_indices);
    ibd.Usage          = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags      = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA idata = { k_quad_indices };
    check_hr(m_device->CreateBuffer(&ibd, &idata, m_ib.GetAddressOf()), "IB");

    std::vector<Vertex> curve_verts;
    curve_verts.reserve((k_curve_segments + 1) * 2);
    for (UINT i = 0; i <= k_curve_segments; ++i) {
        float u = (float)i / (float)k_curve_segments;
        float x = u - 0.5f;
        curve_verts.push_back({ x,  0.5f, 0.0f, u, 0.0f });
        curve_verts.push_back({ x, -0.5f, 0.0f, u, 1.0f });
    }

    std::vector<uint16_t> curve_indices;
    curve_indices.reserve((size_t)k_curve_segments * 6);
    for (UINT i = 0; i < k_curve_segments; ++i) {
        uint16_t tl = (uint16_t)(i * 2 + 0);
        uint16_t bl = (uint16_t)(i * 2 + 1);
        uint16_t tr = (uint16_t)(i * 2 + 2);
        uint16_t br = (uint16_t)(i * 2 + 3);
        curve_indices.push_back(tl);
        curve_indices.push_back(tr);
        curve_indices.push_back(bl);
        curve_indices.push_back(tr);
        curve_indices.push_back(br);
        curve_indices.push_back(bl);
    }
    m_curve_index_count = (UINT)curve_indices.size();

    D3D11_BUFFER_DESC cvbd = {};
    cvbd.ByteWidth      = (UINT)(curve_verts.size() * sizeof(Vertex));
    cvbd.Usage          = D3D11_USAGE_IMMUTABLE;
    cvbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA cvdata = { curve_verts.data() };
    check_hr(m_device->CreateBuffer(&cvbd, &cvdata, m_curve_vb.GetAddressOf()), "CurveVB");

    D3D11_BUFFER_DESC cibd = {};
    cibd.ByteWidth      = (UINT)(curve_indices.size() * sizeof(uint16_t));
    cibd.Usage          = D3D11_USAGE_IMMUTABLE;
    cibd.BindFlags      = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA cidata = { curve_indices.data() };
    check_hr(m_device->CreateBuffer(&cibd, &cidata, m_curve_ib.GetAddressOf()), "CurveIB");

    // --- Constant buffer: PerDrawCB = 160 bytes ---
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(PerDrawCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateBuffer(&cbd, nullptr, m_cb.GetAddressOf()), "CB");

    // --- Samplers: point for the original pixel-art look, linear for sharpened upscale taps ---
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    check_hr(m_device->CreateSamplerState(&sd, m_sampler.GetAddressOf()), "Sampler");

    D3D11_SAMPLER_DESC sdl = sd;
    sdl.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    check_hr(m_device->CreateSamplerState(&sdl, m_sampler_linear.GetAddressOf()), "LinearSampler");

    // --- Alpha blend state ---
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check_hr(m_device->CreateBlendState(&bd, m_blend.GetAddressOf()), "BlendState");

    // --- Rasterizer ---
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // show both sides of quads
    check_hr(m_device->CreateRasterizerState(&rd, m_raster.GetAddressOf()), "RasterizerState");

    // --- Depth stencil: write depth so quads occlude each other correctly ---
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    check_hr(m_device->CreateDepthStencilState(&dsd, m_depth_state.GetAddressOf()), "DepthState");

    // Slice depth state: always pass, no depth write (slices must never block each other)
    D3D11_DEPTH_STENCIL_DESC sdsd = {};
    sdsd.DepthEnable    = TRUE;
    sdsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    sdsd.DepthFunc      = D3D11_COMPARISON_ALWAYS;
    check_hr(m_device->CreateDepthStencilState(&sdsd, m_depth_state_slice.GetAddressOf()), "DepthStateSlice");

    // --- Overlay texture (512x768 BGRA, dynamic, CPU-written) ---
    D3D11_TEXTURE2D_DESC otd = {};
    otd.Width            = k_overlay_w;
    otd.Height           = k_overlay_h;
    otd.MipLevels        = 1;
    otd.ArraySize        = 1;
    otd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    otd.SampleDesc.Count = 1;
    otd.Usage            = D3D11_USAGE_DYNAMIC;
    otd.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    otd.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateTexture2D(&otd, nullptr, m_overlay_tex.GetAddressOf()), "OverlayTex");

    D3D11_SHADER_RESOURCE_VIEW_DESC osrvd = {};
    osrvd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    osrvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    osrvd.Texture2D.MipLevels = 1;
    check_hr(m_device->CreateShaderResourceView(m_overlay_tex.Get(), &osrvd,
             m_overlay_srv.GetAddressOf()), "OverlaySRV");

    D3D11_TEXTURE2D_DESC ptd = otd;
    ptd.Width  = k_preset_overlay_w;
    ptd.Height = k_preset_overlay_h;
    check_hr(m_device->CreateTexture2D(&ptd, nullptr, m_preset_overlay_tex.GetAddressOf()), "PresetOverlayTex");

    D3D11_SHADER_RESOURCE_VIEW_DESC psrvd = {};
    psrvd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    psrvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    psrvd.Texture2D.MipLevels = 1;
    check_hr(m_device->CreateShaderResourceView(m_preset_overlay_tex.Get(), &psrvd,
             m_preset_overlay_srv.GetAddressOf()), "PresetOverlaySRV");

    D3D11_TEXTURE2D_DESC rtd = otd;
    rtd.Width  = k_random_overlay_w;
    rtd.Height = k_random_overlay_h;
    check_hr(m_device->CreateTexture2D(&rtd, nullptr, m_random_overlay_tex.GetAddressOf()), "RandomOverlayTex");

    D3D11_SHADER_RESOURCE_VIEW_DESC rsrvd = {};
    rsrvd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    rsrvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    rsrvd.Texture2D.MipLevels = 1;
    check_hr(m_device->CreateShaderResourceView(m_random_overlay_tex.Get(), &rsrvd,
             m_random_overlay_srv.GetAddressOf()), "RandomOverlaySRV");

    // --- Procedural left/right controller icon textures ---
    D3D11_TEXTURE2D_DESC wtd = {};
    wtd.Width = wtd.Height = 64;
    wtd.MipLevels        = 1;
    wtd.ArraySize        = 1;
    wtd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    wtd.SampleDesc.Count = 1;
    wtd.Usage            = D3D11_USAGE_IMMUTABLE;
    wtd.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC wsrvd = {};
    wsrvd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    wsrvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    wsrvd.Texture2D.MipLevels = 1;
    for (int hand = 0; hand < 2; ++hand) {
        auto pixels = make_controller_icon(hand == 1);
        D3D11_SUBRESOURCE_DATA wtd_data = { pixels.data(), 64 * 4, 0 };
        check_hr(m_device->CreateTexture2D(&wtd, &wtd_data, m_ctrl_tex[hand].GetAddressOf()), "CtrlTex");
        check_hr(m_device->CreateShaderResourceView(m_ctrl_tex[hand].Get(), &wsrvd,
                 m_ctrl_srv[hand].GetAddressOf()), "CtrlSRV");
    }

    // --- Dynamic 4-vertex buffer for box face geometry (re-uploaded per face per layer) ---
    D3D11_BUFFER_DESC fvbd = {};
    fvbd.ByteWidth      = 4 * sizeof(Vertex);
    fvbd.Usage          = D3D11_USAGE_DYNAMIC;
    fvbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    fvbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateBuffer(&fvbd, nullptr, m_face_vb.GetAddressOf()), "FaceVB");
}

void D3D11Renderer::init_flat_pipeline() {
    HRESULT hr;
    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;

    hr = D3DCompile(k_flat_shader, strlen(k_flat_shader), "flat",
                    nullptr, nullptr, "FlatVS", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    vs_blob.GetAddressOf(), err_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::string msg = err_blob ? (char*)err_blob->GetBufferPointer() : "FlatVS compile failed";
        throw std::runtime_error(msg);
    }

    hr = D3DCompile(k_flat_shader, strlen(k_flat_shader), "flat",
                    nullptr, nullptr, "FlatPS", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    ps_blob.GetAddressOf(), err_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::string msg = err_blob ? (char*)err_blob->GetBufferPointer() : "FlatPS compile failed";
        throw std::runtime_error(msg);
    }

    check_hr(m_device->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        nullptr, m_flat_vs.GetAddressOf()), "CreateFlatVS");

    check_hr(m_device->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
        nullptr, m_flat_ps.GetAddressOf()), "CreateFlatPS");

    // Input layout: position only (no UV)
    D3D11_INPUT_ELEMENT_DESC flat_layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    check_hr(m_device->CreateInputLayout(flat_layout, 1,
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        m_flat_layout.GetAddressOf()), "CreateFlatInputLayout");

    // Dynamic vertex buffer for flat-color helpers (lasers, reticles, shadows)
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth      = k_flat_vb_capacity * sizeof(XMFLOAT3);
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateBuffer(&vbd, nullptr, m_flat_vb.GetAddressOf()), "FlatVB");

    // Constant buffer for FlatCB (registered at b1)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(FlatCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateBuffer(&cbd, nullptr, m_flat_cb.GetAddressOf()), "FlatCB");
}

void D3D11Renderer::draw_extruded_layer_walls(const std::vector<LayerFrame>& frames, const EyeParams& eye) {
    if (!m_3d_layers_enabled || frames.empty())
        return;

    m_ctx->IASetInputLayout(m_flat_layout.Get());
    m_ctx->VSSetShader(m_flat_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_flat_ps.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT flat_stride = sizeof(XMFLOAT3), flat_offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_flat_vb.GetAddressOf(), &flat_stride, &flat_offset);
    m_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);

    XMMATRIX vp = XMMatrixMultiply(eye.view, eye.proj);

    auto upload_flat = [&](const XMFLOAT4& color) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb  = reinterpret_cast<FlatCB*>(mapped.pData);
            cb->mvp   = XMMatrixTranspose(vp);
            cb->color = color;
            m_ctx->Unmap(m_flat_cb.Get(), 0);
        }
    };

    auto upload_verts = [&](const XMFLOAT3* verts, int count) {
        if (!verts || count <= 0)
            return;
        if (count > k_flat_vb_capacity)
            count = k_flat_vb_capacity;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, verts, count * sizeof(XMFLOAT3));
            m_ctx->Unmap(m_flat_vb.Get(), 0);
        }
        m_ctx->Draw(count, 0);
    };

    auto stage_to_scene = [&](float x_norm, float y_norm, float qw, float qh, float depth) {
        float curve_x = x_norm * 2.0f - 1.0f;
        XMVECTOR p = XMVectorSet((x_norm - 0.5f) * qw,
                                 eye.quad_y_meters + (0.5f - y_norm) * qh,
                                 -(depth + curved_depth_offset(m_screen_curve, qw, curve_x)),
                                 1.0f);
        p = XMVector3TransformCoord(p, eye.scene_world);
        XMFLOAT3 out;
        XMStoreFloat3(&out, p);
        return out;
    };

    auto emit_horizontal = [&](float x0_norm, float x1_norm, float y_norm,
                               float qw, float qh, float front_depth, float back_depth) {
        XMFLOAT3 v[6] = {
            stage_to_scene(x0_norm, y_norm, qw, qh, back_depth),
            stage_to_scene(x1_norm, y_norm, qw, qh, back_depth),
            stage_to_scene(x1_norm, y_norm, qw, qh, front_depth),
            stage_to_scene(x0_norm, y_norm, qw, qh, back_depth),
            stage_to_scene(x1_norm, y_norm, qw, qh, front_depth),
            stage_to_scene(x0_norm, y_norm, qw, qh, front_depth),
        };
        upload_verts(v, 6);
    };

    auto emit_vertical = [&](float x_norm, float y0_norm, float y1_norm,
                             float qw, float qh, float front_depth, float back_depth) {
        XMFLOAT3 v[6] = {
            stage_to_scene(x_norm, y0_norm, qw, qh, back_depth),
            stage_to_scene(x_norm, y1_norm, qw, qh, back_depth),
            stage_to_scene(x_norm, y1_norm, qw, qh, front_depth),
            stage_to_scene(x_norm, y0_norm, qw, qh, back_depth),
            stage_to_scene(x_norm, y1_norm, qw, qh, front_depth),
            stage_to_scene(x_norm, y0_norm, qw, qh, front_depth),
        };
        upload_verts(v, 6);
    };

    m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);

    for (const auto& f : frames) {
        if (f.rgba.empty() || f.width <= 0 || f.height <= 0)
            continue;

        int copy_count = copy_count_for_frame(f);
        float copy_span = copy_span_for_frame(f, copy_count);
        if (copy_span <= 1e-5f)
            continue;

        float back_depth  = f.depth_meters;
        float front_depth = (std::max)(0.01f, back_depth - copy_span);
        if (back_depth - front_depth <= 1e-5f)
            continue;

        float aspect = (float)f.width / (float)f.height;
        float qw = f.quad_width_meters;
        float qh = m_rotate_screen ? (qw * aspect) : (qw / aspect);

        auto opaque = [&](int x, int y) -> bool {
            if (x < 0 || x >= f.width || y < 0 || y >= f.height)
                return false;
            return f.rgba[((size_t)y * f.width + x) * 4 + 3] > 8;
        };

        // Subtle dark walls: enough to make the volume read, without overpowering the sprite.
        upload_flat({0.08f * m_brightness, 0.08f * m_brightness, 0.10f * m_brightness, 0.92f});

        // Top and bottom boundary runs.
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ) {
                if (opaque(x, y) && !opaque(x, y - 1)) {
                    int x0 = x;
                    while (x + 1 < f.width && opaque(x + 1, y) && !opaque(x + 1, y - 1))
                        ++x;
                    emit_horizontal((float)x0 / (float)f.width,
                                    (float)(x + 1) / (float)f.width,
                                    (float)y / (float)f.height,
                                    qw, qh, front_depth, back_depth);
                }
                ++x;
            }
            for (int x = 0; x < f.width; ) {
                if (opaque(x, y) && !opaque(x, y + 1)) {
                    int x0 = x;
                    while (x + 1 < f.width && opaque(x + 1, y) && !opaque(x + 1, y + 1))
                        ++x;
                    emit_horizontal((float)x0 / (float)f.width,
                                    (float)(x + 1) / (float)f.width,
                                    (float)(y + 1) / (float)f.height,
                                    qw, qh, front_depth, back_depth);
                }
                ++x;
            }
        }

        // Left and right boundary runs.
        for (int x = 0; x < f.width; ++x) {
            for (int y = 0; y < f.height; ) {
                if (opaque(x, y) && !opaque(x - 1, y)) {
                    int y0 = y;
                    while (y + 1 < f.height && opaque(x, y + 1) && !opaque(x - 1, y + 1))
                        ++y;
                    emit_vertical((float)x / (float)f.width,
                                  (float)y0 / (float)f.height,
                                  (float)(y + 1) / (float)f.height,
                                  qw, qh, front_depth, back_depth);
                }
                ++y;
            }
            for (int y = 0; y < f.height; ) {
                if (opaque(x, y) && !opaque(x + 1, y)) {
                    int y0 = y;
                    while (y + 1 < f.height && opaque(x, y + 1) && !opaque(x + 1, y + 1))
                        ++y;
                    emit_vertical((float)(x + 1) / (float)f.width,
                                  (float)y0 / (float)f.height,
                                  (float)(y + 1) / (float)f.height,
                                  qw, qh, front_depth, back_depth);
                }
                ++y;
            }
        }
    }
}

void D3D11Renderer::draw_floor_shadows(const std::vector<LayerFrame>& frames, const EyeParams& eye) {
    if (!m_shadows_enabled || frames.empty())
        return;

    m_ctx->IASetInputLayout(m_flat_layout.Get());
    m_ctx->VSSetShader(m_flat_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_flat_ps.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT flat_stride = sizeof(XMFLOAT3), flat_offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_flat_vb.GetAddressOf(), &flat_stride, &flat_offset);
    m_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);

    XMMATRIX vp = XMMatrixMultiply(eye.view, eye.proj);

    auto upload_flat = [&](const XMFLOAT4& color) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb  = reinterpret_cast<FlatCB*>(mapped.pData);
            cb->mvp   = XMMatrixTranspose(vp);
            cb->color = color;
            m_ctx->Unmap(m_flat_cb.Get(), 0);
        }
    };

    auto upload_verts = [&](const XMFLOAT3* verts, int count) {
        if (!verts || count <= 0)
            return;
        if (count > k_flat_vb_capacity)
            count = k_flat_vb_capacity;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, verts, count * sizeof(XMFLOAT3));
            m_ctx->Unmap(m_flat_vb.Get(), 0);
        }
        m_ctx->Draw(count, 0);
    };

    constexpr int k_segments = 16;
    XMFLOAT3 verts[k_segments * 3];
    m_ctx->OMSetDepthStencilState(m_depth_state_slice.Get(), 0);

    for (const auto& f : frames) {
        if (f.rgba.empty() || f.width <= 0 || f.height <= 0)
            continue;

        const int pixel_count = f.width * f.height;
        int opaque_pixels = 0;
        int min_x = f.width, min_y = f.height, max_x = -1, max_y = -1;
        float left_luma = 0.0f;
        float right_luma = 0.0f;

        const uint8_t* px = f.rgba.data();
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                const size_t j = ((size_t)y * f.width + x) * 4;
                float a = px[j + 3] / 255.0f;
                if (a <= 0.05f)
                    continue;

                ++opaque_pixels;
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;

                float b = px[j + 0] / 255.0f;
                float g = px[j + 1] / 255.0f;
                float r = px[j + 2] / 255.0f;
                float luma = (0.0722f * b + 0.7152f * g + 0.2126f * r) * a;
                if (x < f.width / 2) left_luma += luma;
                else                 right_luma += luma;
            }
        }

        if (opaque_pixels < 8 || max_x < min_x || max_y < min_y)
            continue;

        float coverage = (float)opaque_pixels / (float)pixel_count;
        if (coverage > 0.78f || coverage < 0.01f)
            continue;

        float aspect = (float)f.width / (float)f.height;
        float qw = f.quad_width_meters;
        float qh = m_rotate_screen ? (qw * aspect) : (qw / aspect);

        float bbox_min_u = (float)min_x / (float)f.width;
        float bbox_max_u = (float)(max_x + 1) / (float)f.width;
        float bbox_max_v = (float)(max_y + 1) / (float)f.height;
        float bbox_w = bbox_max_u - bbox_min_u;
        float bbox_h = (float)(max_y - min_y + 1) / (float)f.height;

        float rx = (std::max)(0.05f, qw * bbox_w * 0.32f);
        float rz = (std::max)(0.035f, qh * bbox_h * 0.09f + rx * 0.18f);
        float total_luma = left_luma + right_luma;
        float bias = total_luma > 1e-4f ? (left_luma - right_luma) / total_luma : 0.0f;
        float center_x = (((bbox_min_u + bbox_max_u) * 0.5f) - 0.5f) * qw + bias * rx * 0.55f;
        float center_y = eye.quad_y_meters + (0.5f - bbox_max_v) * qh - qh * 0.045f;
        float center_z = -f.depth_meters - rz * 0.35f;
        float alpha = (std::min)(0.24f, 0.08f + coverage * 0.22f + std::fabs(bias) * 0.08f);

        upload_flat({0.0f, 0.0f, 0.0f, alpha});

        for (int seg = 0; seg < k_segments; ++seg) {
            float a0 = ((float)seg / (float)k_segments) * XM_2PI;
            float a1 = ((float)(seg + 1) / (float)k_segments) * XM_2PI;

            XMVECTOR p0 = XMVectorSet(center_x, center_y, center_z, 1.0f);
            XMVECTOR p1 = XMVectorSet(center_x + std::cos(a0) * rx, center_y, center_z + std::sin(a0) * rz, 1.0f);
            XMVECTOR p2 = XMVectorSet(center_x + std::cos(a1) * rx, center_y, center_z + std::sin(a1) * rz, 1.0f);

            p0 = XMVector3TransformCoord(p0, eye.scene_world);
            p1 = XMVector3TransformCoord(p1, eye.scene_world);
            p2 = XMVector3TransformCoord(p2, eye.scene_world);

            XMStoreFloat3(&verts[seg * 3 + 0], p0);
            XMStoreFloat3(&verts[seg * 3 + 1], p1);
            XMStoreFloat3(&verts[seg * 3 + 2], p2);
        }

        upload_verts(verts, k_segments * 3);
    }
}

void D3D11Renderer::draw_ambilight(const std::vector<LayerFrame>& frames, const EyeParams& eye) {
    if (!m_ambilight_enabled || frames.empty())
        return;

    m_ctx->IASetInputLayout(m_flat_layout.Get());
    m_ctx->VSSetShader(m_flat_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_flat_ps.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT flat_stride = sizeof(XMFLOAT3), flat_offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_flat_vb.GetAddressOf(), &flat_stride, &flat_offset);
    m_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);

    XMMATRIX vp = XMMatrixMultiply(eye.view, eye.proj);

    auto upload_flat = [&](const XMFLOAT4& color) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb  = reinterpret_cast<FlatCB*>(mapped.pData);
            cb->mvp   = XMMatrixTranspose(vp);
            cb->color = color;
            m_ctx->Unmap(m_flat_cb.Get(), 0);
        }
    };

    auto upload_verts = [&](const XMFLOAT3* verts, int count) {
        if (!verts || count <= 0)
            return;
        if (count > k_flat_vb_capacity)
            count = k_flat_vb_capacity;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, verts, count * sizeof(XMFLOAT3));
            m_ctx->Unmap(m_flat_vb.Get(), 0);
        }
        m_ctx->Draw(count, 0);
    };

    auto stage_to_scene = [&](float x, float y, float depth) {
        XMVECTOR p = XMVectorSet(x, y, -depth, 1.0f);
        p = XMVector3TransformCoord(p, eye.scene_world);
        XMFLOAT3 out;
        XMStoreFloat3(&out, p);
        return out;
    };

    auto emit_rect = [&](float cx, float cy, float depth, float w, float h, const XMFLOAT4& color) {
        if (color.w <= 0.001f || w <= 0.001f || h <= 0.001f)
            return;
        XMFLOAT3 verts[6] = {
            stage_to_scene(cx - w * 0.5f, cy + h * 0.5f, depth),
            stage_to_scene(cx + w * 0.5f, cy + h * 0.5f, depth),
            stage_to_scene(cx + w * 0.5f, cy - h * 0.5f, depth),
            stage_to_scene(cx - w * 0.5f, cy + h * 0.5f, depth),
            stage_to_scene(cx + w * 0.5f, cy - h * 0.5f, depth),
            stage_to_scene(cx - w * 0.5f, cy - h * 0.5f, depth),
        };
        upload_flat(color);
        upload_verts(verts, 6);
    };

    float screen_w = 0.0f;
    float screen_h = 0.0f;
    float back_depth = 0.0f;
    for (const auto& frame : frames) {
        if (frame.rgba.empty() || frame.width <= 0 || frame.height <= 0)
            continue;
        float aspect = (float)frame.width / (float)frame.height;
        float qw = frame.quad_width_meters;
        float qh = m_rotate_screen ? (qw * aspect) : (qw / aspect);
        screen_w = (std::max)(screen_w, qw);
        screen_h = (std::max)(screen_h, qh);

        float depth = frame.depth_meters;
        if (m_depthmap_enabled && m_depthmap_mirror_enabled)
            depth += copy_span_for_frame(frame, copy_count_for_frame(frame));
        back_depth = (std::max)(back_depth, depth);
    }

    if (screen_w <= 0.001f || screen_h <= 0.001f)
        return;

    XMFLOAT4 edge_color[4] = {
        average_edge_color(frames, AmbilightEdge::Top, m_rotate_screen),
        average_edge_color(frames, AmbilightEdge::Bottom, m_rotate_screen),
        average_edge_color(frames, AmbilightEdge::Left, m_rotate_screen),
        average_edge_color(frames, AmbilightEdge::Right, m_rotate_screen)
    };

    float total_alpha = 0.0f;
    for (const auto& c : edge_color)
        total_alpha += c.w;
    if (total_alpha <= 0.02f)
        return;

    static const float k_shell_alpha[3] = { 0.12f, 0.065f, 0.035f };
    m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);

    float halo_depth = back_depth + 0.03f;
    for (int shell = 0; shell < 3; ++shell) {
        float grow = (float)shell;
        float top_h = screen_h * (0.24f + 0.14f * grow);
        float top_w = screen_w * (1.08f + 0.14f * grow);
        float side_w = screen_w * (0.16f + 0.10f * grow);
        float side_h = screen_h * (1.04f + 0.10f * grow);
        float top_y = eye.quad_y_meters + screen_h * (0.56f + 0.10f * grow);
        float bottom_y = eye.quad_y_meters - screen_h * (0.56f + 0.10f * grow);
        float left_x = -screen_w * (0.56f + 0.09f * grow);
        float right_x = screen_w * (0.56f + 0.09f * grow);

        auto tint = [&](const XMFLOAT4& src) {
            float alpha = src.w * k_shell_alpha[shell];
            return XMFLOAT4{
                (std::min)(1.0f, src.x * 1.10f),
                (std::min)(1.0f, src.y * 1.10f),
                (std::min)(1.0f, src.z * 1.10f),
                alpha
            };
        };

        emit_rect(0.0f, top_y, halo_depth, top_w, top_h, tint(edge_color[0]));
        emit_rect(0.0f, bottom_y, halo_depth, top_w, top_h, tint(edge_color[1]));
        emit_rect(left_x, eye.quad_y_meters, halo_depth, side_w, side_h, tint(edge_color[2]));
        emit_rect(right_x, eye.quad_y_meters, halo_depth, side_w, side_h, tint(edge_color[3]));
    }
}

void D3D11Renderer::draw_laser_beams(const EyeParams& eye) {
    // Set up flat pipeline
    m_ctx->IASetInputLayout(m_flat_layout.Get());
    m_ctx->VSSetShader(m_flat_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_flat_ps.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(1, 1, m_flat_cb.GetAddressOf());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT flat_stride = sizeof(XMFLOAT3), flat_offset = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_flat_vb.GetAddressOf(), &flat_stride, &flat_offset);

    // No index buffer for flat draws — use DrawAuto style triangle list
    m_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);

    XMMATRIX vp = XMMatrixMultiply(eye.view, eye.proj);

    auto upload_flat = [&](const XMFLOAT4& color) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb  = reinterpret_cast<FlatCB*>(mapped.pData);
            cb->mvp   = XMMatrixTranspose(vp); // identity world; verts already in stage space
            cb->color = color;
            m_ctx->Unmap(m_flat_cb.Get(), 0);
        }
    };

    auto upload_verts = [&](const XMFLOAT3* verts, int count) {
        if (!verts || count <= 0)
            return;
        if (count > k_flat_vb_capacity)
            count = k_flat_vb_capacity;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_flat_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, verts, count * sizeof(XMFLOAT3));
            m_ctx->Unmap(m_flat_vb.Get(), 0);
        }
        m_ctx->Draw(count, 0);
    };

    // Laser colors: left = orange, right = cyan; brighter when on screen
    static const XMFLOAT4 k_color_base[2] = {
        {1.0f, 0.4f, 0.1f, 0.85f},  // left: orange
        {0.2f, 0.8f, 1.0f, 0.85f},  // right: cyan
    };
    static const XMFLOAT4 k_color_hot[2] = {
        {1.0f, 0.6f, 0.2f, 0.95f},  // left: brighter orange
        {0.4f, 1.0f, 1.0f, 0.95f},  // right: brighter cyan
    };

    const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    for (int h = 0; h < 2; ++h) {
        if (!m_laser_on_screen[h] && !m_laser_on_panel[h])
            continue;
        XMVECTOR origin = XMLoadFloat3(&m_laser_origin[h]);
        XMVECTOR tip    = XMLoadFloat3(&m_laser_tip[h]);
        XMVECTOR dir    = XMVector3Normalize(XMVectorSubtract(tip, origin));

        // Half-width for ribbon (6 mm)
        // If laser is nearly vertical, use world-X as the billboard axis instead
        const float hw = 0.003f;
        XMVECTOR cross = XMVector3Cross(dir, up);
        if (XMVectorGetX(XMVector3LengthSq(cross)) < 1e-6f)
            cross = XMVectorSet(1.f, 0.f, 0.f, 0.f);
        XMVECTOR right = XMVector3Normalize(cross);
        XMVECTOR rv    = XMVectorScale(right, hw);

        // ribbon: v0=origin-r, v1=origin+r, v2=tip-r, v3=tip+r
        // triangles: (0,1,2), (1,3,2)
        XMFLOAT3 v0, v1, v2, v3;
        XMStoreFloat3(&v0, XMVectorSubtract(origin, rv));
        XMStoreFloat3(&v1, XMVectorAdd(origin, rv));
        XMStoreFloat3(&v2, XMVectorSubtract(tip, rv));
        XMStoreFloat3(&v3, XMVectorAdd(tip, rv));

        bool active = m_laser_on_screen[h] || m_laser_on_panel[h];
        XMFLOAT4 col = active ? k_color_hot[h] : k_color_base[h];

        upload_flat(col);

        XMFLOAT3 ribbon[6] = { v0, v1, v2, v1, v3, v2 };
        upload_verts(ribbon, 6);

        // Reticle: small diamond at the tip (4 triangles from center)
        if (active) {
            const float rs = 0.012f; // reticle half-size
            XMVECTOR cx = XMVectorScale(right, rs);
            XMVECTOR cy = XMVectorScale(up,    rs);
            XMFLOAT3 tc, tr, tt, tl, tb;
            XMStoreFloat3(&tc, tip);
            XMStoreFloat3(&tr, XMVectorAdd(tip, cx));
            XMStoreFloat3(&tt, XMVectorAdd(tip, cy));
            XMStoreFloat3(&tl, XMVectorSubtract(tip, cx));
            XMStoreFloat3(&tb, XMVectorSubtract(tip, cy));

            // Diamond = 4 triangles
            XMFLOAT3 diamond[12] = {
                tc, tr, tt,
                tc, tt, tl,
                tc, tl, tb,
                tc, tb, tr,
            };
            upload_verts(diamond, 12);
        }
    }
}

void D3D11Renderer::resize_layers(int n) {
    m_layers.resize(n);
}

void D3D11Renderer::ensure_hover_texture(int w, int h) {
    if (m_hover_tex && m_hover_mask_width == w && m_hover_mask_height == h)
        return;

    m_hover_srv.Reset();
    m_hover_tex.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateTexture2D(&td, nullptr, m_hover_tex.GetAddressOf()), "HoverTex");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    check_hr(m_device->CreateShaderResourceView(m_hover_tex.Get(), &srvd,
             m_hover_srv.GetAddressOf()), "HoverSRV");

    m_hover_mask_width = w;
    m_hover_mask_height = h;
}

void D3D11Renderer::update_hover_mask(int layer_idx, int width, int height, const uint8_t* alpha_mask) {
    if (layer_idx < 0 || width <= 0 || height <= 0 || !alpha_mask) {
        clear_hover_mask();
        return;
    }

    ensure_hover_texture(width, height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_ctx->Map(m_hover_tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;

    const uint8_t cb = 0xFF;
    const uint8_t cg = 0xFF;
    const uint8_t cr = 0xFF;
    uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = dst + y * mapped.RowPitch;
        for (int x = 0; x < width; ++x) {
            uint8_t a = alpha_mask[y * width + x];
            row[x * 4 + 0] = cb;
            row[x * 4 + 1] = cg;
            row[x * 4 + 2] = cr;
            row[x * 4 + 3] = a;
        }
    }
    m_ctx->Unmap(m_hover_tex.Get(), 0);

    m_hover_mask_layer = layer_idx;
}

void D3D11Renderer::clear_hover_mask() {
    m_hover_mask_layer = -1;
}

void D3D11Renderer::ensure_layer_texture(int idx, int w, int h) {
    auto& lt = m_layers[idx];
    if (lt.tex && lt.width == w && lt.height == h) return;

    lt.tex.Reset();
    lt.srv.Reset();
    lt.width  = w;
    lt.height = h;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    check_hr(m_device->CreateTexture2D(&td, nullptr, lt.tex.GetAddressOf()), "LayerTex");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format              = td.Format;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    check_hr(m_device->CreateShaderResourceView(lt.tex.Get(), &srvd,
             lt.srv.GetAddressOf()), "LayerSRV");
}

void D3D11Renderer::update_layer(int idx, const LayerFrame& frame) {
    ensure_layer_texture(idx, frame.width, frame.height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hr(m_ctx->Map(m_layers[idx].tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
             "Map layer texture");
    const uint8_t* src = frame.rgba.data();
    uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < frame.height; ++y) {
        memcpy(dst + y * mapped.RowPitch,
               src + y * frame.width * 4,
               frame.width * 4);
    }
    m_ctx->Unmap(m_layers[idx].tex.Get(), 0);
}

void D3D11Renderer::set_editor_state(bool active, int selected_layer, bool blink_on) {
    m_editor_active   = active;
    m_editor_selected = selected_layer;
    m_editor_blink_on = blink_on;
}

void D3D11Renderer::update_text_texture(ID3D11Texture2D* tex, int tex_w, int tex_h,
                                        const std::string& text, int font_scale) {
    if (!tex || font_scale <= 0)
        return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;

    const int pitch = mapped.RowPitch / 4;
    uint32_t* pixels = reinterpret_cast<uint32_t*>(mapped.pData);

    const uint32_t bg_color = 0xC8000000u;
    for (int y = 0; y < tex_h; ++y)
        for (int x = 0; x < tex_w; ++x)
            pixels[y * pitch + x] = bg_color;

    const int glyph_w = 8 * font_scale;
    const int glyph_h = 8 * font_scale;
    const int margin_x = font_scale;
    const int margin_y = font_scale;
    const uint32_t fg_color  = 0xFFFFFFFFu;
    const uint32_t sel_color = 0xFF00FFFFu;

    int cx = margin_x;
    int cy = margin_y;
    bool is_sel_line = false;

    for (char c : text) {
        if (c == '\n') {
            cx = margin_x;
            cy += glyph_h + 1;
            is_sel_line = false;
            if (cy + glyph_h > tex_h)
                break;
            continue;
        }
        if (cx == margin_x && c == '>')
            is_sel_line = true;
        if (cx + glyph_w > tex_w)
            continue;

        uint32_t color = is_sel_line ? sel_color : fg_color;
        int idx = (uint8_t)c - 32;
        if (idx < 0 || idx >= 96)
            idx = 0;

        for (int row = 0; row < 8; ++row) {
            uint8_t bits = k_font[idx][row];
            for (int col = 0; col < 8; ++col) {
                if (bits & (0x01u << col)) {
                    int bx = cx + col * font_scale;
                    int by = cy + row * font_scale;
                    for (int dy = 0; dy < font_scale; ++dy)
                        for (int dx = 0; dx < font_scale; ++dx)
                            if (bx + dx < tex_w && by + dy < tex_h)
                                pixels[(by + dy) * pitch + (bx + dx)] = color;
                }
            }
        }
        cx += glyph_w;
    }

    m_ctx->Unmap(tex, 0);
}

void D3D11Renderer::update_overlay(const std::string& text) {
    update_text_texture(m_overlay_tex.Get(), k_overlay_w, k_overlay_h, text, 6);
}

void D3D11Renderer::update_preset_overlay(const std::string& text) {
    m_has_preset_overlay = !text.empty();
    update_text_texture(m_preset_overlay_tex.Get(), k_preset_overlay_w, k_preset_overlay_h, text, 4);
}

void D3D11Renderer::update_random_overlay(const std::string& text) {
    m_has_random_overlay = !text.empty();
    update_text_texture(m_random_overlay_tex.Get(), k_random_overlay_w, k_random_overlay_h, text, 12);
}

void D3D11Renderer::render_frame(const std::vector<LayerFrame>& frames,
                                  const EyeParams& eye)
{
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(m_input_layout.Get());

    UINT stride = sizeof(Vertex), offset = 0;
    bool use_curved_mesh = std::fabs(m_screen_curve) >= 0.0001f && m_curve_vb && m_curve_ib;
    ID3D11Buffer* layer_vb = use_curved_mesh ? m_curve_vb.Get() : m_vb.Get();
    ID3D11Buffer* layer_ib = use_curved_mesh ? m_curve_ib.Get() : m_ib.Get();
    UINT layer_index_count = use_curved_mesh ? m_curve_index_count : 6;
    m_ctx->IASetVertexBuffers(0, 1, &layer_vb, &stride, &offset);
    m_ctx->IASetIndexBuffer(layer_ib, DXGI_FORMAT_R16_UINT, 0);

    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[] = { m_sampler.Get(), m_sampler_linear.Get() };
    m_ctx->PSSetSamplers(0, 2, samplers);
    m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());

    m_ctx->RSSetState(m_raster.Get());
    m_ctx->RSSetViewports(1, &eye.viewport);

    float blend_factor[4] = {1, 1, 1, 1};
    m_ctx->OMSetBlendState(m_blend.Get(), blend_factor, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);

    XMMATRIX vp = XMMatrixMultiply(eye.view, eye.proj);
    XMMATRIX scene_vp = XMMatrixMultiply(eye.scene_world, vp);

    // Upload helper for layer draws: GPU derives all copy transforms from SV_InstanceID.
    // No per-copy matrix computation on the CPU — ever.
    auto upload_layer = [&](const LayerFrame& frame, float qh, const XMFLOAT4& tint, float copy_span_sign = 1.0f) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb       = reinterpret_cast<PerDrawCB*>(mapped.pData);
            cb->vp         = XMMatrixTranspose(scene_vp);
            cb->depth      = frame.depth_meters;
            cb->quad_w     = frame.quad_width_meters;
            cb->quad_h     = qh;
            cb->quad_y     = eye.quad_y_meters;
            cb->tint       = tint;
            cb->contrast   = m_contrast;
            cb->saturation = m_saturation;
            cb->gamma      = m_gamma;
            cb->rotate90   = m_rotate_screen ? 1.0f : 0.0f;
            cb->roundness  = m_roundness;
            cb->copy_count = (float)copy_count_for_frame(frame);
            cb->copy_span  = copy_span_sign * copy_span_for_frame(frame, (int)cb->copy_count);
            cb->depthmap   = m_depthmap_enabled ? 1.0f : 0.0f;
            cb->upscale    = m_upscale_enabled ? 1.0f : 0.0f;
            cb->screen_curve = m_screen_curve;
            cb->pad0       = 0.0f;
            cb->pad1       = 0.0f;
            m_ctx->Unmap(m_cb.Get(), 0);
        }
    };

    auto upload_layer_face = [&](const LayerFrame& frame, float qh, const XMFLOAT4& tint, float face_depth) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb       = reinterpret_cast<PerDrawCB*>(mapped.pData);
            cb->vp         = XMMatrixTranspose(scene_vp);
            cb->depth      = face_depth;
            cb->quad_w     = frame.quad_width_meters;
            cb->quad_h     = qh;
            cb->quad_y     = eye.quad_y_meters;
            cb->tint       = tint;
            cb->contrast   = m_contrast;
            cb->saturation = m_saturation;
            cb->gamma      = m_gamma;
            cb->rotate90   = m_rotate_screen ? 1.0f : 0.0f;
            cb->roundness  = 0.0f;
            cb->copy_count = 0.0f;
            cb->copy_span  = 0.0f;
            cb->depthmap   = 0.0f;
            cb->upscale    = m_upscale_enabled ? 1.0f : 0.0f;
            cb->screen_curve = m_screen_curve;
            cb->pad0       = 0.0f;
            cb->pad1       = 0.0f;
            m_ctx->Unmap(m_cb.Get(), 0);
        }
    };

    // Upload helper for single-instance draws (overlay, controller).
    // Pass full MVP as gVP; depth=0, quad_w/h=1, quad_y=0 so VS becomes: mul(pos, MVP).
    auto upload_simple = [&](const XMMATRIX& mvp, const XMFLOAT4& tint) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* cb   = reinterpret_cast<PerDrawCB*>(mapped.pData);
            cb->vp         = XMMatrixTranspose(mvp);
            cb->depth      = 0.0f;
            cb->quad_w     = 1.0f;
            cb->quad_h     = 1.0f;
            cb->quad_y     = 0.0f;
            cb->tint       = tint;
            cb->contrast   = 1.0f; // no grading on UI overlay
            cb->saturation = 1.0f;
            cb->gamma      = 1.0f;
            cb->rotate90   = 0.0f; // never rotate overlay/controller
            cb->roundness  = 0.0f; // no sphere effect on UI elements
            cb->copy_count = 0.0f;
            cb->copy_span  = 0.0f;
            cb->depthmap   = 0.0f;
            cb->upscale    = 0.0f;
            cb->screen_curve = 0.0f;
            cb->pad0       = 0.0f;
            cb->pad1       = 0.0f;
            m_ctx->Unmap(m_cb.Get(), 0);
        }
    };

    const XMFLOAT4 tint_normal = {m_brightness, m_brightness, m_brightness, 1.0f};
    const XMFLOAT4 tint_blink  = {1.5f, 1.5f, 0.3f, 1.0f};

    ULONGLONG blink_ms   = GetTickCount64() % 1500ULL;
    bool      blink_on   = blink_ms < 1000ULL; // 1.0 s on, 0.5 s off

    // Draw back-to-front (highest depth first) so alpha blending composites correctly.
    for (int i = 0; i < (int)frames.size(); ++i) {
        const auto& f = frames[i];
        if (f.rgba.empty() || i >= (int)m_layers.size()) continue;

        float aspect    = (f.height > 0) ? (float)f.width / f.height : 1.0f;
        float qw        = f.quad_width_meters;
        // When rotated 90°, the physical quad flips its aspect (tall instead of wide)
        float qh        = m_rotate_screen ? (qw * aspect) : (qw / aspect);
        int   copy_count = copy_count_for_frame(f);
        float copy_span  = copy_span_for_frame(f, copy_count);
        bool  is_sel    = m_editor_active && (i == m_editor_selected);
        XMFLOAT4 ltint;
        if      (is_sel && !m_editor_blink_on) ltint = tint_blink;
        else                                   ltint = tint_normal;

        m_ctx->PSSetShaderResources(0, 1, m_layers[i].srv.GetAddressOf());
        upload_layer(f, qh, ltint);

        // Main quad first (inst 0 → offset=0): writes depth.
        m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
        m_ctx->DrawIndexed(layer_index_count, 0, 0);

        if (m_hover_mask_layer == i && m_hover_srv && blink_on) {
            XMFLOAT4 hover_tint = {1.0f, 1.0f, 1.0f, 0.95f};
            switch (m_hovered_group) {
            case 0: hover_tint = {0.47f, 0.47f, 0.47f, 0.95f}; break; // gray
            case 1: hover_tint = {0.24f, 0.47f, 0.86f, 0.95f}; break; // blue
            case 2: hover_tint = {0.24f, 0.78f, 0.31f, 0.95f}; break; // green
            case 3: hover_tint = {0.86f, 0.24f, 0.24f, 0.95f}; break; // red
            default: break;
            }
            m_ctx->PSSetShaderResources(0, 1, m_hover_srv.GetAddressOf());
            upload_layer(f, qh, hover_tint);
            m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
            m_ctx->DrawIndexed(layer_index_count, 0, 0);
        }

        if (m_3d_layers_enabled && copy_count > 0 && copy_span > 1e-5f) {
            float front_depth = (std::max)(0.01f, f.depth_meters - copy_span);
            upload_layer_face(f, qh, ltint, front_depth);
            m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
            m_ctx->DrawIndexed(layer_index_count, 0, 0);
        } else if (copy_count > 0) {
            // Flat mode: classic duplicated slices, blended on top of the layer.
            // Depthmap mode: per-pixel brightness selects how far each pixel extrudes,
            // so slices must write depth and naturally occlude each other.
            m_ctx->OMSetDepthStencilState(
                m_depthmap_enabled ? m_depth_state.Get() : m_depth_state_slice.Get(), 0);
            m_ctx->DrawIndexedInstanced(layer_index_count, copy_count, 0, 0, 1);
            if (m_depthmap_enabled && m_depthmap_mirror_enabled) {
                // Mirror mode: render the same relief behind the base plane as well.
                // This makes the object read as a thicker volume when viewed from the side.
                upload_layer(f, qh, ltint, -1.0f);
                m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
                m_ctx->DrawIndexedInstanced(layer_index_count, copy_count, 0, 0, 1);
            }
        }
    }

    // In 3D layers mode, close the thickness with silhouette walls so the user
    // sees a solid volume instead of exposed slice bands when moving close.
    draw_extruded_layer_walls(frames, eye);

    // Floor shadows are projected after the scene so they darken the background,
    // but before the side panels/controllers so UI stays crisp.
    draw_floor_shadows(frames, eye);
    draw_ambilight(frames, eye);

    // Restore the standard textured pipeline after the flat-color shadow pass.
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(m_input_layout.Get());
    m_ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->PSSetSamplers(0, 2, samplers);

    // --- Left wall status panel ---
    {
        float panel_h = 3.0f;
        float panel_w = panel_h * ((float)k_overlay_w / (float)k_overlay_h);
        float panel_x = -2.25f;
        float panel_z =  0.0f;  // centered in left-wall view (was -1.65 = near front screen)
        XMMATRIX panel_world =
            XMMatrixScaling(-panel_w, panel_h, 1.0f) *
            XMMatrixRotationY(-XM_PIDIV2) *
            XMMatrixTranslation(panel_x, eye.quad_y_meters, panel_z);

        m_ctx->PSSetShaderResources(0, 1, m_overlay_srv.GetAddressOf());
        upload_simple(XMMatrixMultiply(panel_world, vp), {1.0f, 1.0f, 1.0f, 0.92f}); // vp: panels fixed in world, unaffected by scene tilt
        m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
        m_ctx->DrawIndexed(6, 0, 0);
    }

    // --- Preset strip below the left status panel ---
    if (m_has_preset_overlay && m_preset_overlay_srv) {
        constexpr float preset_h = 0.72f;
        constexpr float preset_gap = 0.10f;
        float preset_w = 3.0f * ((float)k_overlay_w / (float)k_overlay_h);
        float preset_x = -2.25f;
        float preset_z =  0.0f;
        float preset_y = eye.quad_y_meters - (3.0f * 0.5f + preset_h * 0.5f + preset_gap);
        XMMATRIX preset_world =
            XMMatrixScaling(-preset_w, preset_h, 1.0f) *
            XMMatrixRotationY(-XM_PIDIV2) *
            XMMatrixTranslation(preset_x, preset_y, preset_z);

        m_ctx->PSSetShaderResources(0, 1, m_preset_overlay_srv.GetAddressOf());
        upload_simple(XMMatrixMultiply(preset_world, vp), {1.0f, 1.0f, 1.0f, 0.94f});
        m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
        m_ctx->DrawIndexed(6, 0, 0);
    }

    // --- Right wall random-game panel ---
    if (m_has_random_overlay && m_random_overlay_srv) {
        constexpr float random_h = 3.0f;
        float random_w = random_h * ((float)k_random_overlay_w / (float)k_random_overlay_h);
        float random_x = 2.25f;
        float random_z = 0.0f;
        XMMATRIX random_world =
            XMMatrixScaling(-random_w, random_h, 1.0f) *
            XMMatrixRotationY(XM_PIDIV2) *
            XMMatrixTranslation(random_x, eye.quad_y_meters, random_z);

        m_ctx->PSSetShaderResources(0, 1, m_random_overlay_srv.GetAddressOf());
        upload_simple(XMMatrixMultiply(random_world, vp), {1.0f, 1.0f, 1.0f, 0.94f});
        m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
        m_ctx->DrawIndexed(6, 0, 0);
    }

    // --- Color palette panel (flush to the left of the status panel) ---
    if (m_colorpal_srv) {
        constexpr float pal_h = 3.0f;
        constexpr float pal_w = 0.375f; // 1 column, width = panel_h/8 → square swatches
        constexpr float pal_x = -2.20f; // 5 cm closer than status panel → t always wins
        constexpr float pal_z =  1.25f; // center z: just left of status panel
        XMMATRIX pal_world =
            XMMatrixScaling(-pal_w, pal_h, 1.0f) *
            XMMatrixRotationY(-XM_PIDIV2) *
            XMMatrixTranslation(pal_x, eye.quad_y_meters, pal_z);
        m_ctx->PSSetShaderResources(0, 1, m_colorpal_srv.GetAddressOf());
        upload_simple(XMMatrixMultiply(pal_world, vp), {1.0f, 1.0f, 1.0f, 0.95f});
        m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
        m_ctx->DrawIndexed(6, 0, 0);
    }

    // --- Controller icon quads ---
    static const XMFLOAT4 ctrl_tint[2] = { {1.0f, 1.0f, 1.0f, 0.98f},
                                            {1.0f, 1.0f, 1.0f, 0.98f} };
    static constexpr float k_ctrl_size = 0.13f;
    m_ctx->OMSetDepthStencilState(m_depth_state.Get(), 0);
    for (int hand = 0; hand < 2; ++hand) {
        if (!m_ctrl_valid[hand]) continue;
        m_ctx->PSSetShaderResources(0, 1, m_ctrl_srv[hand].GetAddressOf());
        // Scale a small quad, then apply the controller's world transform
        XMMATRIX ctrl_world =
            XMMatrixScaling(k_ctrl_size, k_ctrl_size, 1.0f) *
            XMMatrixRotationY(XM_PI) *
            m_ctrl_world[hand];
        XMMATRIX ctrl_mvp   = XMMatrixMultiply(ctrl_world, vp);
        upload_simple(ctrl_mvp, ctrl_tint[hand]);
        m_ctx->DrawIndexed(6, 0, 0);
    }

    // --- Laser beams (flat-color ribbons from controller aim to hit point) ---
    // Restore standard pipeline state first, then draw beams
    m_ctx->OMSetDepthStencilState(m_depth_state_slice.Get(), 0);
    draw_laser_beams(eye);

    // Restore normal input layout for next frame
    m_ctx->IASetInputLayout(m_input_layout.Get());
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->PSSetSamplers(0, 2, samplers);
}
