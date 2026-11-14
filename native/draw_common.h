#pragma once
// What both renderers make of the console's state, copied from the Vulkan backend rather than rewritten: the constants a
// draw hands its shaders, the keys its shaders are cached under, and the CPU side texture and memory helpers. A second
// backend that computed these its own way would draw a different picture from the first one, which is exactly what the
// port cannot afford while the two are being compared.
//
// Everything here is API free: the two structures the drawing state lands in (DrawShaders, DrawTextures) carry the
// renderer's own handles in the fields at the end, and nothing else differs.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace port::draw {

using namespace gpu;

// ---- the draw's state as this renderer keeps it

struct DrawTextures {
    TexInfo info[4];
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
    uint32_t gw[4] = {};    // a render target's guest width, 0 for any other texture
    unsigned tex[4] = {};   // the renderer's textures, one per stage
};

struct DrawShaders {
    bool prog = false;
    bool fixed_transform = false;
    bool quad = false;  // a screen quad: QuadHlsl
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    unsigned vs = 0, ps = 0;
};

struct GuestExtent {
    uint32_t w, h;
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];
    uint32_t strides[16];
    uint32_t buffers;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kQuadVertices = 6;   // a quad drawn as two triangles
constexpr uint32_t kSceneSize = 512;    // the game's 512x512 render targets: the HD scene's
constexpr float kSceneBoost = 640.0f / 512.0f;
constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kPointsSalt = 0x9017;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kQuadSalt = 0xC3C30000;  // the QuadHlsl variant, apart from the plain shader
constexpr float kAlphaTestAlways = 7.0f;
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;
constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6;

std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    const std::string clamp = "    p.w = magclamp(p.w);\n";
    const std::string screen_space = "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n";
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    size_t at = src.find(clamp);
    if (at != std::string::npos) src.replace(at, clamp.size(), screen_space);
    return src;
}

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

void Rgb565(uint16_t c, uint8_t* bgr) {
    bgr[2] = uint8_t((c >> 11) * 255 / 31);
    bgr[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    bgr[0] = uint8_t((c & 31) * 255 / 31);
}

void ColourBlock(const uint8_t* b, bool dxt1, uint8_t* out) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int i = 0; i < 4; i++) pal[i][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out + i * 4, pal[(bits >> (i * 2)) & 3], 4);
}

void DecodeDxt(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h, uint32_t z,
               std::vector<uint8_t>& out) {
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    out.assign(size_t(w) * h * z * 4, 0);
    uint8_t px[64];
    for (uint32_t s = 0; s < z; s++)
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t at = (size_t(s) * bh + by) * row_pitch + size_t(bx) * block;
                if (at + block > in.size()) continue;
                const uint8_t* b = &in[at];
                ColourBlock(host == Host::BC1 ? b : b + 8, host == Host::BC1, px);
                if (host == Host::BC2) {
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                } else if (host == Host::BC3) {
                    uint8_t a[8] = {b[0], b[1]};
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                    } else {
                        for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                        std::memcpy(&out[((size_t(s) * h + by * 4 + y) * w + bx * 4 + x) * 4], px + (y * 4 + x) * 4, 4);
            }
}

uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (a.topology == Topology::Points) vkey ^= kPointsSalt;
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgram(*s.vs, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to Vulkan's RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void FillTextureConstants(const State& s, const DrawShaders& sh, float target_shift, const DrawTextures& stages,
                          DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) c.rtshift[i] = target_shift;
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}


void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++)
        if ((fk.types >> (i * 2)) & 3) LightToView(s.lights[size_t(i)], vm, c.lt[i]);
}

struct GuestRect {
    int32_t left, top, right, bottom;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}

void ReadGuestPoint(uint32_t points, uint32_t i, int32_t& x, int32_t& y) {
    x = int32_t(kernel::R32(points + i * 8));
    y = int32_t(kernel::R32(points + i * 8 + 4));
}

uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

}  // namespace port::draw
