// The MRP's modern lighting: the sun's cascaded shadow maps (see mrp_lighting.h). Host code, an enhancement: none of it is the game's.
#include <game/render/mrp_lighting.h>

#include <game/ovl/ovl_common.h>
#include <game/ovl/ovl_reloc.h>
#include <game/render/mrp_lod.h>
#include <game/render/mrp_moby_renderer.h>
#include <game/render/mrp_world_renderer.h>
#include <game/render/render_view.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

namespace rac1 {

namespace {
int env_int(const char* name, int def) {
    const char* e = std::getenv(name);
    return e != nullptr ? std::atoi(e) : def;
}
}  // namespace

int g_modern_shadows = env_int("RAC1_SHADOWS", 0);
int g_shadow_quality = std::clamp(env_int("RAC1_SHADOW_RES", 1), 0, 2);
int g_shadow_strength = std::clamp(env_int("RAC1_SHADOW_STRENGTH", 55), 0, 100);
int g_shadow_debug = env_int("RAC1_SHADOW_DEBUG", 0);
int g_modern_points = env_int("RAC1_POINTS", 1);
int g_point_shadows = env_int("RAC1_POINT_SHADOWS", 0);
int g_auto_lights = env_int("RAC1_AUTO_LIGHTS", 1);
int g_shadow_cache = env_int("RAC1_SHADOW_CACHE", 1);
int g_user_light_selected = -1;
int g_shadow_fade = env_int("RAC1_SHADOW_FADE", 1);
int g_ao = env_int("RAC1_AO", 0);
float g_shadow_moby_dist = std::getenv("RAC1_SHADOW_MOBY_DIST") != nullptr ? static_cast<float>(std::atof(std::getenv("RAC1_SHADOW_MOBY_DIST"))) : 0.0f;

namespace {
constexpr int kCascades = 3;
constexpr int kUnit = 24;  // the shadow map's texture unit (the world's samplers use 0..16)
constexpr GLenum kGlCubeMapArray = 0x9009;  // (GL_TEXTURE_CUBE_MAP_ARRAY, GL 4.0: not in the loader's header)
constexpr int kPtUnit = 25;  // the point lights' cube shadow maps' unit
constexpr int kTileUnit = 26;
constexpr int kDepthUnit = 27;  // the cascades' maps without the compare (their depths) // the point lights' screen tiles' unit
constexpr int kTilesX = 32, kTilesY = 18;  // (the view's clip square cut into tiles: per tile the lights whose sphere reaches it)
constexpr int kPtShadows = 4;  // the point lights with a shadow at most (the nearest)
constexpr int kPtRes = 512;    // their cube faces' resolution
constexpr float kPtNear = 0.3f;  // (an automatic emitter sits on its lamp's surface: the lamp itself not its occluder)
constexpr int kMaxPts = 96;  // the point lights lit per pixel at most (the shaders' arrays): the game's 8, then the automatic emitters
// how far toward the sun (from a cascade's centre) the casters reach, units: the light box's near plane, the same for the frame's casters
// and the added ones (no depth clamp: a far caster the camera sees would cast, one it does not would not - its shadow came and went)
constexpr float kBack = 400.0f;
// a cached cascade's box: its sphere this many times the view's slice's (the slice moves inside it this far before the
// static world is drawn again); the casters outside the view are looked for in the same larger sphere (mrp_lighting_cull_params)
constexpr float kCacheGrow = 1.25f;
std::atomic<bool> g_sun_known{false};
std::atomic<bool> g_has_sun{true};  // the level's key light comes from above: the sun's shadows (else none: Blarg)
// how much of the sun's weight the view keeps: 0 covered from the sun .. 1 open (see kProbeW)
float view_open();
std::atomic<bool> g_casc_on{true};  // the cascades drawn: the zone has a sun (its shadows' weight may be faded to 0: the view covered)
float g_cull_sun[3] = {0.0f, 0.0f, 1.0f};  // (the sun for the casters' search on the scene's worker)
float g_cull_tan[2] = {0.0f, 0.0f};         // (the drawn view's half-angle tangents, 1 / P00 and 1 / P11)

struct Cascade {
    glm::mat4 vp{1.0f};   // world -> the light's clip space
    glm::mat4 tex{1.0f};  // world -> the map's [0, 1] coordinates and depth
    float texel = 0.0f;   // a texel's size in world units
    int res = 0;          // its square in the map: the map's size, or half of it (High: the far cascades at 2048)
    float part = 1.0f;    // (its square's share of the layer: the sampling's scale, uShPart)
    float range = 1.0f;   // its depth range, units (the box's near to far)
};

struct State {
    GLuint fbo = 0, tex = 0;
    int res = 0;
    bool on = false;  // this frame's receivers shade
    Cascade casc[kCascades];
    glm::vec3 sun{0.0f, 0.0f, 1.0f};  // toward the sun
    float sun_k = 1.0f;               // its shadows' weight (the zone's light's share; faded between zones)
    glm::vec4 key{0.0f};              // the key light's colour (as the set holds it) and back-face factor: what a shadow takes off the world's colours
    int npts = 0;                     // the frame's active point lights: position, radius; colour
    glm::vec4 pt_pos[kMaxPts], pt_col[kMaxPts];  // (pt_col.w: the light's cube in the shadow array, -1 none)
    int pt_slot[kMaxPts] = {};                   // (the game's slot of each: its casters' view, 3 + slot; -1 an automatic emitter)
    uint64_t frame = 0;              // the far cascade's turn
    // the static world's cache per cascade: the cascade's box held still while the view's slice stays inside it (its
    // sphere, kCacheGrow times the slice's), the world's casters drawn into `stex` only when it moves; each frame that map is copied
    // into the cascade's layer and the mobys drawn over it
    struct Cache {
        bool valid = false;
        glm::vec3 lc{0.0f};  // the box's centre in the light's frame (x, y snapped to its texels)
        float R = 0.0f, back = 0.0f;
        int res = 0;
        GLuint stex = 0;
        int stex_res = 0;
        bool incomplete = false;  // drawn without the casters outside the view: drawn again as soon as they are made
        glm::vec3 prev{0.0f};     // the slice's centre the frame before (its speed: when the box's edge is near)
        int dirty[4] = {0, 0, 0, 0};  // the layer's rectangle the mobys were drawn over last frame (x0, y0, x1, y1 texels; empty x1 <= x0)
    } cache[kCascades];
    std::atomic<bool> want{true};  // the next frame's casters outside the view wanted (the scene's worker reads it)
    bool refresh[kCascades] = {};
    uint64_t cache_key = 0;  // (the level, the sun, the LOD, the quality: any change empties the caches)
    uint64_t refreshes = 0, frames_cached = 0, built = 0;
    GLuint ptex = 0;                  // the point lights' cube shadow maps (a cube map array, kPtShadows cubes)
    glm::vec3 cam{0.0f};
    glm::mat4 view_proj{1.0f};  // the view's world -> clip (the point lights' tiles)
    GLuint tiles = 0;           // their texture (kTilesX x kTilesY, RGBA32UI: lights 0..31, 32..63, 64..95)
    glm::mat4 inv_view{1.0f};
    glm::vec2 proj{1.0f, 1.0f};
    float splits[kCascades] = {18.0f, 60.0f, 220.0f};
    bool logged = false;
} g_sh;

// orig: (host) a cascade's resolution - at the High setting (4096) the near cascade alone at full size, the others at half
// (drawn into a corner of their layer: the far ones' texels are coarse anyway, the cost of 4096 x 4096 three times is not)
int cascade_res(int k) { return g_shadow_quality >= 2 && k > 0 ? g_sh.res / 2 : g_sh.res; }

// orig: (host) one light set's sun - of its two directional lights (0x17fec0 + 0x40 x set: colour 1, direction 1, colour 2,
// direction 2) the brightest coming from above (at least ~11 degrees: the direction negated, the relighting
// routines take it as the light's travel), with its share of the set's brightest (the shadow's weight: a dim second light, weak
// shadows); none when both come from below or the horizon (Blarg's hangar's key light, Umbris' lava glow from beneath)
struct SetSun {
    glm::vec3 dir{0.0f, 0.0f, 1.0f};
    float weight = 0.0f;
    bool ok = false;
    glm::vec4 key{0.0f};  // that light's colour (rgb as the set holds it: x 128 in the vertex colours' bytes) and back-face factor (w: the set's k)
};
SetSun set_sun(uint32_t set) {
    SetSun r;
    if (set > 15) return r;
    const uint32_t base = OV(0x0017fec0u) + 0x40 * set;
    float lum[2], best = 0.0f;
    glm::vec3 dir[2];
    for (uint32_t l = 0; l < 2; ++l) {
        const uint32_t c = base + 0x20 * l, d = c + 0x10;
        lum[l] = std::max({lwc1(c), lwc1(c + 4), lwc1(c + 8), 0.0f});
        dir[l] = glm::vec3(-lwc1(d), -lwc1(d + 4), -lwc1(d + 8));
        best = std::max(best, lum[l]);
    }
    int pick = -1;
    for (int l = 0; l < 2; ++l) {
        const float n = glm::length(dir[l]);
        if (!(n > 1e-3f) || dir[l].z < 0.2f * n || !(lum[l] > 0.0f)) continue;
        if (pick < 0 || lum[l] > lum[pick]) pick = l;
    }
    if (pick < 0 || !(best > 0.0f)) return r;
    r.dir = glm::normalize(dir[pick]);
    r.weight = lum[pick] / best;
    r.ok = true;
    const uint32_t c = base + 0x20 * static_cast<uint32_t>(pick);
    r.key = glm::vec4(std::max(lwc1(c), 0.0f), std::max(lwc1(c + 4), 0.0f), std::max(lwc1(c + 8), 0.0f), std::clamp(lwc1(c + 0xc), 0.0f, 1.0f));
    return r;
}

// orig: (host) the direction toward the sun this frame, and its shadows' weight (0..1; 0: no sun): RAC1_SUN_DIR="x,y,z" (world, z up),
// else the light sets the game lights the hero with - moby +0x38 / +0x39, blended by +0x3a / 256: the zone's
// own key light, as Ratchet is lit (Veldin's outdoors set 0, its house set 1; Blarg's zones each their own) - each set's sun (set_sun),
// the two blended. On Veldin's outdoors (-0.397, 0.852, -0.342): the low warm sun the sky shows (checked against the screenshots)
glm::vec3 sun_direction(float* weight, glm::vec4* key) {
    *weight = 1.0f;
    *key = glm::vec4(0.0f);
    if (const char* e = std::getenv("RAC1_SUN_DIR")) {
        float x = 0, y = 0, z = 0;
        if (std::sscanf(e, "%f,%f,%f", &x, &y, &z) == 3 && x * x + y * y + z * z > 1e-6f) return glm::normalize(glm::vec3(x, y, z));
    }
    if (std::getenv("RAC1_SHADOW_LIGHTS") != nullptr) {  // (testing: the level's light sets, once)
        static uint32_t shown = 0xffffffffu;
        if (shown != OV(0x0017fec0u)) {
            shown = OV(0x0017fec0u);
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t b = shown + 0x40 * i;
                float v[16];
                bool any = false;
                for (uint32_t k = 0; k < 16; ++k) {
                    v[k] = lwc1(b + 4 * k);
                    any = any || v[k] != 0.0f;
                }
                if (any)
                    std::printf("[shadows] light set %u @%08x: c1 %.2f %.2f %.2f k1 %.2f d1 %.3f %.3f %.3f | c2 %.2f %.2f %.2f k2 %.2f d2 %.3f %.3f %.3f\n", i, b, v[0], v[1], v[2],
                                v[3], v[4], v[5], v[6], v[8], v[9], v[10], v[11], v[12], v[13], v[14]);  // (as float)
            }
        }
    }
    if (std::getenv("RAC1_SUN_TRACE") != nullptr) {  // (testing: the light sets the hero is lit with)
        static uint32_t last = 0xffffffffu;
        const uint32_t hero = lwu(OV(0x0015ffd8u));
        static int census = 0;
        if (hero != 0 && ++census == 400) {  // (the light sets the level's mobys use, once: count, an example position, the key light)
            std::map<uint32_t, std::pair<int, uint32_t>> sets;
            for (uint32_t m = hero; m < 0x01fe0000u && lbu(m + 0x20) != 0xff; m += 0x100) {
                if (static_cast<int8_t>(lbu(m + 0x20)) < 0) continue;
                const uint32_t k = lbu(m + 0x38) | (lbu(m + 0x39) << 8);
                auto& e = sets[k];
                if (e.first++ == 0) e.second = m;
            }
            for (const auto& [k, e] : sets) {
                const uint32_t da = OV(0x0017fec0u) + 0x40 * (k & 15) + 0x10;
                std::printf("[sun] sets %u / %u: %d mobys, e.g. at %.0f %.0f %.0f; set a's d1 (%.2f %.2f %.2f) colour %.2f %.2f %.2f\n", k & 0xff, k >> 8, e.first,
                            lwc1(e.second + 0x10), lwc1(e.second + 0x14), lwc1(e.second + 0x18), lwc1(da), lwc1(da + 4), lwc1(da + 8), lwc1(da - 0x10), lwc1(da - 0xc),
                            lwc1(da - 8));
            }
        }
        if (hero != 0) {
            const uint32_t a = lbu(hero + 0x38), b = lbu(hero + 0x39), w = lbu(hero + 0x3a), key = a | (b << 8) | (w << 16);
            if (key != last) {
                last = key;
                const uint32_t da = OV(0x0017fec0u) + 0x40 * (a & 15) + 0x10, db = OV(0x0017fec0u) + 0x40 * (b & 15) + 0x10;
                std::printf("[sun] hero %.1f %.1f %.1f: sets %u / %u blend %u: d1 a (%.2f %.2f %.2f) b (%.2f %.2f %.2f)\n", lwc1(hero + 0x10), lwc1(hero + 0x14), lwc1(hero + 0x18), a, b, w,
                            lwc1(da), lwc1(da + 4), lwc1(da + 8), lwc1(db), lwc1(db + 4), lwc1(db + 8));
            }
        }
    }
    uint32_t a = 0, b = 0;
    float w = 0.0f;
    static const bool zone_dir = std::getenv("RAC1_SUN_ZONE") != nullptr;  // (testing: turn the sun to each zone's light set)
    if (const uint32_t hero = lwu(OV(0x0015ffd8u)); hero != 0) {
        a = lbu(hero + 0x38);
        b = lbu(hero + 0x39);
        w = static_cast<float>(lbu(hero + 0x3a)) / 256.0f;
        if (a > 15) a = 0;  // (15 / 15 or none: the level's own)
        if (b > 15) b = a;
        if (a == 15) a = b = 0;
    }
    const SetSun s0 = set_sun(0);
    const SetSun sa = set_sun(a), sb = set_sun(b);
    if (!zone_dir && s0.ok) {
        const float wa = sa.ok ? sa.weight : 0.0f;
        const float wb = sb.ok ? sb.weight : 0.0f;
        *weight = wa + (wb - wa) * w;
        *key = s0.key;
        return s0.dir;
    }
    if (sa.ok && sb.ok) {
        *weight = sa.weight + (sb.weight - sa.weight) * w;
        *key = sa.key + (sb.key - sa.key) * w;
        return glm::normalize(sa.dir + (sb.dir - sa.dir) * w);
    }
    if (sa.ok || sb.ok) {
        const SetSun& s1 = sa.ok ? sa : sb;
        *weight = s1.weight * (sa.ok ? 1.0f - w : w);
        *key = s1.key;
        return s1.dir;
    }
    *weight = 0.0f;
    return glm::normalize(glm::vec3(0.4f, -0.85f, 0.35f));
}

// orig: (host) testing, RAC1_TFRAG_LIGHT_TRACE=<every>[,x,y,z,r]: the terrain's lit colours near the hero (or that point) taken
// apart as the game's relighting (world_tfrag_lighting 0x295350, world_render.md section 10) makes them - the vertex's base colour,
// its light set's two directional terms (colour x max(n.l, k n.l)) - against the colour EE memory holds; and the sun's set
void tfrag_light_trace() {
    const char* e = std::getenv("RAC1_TFRAG_LIGHT_TRACE");
    if (e == nullptr) return;
    static uint64_t n = 0;
    int every = 60;
    float px = 0, py = 0, pz = 0, pr = 12.0f;
    const int got = std::sscanf(e, "%d,%f,%f,%f,%f", &every, &px, &py, &pz, &pr);
    if (n++ % static_cast<uint64_t>(std::max(1, every)) != 0) return;
    if (got < 4) {
        const uint32_t hero = lwu(OV(0x0015ffd8u));
        if (hero == 0) return;
        px = lwc1(hero + 0x10);
        py = lwc1(hero + 0x14);
        pz = lwc1(hero + 0x18);
    }
    const uint32_t table = lwu(OV(0x00160f4cu));
    const int32_t count = lw(OV(0x00160f50u));
    if (table == 0 || count <= 0 || count > 0x10000) return;
    const uint32_t sets = OV(0x0017fec0u), norms = OV(0x00166080u);
    auto set_term = [&](uint32_t set, const glm::vec3& nrm, int l) {  // (one light's term, 0..255 units: the routine's colours are 65536 + byte / 128)
        const uint32_t c = sets + 0x40 * (set & 15) + 0x20 * static_cast<uint32_t>(l), d = c + 0x10;
        const float k = lwc1(c + 0xc);
        const float nd = -glm::dot(nrm, glm::vec3(lwc1(d), lwc1(d + 4), lwc1(d + 8)));  // (toward the light)
        return glm::vec3(lwc1(c), lwc1(c + 4), lwc1(c + 8)) * (128.0f * std::max(nd, k * nd));
    };
    double sum_base = 0, sum_t[2] = {0, 0}, sum_lit = 0, sum_err = 0;
    int verts = 0, back = 0, neg_key = 0;
    std::map<uint32_t, int> set_use;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t ent = table + 0x40 * static_cast<uint32_t>(i);
        const uint32_t data = lwu(ent + 0x10);
        if (data == 0 || data >= 0x02000000u) continue;
        const int8_t single = static_cast<int8_t>(lbu(ent + 0x34));  // (>= 0: one set for the whole entry, its count +0x29 pairs)
        const uint32_t va = data + static_cast<uint32_t>(static_cast<int32_t>(lh(ent + 0x30))) + 0x10;
        const uint32_t nv = single >= 0 ? 2u * lbu(ent + 0x29) : lbu(ent + 0x3c);
        const uint32_t lit = data + lhu(ent + 0x1e);
        const glm::vec3 base_pos(static_cast<float>(lw(va - 0x10)) / 1024.0f, static_cast<float>(lw(va - 0xc)) / 1024.0f, static_cast<float>(lw(va - 8)) / 1024.0f);
        for (uint32_t v = 0; v < nv; ++v) {
            const uint32_t vx = va + 8 * v;
            const uint32_t po = data + lhu(vx);
            const glm::vec3 p = base_pos + glm::vec3(lh(po), lh(po + 2), lh(po + 4)) / 1024.0f;
            if (glm::length(p - glm::vec3(px, py, pz)) > pr) continue;
            const uint32_t az = lbu(vx + 2), el = lbu(vx + 3);
            const float ca = lwc1(norms + 8 * az), sa = lwc1(norms + 8 * az + 4), ce = lwc1(norms + 8 * el), se = lwc1(norms + 8 * el + 4);
            const glm::vec3 nrm(ca * ce, sa * ce, se);
            const uint16_t b5 = lhu(vx + 4), sh = lhu(vx + 6);
            const glm::vec3 base(static_cast<float>((b5 & 31) << 3), static_cast<float>(((b5 >> 5) & 31) << 3), static_cast<float>(((b5 >> 10) & 31) << 3));
            const uint32_t sa_ = single >= 0 ? static_cast<uint32_t>(single) : (sh & 15u), sb_ = single >= 0 ? sa_ : ((sh >> 4) & 15u);
            const float wb = single >= 0 ? 0.0f : static_cast<float>(sh >> 8) / 256.0f;
            glm::vec3 t[2];
            for (int l = 0; l < 2; ++l) t[l] = set_term(sa_, nrm, l) * (1.0f - wb) + set_term(sb_, nrm, l) * wb;  // (approximate: the game blends the rows)
            const glm::vec3 sum = glm::min(base + t[0] + t[1], glm::vec3(255.0f));
            const uint32_t lc = lwu(lit + 4 * v);
            const glm::vec3 got_c(static_cast<float>(lc & 0xff), static_cast<float>((lc >> 8) & 0xff), static_cast<float>((lc >> 16) & 0xff));
            auto lum = [](const glm::vec3& x) { return (x.r + x.g + x.b) / 3.0; };
            sum_base += lum(base);
            sum_t[0] += lum(t[0]);
            sum_t[1] += lum(t[1]);
            sum_lit += lum(got_c);
            sum_err += std::fabs(lum(sum) - lum(got_c));
            back += glm::dot(nrm, g_sh.sun) <= 0.0f ? 1 : 0;
            neg_key += lum(t[0]) < 0.0 ? 1 : 0;
            ++set_use[sa_ | (sb_ << 4)];
            ++verts;
        }
    }
    if (verts == 0) return;
    std::printf("[tfrag light] at %.1f %.1f %.1f r %.0f: %d vertices; mean (0..255) lit %.1f = base %.1f + light 1 %.1f + light 2 %.1f (recomposed off by %.1f); "
                "facing away from the sun %d, light 1 negative %d; sets", px, py, pz, pr, verts, sum_lit / verts, sum_base / verts, sum_t[0] / verts, sum_t[1] / verts,
                sum_err / verts, back, neg_key);
    for (const auto& [k, c] : set_use) std::printf(" %u/%u:%d", k & 15, k >> 4, c);
    std::printf("; sun (%.2f %.2f %.2f) weight %.2f\n", g_sh.sun.x, g_sh.sun.y, g_sh.sun.z, g_sh.sun_k);
}

void read_splits() {
    if (const char* e = std::getenv("RAC1_SHADOW_SPLITS")) {
        float a = 0, b = 0, c = 0;
        if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3 && a > 0 && b > a && c > b) {
            g_sh.splits[0] = a;
            g_sh.splits[1] = b;
            g_sh.splits[2] = c;
        }
    }
}

bool ensure_resources() {
    static const int sizes[3] = {1024, 2048, 4096};
    const int want = sizes[std::clamp(g_shadow_quality, 0, 2)];
    if (g_sh.fbo != 0 && g_sh.res == want) return true;
    if (g_sh.fbo == 0) {
        glGenFramebuffers(1, &g_sh.fbo);
        glGenTextures(1, &g_sh.tex);
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_sh.tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, want, want, kCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    g_sh.res = want;
    return true;
}

// orig: (host) the cascades fitted to the view's slices: per slice its bounding sphere (stable under the camera's rotation), the
// light's orthographic box around it reaching `back` units toward the sun for the casters, its centre snapped to the map's texels
// (no shimmering when the camera moves)
void fit_cascades() {
    const glm::mat4 view = render_view_matrix();
    float vpf[16];
    mrp_world_view_proj(vpf);
    const glm::dmat4 vp = glm::make_mat4(vpf);
    const glm::dmat4 proj = vp * glm::inverse(glm::dmat4(view));
    g_sh.proj = glm::vec2(static_cast<float>(proj[0][0]), static_cast<float>(proj[1][1]));
    g_sh.view_proj = glm::mat4(vp);
    g_sh.inv_view = glm::mat4(glm::inverse(glm::dmat4(view)));
    g_sh.cam = glm::vec3(g_sh.inv_view[3]);
    {  // the zone's sun: turned toward a new zone's at most 1 degree a frame, its shadows' weight faded (1 / 30 a frame)
        float tw = 0.0f;
        glm::vec4 tkey(0.0f);
        const glm::vec3 target = sun_direction(&tw, &tkey);
        const float zone_w = tw;
        tw *= view_open();  // (a view covered from the sun: its shadows faded out, the cascades still drawn - the probe's)
        static glm::vec3 cur{0.0f, 0.0f, 1.0f};
        static float curw = 0.0f;
        static int32_t lvl = -1;
        const int32_t level = lw(0x0015ed84);
        static glm::vec4 curkey{0.0f};
        if (level != lvl) {
            lvl = level;
            cur = target;
            curw = tw;
            curkey = tkey;
        } else {
            curkey += (tkey - curkey) * 0.1f;  // (a new zone's key light's colour: over about half a second)
            curw += std::clamp(tw - curw, -1.0f / 30.0f, 1.0f / 30.0f);
            if (curw <= 0.02f) {
                cur = target;  // (unseen: at once)
            } else {
                const float c = std::clamp(glm::dot(cur, target), -1.0f, 1.0f), ang = std::acos(c), step = glm::radians(1.0f);
                if (ang <= step) cur = target;
                else cur = glm::normalize(cur * std::sin(ang - step) / std::sin(ang) + target * std::sin(step) / std::sin(ang));
            }
        }
        g_sh.sun = cur;
        g_sh.sun_k = curw;
        g_sh.key = curkey;
        g_has_sun.store(curw > 0.01f);
        g_casc_on.store(curw > 0.01f || zone_w > 0.01f);
    }
    for (int i = 0; i < 3; ++i) g_cull_sun[i] = g_sh.sun[i];  // (the casters' search of the next frames: the level's sun is fixed)
    g_cull_tan[0] = 1.0f / g_sh.proj.x;
    g_cull_tan[1] = 1.0f / g_sh.proj.y;
    g_sun_known.store(true);
    tfrag_light_trace();
    const glm::vec3 up = std::fabs(g_sh.sun.z) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    const float tx = 1.0f / g_sh.proj.x, ty = 1.0f / g_sh.proj.y;
    float near_d = 0.1f;
    // the caches emptied when what they hold may have changed
    uint64_t key = static_cast<uint64_t>(lw(0x0015ed84)) * 1000003u + static_cast<uint64_t>(g_mrp_lod) * 7919u + static_cast<uint64_t>(g_sh.res);
    for (int i = 0; i < 3; ++i) {
        uint32_t u;
        std::memcpy(&u, &g_sh.sun[i], 4);
        key = key * 31u + u;
    }
    if (key != g_sh.cache_key || g_shadow_cache == 0) {
        g_sh.cache_key = key;
        for (auto& ca : g_sh.cache) ca.valid = false;
    }
    bool want_next = g_shadow_cache == 0;
    for (int k = 0; k < kCascades; ++k) {
        const float far_d = g_sh.splits[k];
        // the slice's corners in view space (z = -d), to the world
        glm::vec3 c(0.0f);
        glm::vec3 pts[8];
        int n = 0;
        for (float d : {near_d, far_d})
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sx = -1; sx <= 1; sx += 2) {
                    pts[n] = glm::vec3(g_sh.inv_view * glm::vec4(sx * d * tx, sy * d * ty, -d, 1.0f));
                    c += pts[n++];
                }
        c /= 8.0f;
        float r = 0.0f;
        for (const glm::vec3& p : pts) r = std::max(r, glm::length(p - c));
        r = std::ceil(r * 1.05f);  // (whole units: the box's size does not change with the rotation)
        const glm::mat4 lv0 = glm::lookAt(glm::vec3(0.0f), -g_sh.sun, up);
        const int cres = cascade_res(k);
        const glm::vec3 lc0 = glm::vec3(lv0 * glm::vec4(c, 1.0f));
        float R = r, back = 0.0f;
        glm::vec3 lc = lc0;
        g_sh.refresh[k] = true;
        if (g_shadow_cache != 0) {  // the box kept while the slice's sphere stays inside its own
            State::Cache& ca = g_sh.cache[k];
            R = std::ceil(r * kCacheGrow);
            const float slack = R - (glm::length(lc0 - ca.lc) + r);
            if (ca.valid && ca.res == cres && ca.R == R && slack >= 0.0f && !(ca.incomplete && mrp_world_casters_built())) {
                g_sh.refresh[k] = false;
                lc = ca.lc;
                back = ca.back;
            }
            // (the casters wanted next frame when the box may be left by then: within a few frames' moves of its edge)
            const float speed = ca.valid ? glm::length(lc0 - ca.prev) : R;
            ca.prev = lc0;
            want_next = want_next || !ca.valid || ca.incomplete || slack < 4.0f * speed + 0.02f * R;
        }
        const float texel = 2.0f * R / static_cast<float>(cres);
        if (g_sh.refresh[k]) {
            // the casters toward the sun: up to the cut plane (kBack from the camera along the sun, uShCut), which does not move when
            // the camera turns - the box reaches past it (a cached box: past it wherever the camera goes while the box is kept)
            back = R + kBack + glm::length(c - g_sh.cam) + (g_shadow_cache != 0 ? R : 0.0f);
            // the centre snapped to the texels, in the light's frame
            lc.x = std::floor(lc.x / texel) * texel;
            lc.y = std::floor(lc.y / texel) * texel;
            if (g_shadow_cache != 0) {
                State::Cache& ca = g_sh.cache[k];
                ca.valid = true;
                ca.lc = lc;
                ca.R = R;
                ca.back = back;
                ca.res = cres;
            }
        }
        const glm::mat4 lv = glm::translate(glm::mat4(1.0f), -lc) * lv0;  // (the light's frame centred on the slice)
        const glm::mat4 lp = glm::ortho(-R, R, -R, R, -back, R);
        Cascade& C = g_sh.casc[k];
        C.vp = lp * lv;
        const glm::mat4 bias = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
        const float part = static_cast<float>(cres) / static_cast<float>(g_sh.res);  // (the corner [0, part] of the layer)
        C.tex = bias * C.vp;
        C.part = part;
        C.res = cres;
        C.texel = texel;
        C.range = back + R;
        near_d = far_d * 0.9f;  // (the slices overlap a little: the blend band)
    }
    g_sh.want = want_next;
    if (!g_sh.logged) {
        g_sh.logged = true;
        if (!g_has_sun.load()) std::printf("[shadows] on: no sun (the level's key light comes from below or the horizon): no sun's shadows\n");
        else std::printf("[shadows] on: sun (%.3f %.3f %.3f), cascades %.0f / %.0f / %.0f units at %d px (texel %.3f / %.3f / %.3f)\n", g_sh.sun.x, g_sh.sun.y, g_sh.sun.z, g_sh.splits[0],
                    g_sh.splits[1], g_sh.splits[2], g_sh.res, g_sh.casc[0].texel, g_sh.casc[1].texel, g_sh.casc[2].texel);
    }
}
}  // namespace

namespace {
// orig: (host) the point lights per screen tile: each light's sphere (its bounding box's corners) projected by the view, the
// tiles of its screen rectangle given its bit (the whole screen when a corner is behind the camera); the fragment shader goes through
// its tile's lights alone (sh_points)
void point_tiles_update() {
    static std::vector<uint32_t> m(static_cast<size_t>(kTilesX) * kTilesY * 4);
    std::fill(m.begin(), m.end(), 0u);
    static const bool all = std::getenv("RAC1_POINT_TILES") != nullptr && std::atoi(std::getenv("RAC1_POINT_TILES")) == 0;  // (testing: every light everywhere)
    for (int i = 0; i < g_sh.npts && i < kMaxPts; ++i) {
        const glm::vec3 p(g_sh.pt_pos[i]);
        const float r = g_sh.pt_pos[i].w;
        const float wc = (g_sh.view_proj * glm::vec4(p, 1.0f)).w;
        if (!all && wc + r < 0.05f) continue;
        const glm::vec3 dcam = p - g_sh.cam;
        const bool inside_cam = all || glm::dot(dcam, dcam) <= (r + 0.15f) * (r + 0.15f);
        int tx0 = 0, tx1 = kTilesX - 1, ty0 = 0, ty1 = kTilesY - 1;
        if (!inside_cam) {
            constexpr float kNearW = 0.05f;
            glm::vec4 q[8];
            int nfront = 0;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (int c = 0; c < 8; ++c) {
                q[c] = g_sh.view_proj * glm::vec4(p + glm::vec3((c & 1) ? r : -r, (c & 2) ? r : -r, (c & 4) ? r : -r), 1.0f);
                if (q[c].w >= kNearW) {
                    ++nfront;
                    const float inv_w = 1.0f / q[c].w;
                    x0 = std::min(x0, q[c].x * inv_w);
                    x1 = std::max(x1, q[c].x * inv_w);
                    y0 = std::min(y0, q[c].y * inv_w);
                    y1 = std::max(y1, q[c].y * inv_w);
                }
            }
            if (nfront == 0) continue;
            if (nfront < 8) {
                // Clip the 12 AABB edges that straddle the w = kNearW plane
                for (int a = 0; a < 8; ++a) {
                    for (int bit = 1; bit <= 4; bit <<= 1) {
                        const int b = a ^ bit;
                        if (a < b && (q[a].w >= kNearW) != (q[b].w >= kNearW)) {
                            const float t = (kNearW - q[a].w) / (q[b].w - q[a].w);
                            const float cx = (q[a].x + t * (q[b].x - q[a].x)) * (1.0f / kNearW);
                            const float cy = (q[a].y + t * (q[b].y - q[a].y)) * (1.0f / kNearW);
                            x0 = std::min(x0, cx);
                            x1 = std::max(x1, cx);
                            y0 = std::min(y0, cy);
                            y1 = std::max(y1, cy);
                        }
                    }
                }
            }
            if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) continue;  // (off the screen)
            tx0 = std::clamp(static_cast<int>(std::floor((x0 * 0.5f + 0.5f) * kTilesX)), 0, kTilesX - 1);
            tx1 = std::clamp(static_cast<int>(std::floor((x1 * 0.5f + 0.5f) * kTilesX)), 0, kTilesX - 1);
            ty0 = std::clamp(static_cast<int>(std::floor((y0 * 0.5f + 0.5f) * kTilesY)), 0, kTilesY - 1);
            ty1 = std::clamp(static_cast<int>(std::floor((y1 * 0.5f + 0.5f) * kTilesY)), 0, kTilesY - 1);
        }
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) m[static_cast<size_t>(ty * kTilesX + tx) * 4 + (i >> 5)] |= 1u << (i & 31);
    }
    if (g_sh.tiles == 0) {
        glGenTextures(1, &g_sh.tiles);
        glBindTexture(GL_TEXTURE_2D, g_sh.tiles);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, kTilesX, kTilesY, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, g_sh.tiles);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTilesX, kTilesY, GL_RGBA_INTEGER, GL_UNSIGNED_INT, m.data());
}

// orig: (host) the point lights' shadows: the nearest lit ones (up to kPtShadows, within the cascades' reach) each drawn into a
// cube of depth maps (6 views of 90 degrees from the light, near kPtNear, far its radius) with the frame's casters, world and mobys;
// the light's pt_col.w names its cube (the fragment shader's sh_points compares against it). The shadow pass's framebuffer bound.
void point_shadows_pass() {
    if (g_point_shadows == 0 || g_modern_points == 0 || g_sh.npts == 0) return;
    if (g_sh.ptex == 0) {
        glGenTextures(1, &g_sh.ptex);
        glBindTexture(kGlCubeMapArray, g_sh.ptex);
        glTexImage3D(kGlCubeMapArray, 0, GL_DEPTH_COMPONENT32F, kPtRes, kPtRes, 6 * kPtShadows, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(kGlCubeMapArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(kGlCubeMapArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(kGlCubeMapArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(kGlCubeMapArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(kGlCubeMapArray, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(kGlCubeMapArray, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindTexture(kGlCubeMapArray, 0);
    }
    // the nearest to the camera first
    int order[kMaxPts];
    for (int i = 0; i < g_sh.npts; ++i) order[i] = i;
    std::sort(order, order + g_sh.npts, [](int a, int b) {
        return glm::length(glm::vec3(g_sh.pt_pos[a]) - g_sh.cam) - g_sh.pt_pos[a].w < glm::length(glm::vec3(g_sh.pt_pos[b]) - g_sh.cam) - g_sh.pt_pos[b].w;
    });
    // the faces as the cube map's sampling orders them (+x, -x, +y, -y, +z, -z: OpenGL's targets and up vectors)
    static const glm::vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 ups[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    // the cubes: the game's lights first (the frame's casters sorted for their slots), then the automatic emitters with a moby inside
    // their reach, the nearest to one (relative to the reach) first - their cube the mobys' alone: a merged emitter can sit inside its
    // lamp, the lamp's own shape would put it out -. An emitter's shadow fades in when it gets a cube and out when it loses it (it
    // keeps the cube while fading), so no shadow appears or goes at once.
    int pick[kPtShadows], npick = 0;
    for (int oi = 0; oi < g_sh.npts && npick < kPtShadows; ++oi)
        if (g_sh.pt_slot[order[oi]] >= 0 && glm::length(glm::vec3(g_sh.pt_pos[order[oi]]) - g_sh.cam) - g_sh.pt_pos[order[oi]].w <= g_sh.splits[1]) pick[npick++] = order[oi];
    const int game_cubes = npick;
    struct Fading {
        glm::vec3 pos{0.0f};
        float k = 0.0f;
    };
    static Fading fade[kPtShadows];
    static int nfade = 0;
    constexpr float kStep = 0.125f;  // (8 frames in, 8 out)
    if (g_auto_lights != 0) {
        // the wanted: the emitters with a moby part within their reach, by the part's distance over the reach
        std::pair<float, int> want[kMaxPts];
        int nwant = 0;
        for (int i = 0; i < g_sh.npts; ++i) {
            if (g_sh.pt_slot[i] >= 0) continue;
            const float p[3] = {g_sh.pt_pos[i].x, g_sh.pt_pos[i].y, g_sh.pt_pos[i].z};
            const float d = mrp_moby_nearest_part(p) / g_sh.pt_pos[i].w;
            if (d < 1.0f) want[nwant++] = {d, i};
        }
        std::sort(want, want + nwant);
        const int room = kPtShadows - game_cubes;
        nwant = std::min(nwant, room);
        auto find = [](const glm::vec3& q) {
            for (int i = 0; i < g_sh.npts; ++i)
                if (g_sh.pt_slot[i] < 0 && glm::vec3(g_sh.pt_pos[i]) == q) return i;
            return -1;
        };
        // the ones fading: up if still wanted, down if not (gone at 0, or when the light left the frame's list)
        int kept = 0;
        for (int j = 0; j < nfade; ++j) {
            bool wanted = false;
            for (int w = 0; w < nwant; ++w) wanted = wanted || glm::vec3(g_sh.pt_pos[want[w].second]) == fade[j].pos;
            fade[j].k = std::clamp(fade[j].k + (wanted ? kStep : -kStep), 0.0f, 1.0f);
            if (fade[j].k > 0.0f && find(fade[j].pos) >= 0) fade[kept++] = fade[j];
        }
        nfade = std::min(kept, room);  // (a game light taking a cube: the weakest dropped)
        for (int w = 0; w < nwant && nfade < room; ++w) {
            const glm::vec3 q(g_sh.pt_pos[want[w].second]);
            bool in = false;
            for (int j = 0; j < nfade; ++j) in = in || fade[j].pos == q;
            if (!in) fade[nfade++] = Fading{q, kStep};
        }
        for (int j = 0; j < nfade && npick < kPtShadows; ++j) pick[npick++] = find(fade[j].pos);
    } else {
        nfade = 0;
    }
    int cube = 0;
    for (int pi = 0; pi < npick; ++pi) {
        const int i = pick[pi];
        const glm::vec3 lp(g_sh.pt_pos[i]);
        const float r = g_sh.pt_pos[i].w;
        if (glm::length(lp - g_sh.cam) - r > g_sh.splits[1]) continue;  // (beyond the middle cascade: not worth a cube)
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, kPtNear, r);
        for (int f = 0; f < 6; ++f) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, g_sh.ptex, 0, 6 * cube + f);
            glViewport(0, 0, kPtRes, kPtRes);
            glDepthMask(GL_TRUE);
            glClear(GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            const glm::mat4 vp = proj * glm::lookAt(lp, lp + dirs[f], ups[f]);
            if (g_sh.pt_slot[i] >= 0) mrp_world_draw_casters(glm::value_ptr(vp), 3 + g_sh.pt_slot[i]);
            mrp_moby_draw_casters(glm::value_ptr(vp), glm::value_ptr(g_sh.pt_pos[i]));
        }
        float k = 1.0f;  // (an emitter's shadow fading: its weight with the cube's index, pt_col.w = cube + 0.05 + 0.9 k)
        for (int j = 0; j < nfade; ++j)
            if (g_sh.pt_slot[i] < 0 && fade[j].pos == lp) k = fade[j].k;
        g_sh.pt_col[i].w = static_cast<float>(cube) + 0.05f + 0.9f * k;
        ++cube;
    }
}
}  // namespace

// orig: (host) see mrp_lighting.h
bool mrp_lighting_active() { return g_modern_shadows != 0 && g_sh.on; }

// orig: (host) see mrp_lighting.h
namespace {
// the game's point lights' ages: a light lit for more than kOldLight frames in a row (Blarg's purple lamp, lit from the
// level's start) is the level's own - the game's vertex colours carry it, as on the PS2: it is neither lit per pixel nor holds the
// colours; a short one (an explosion, a shot) is lit per pixel, the colours held meanwhile
constexpr uint64_t kOldLight = 90;
std::atomic<uint32_t> g_slot_old{0};  // bit i: slot i's light is the level's own
uint32_t g_slot_id[8] = {};           // its use record's +0x10 (0: free)
uint64_t g_slot_since[8] = {}, g_freeze_calls = 0;
}  // namespace

bool mrp_lighting_points_freeze() {
    if (g_modern_shadows == 0 || g_modern_points == 0) return false;
    ++g_freeze_calls;
    bool young = false;
    uint32_t old = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t id = lwu(OV(0x001804c0u) + i * 0x30 + 0x10);
        if (id == 0) {
            g_slot_id[i] = 0;
            continue;
        }
        if (id != g_slot_id[i]) {
            g_slot_id[i] = id;
            g_slot_since[i] = g_freeze_calls;
        }
        if (g_freeze_calls - g_slot_since[i] >= kOldLight) old |= 1u << i;
        else young = true;
    }
    g_slot_old.store(old);
    return young;
}

// ---- ambient occlusion -----------------------------------------------------------------------------------------------
//
// Screen space, from the 3D's depth alone: the framebuffer's depth copied (a blit into a texture of its format), per pixel its world
// position (the world's view-projection inverted), its face normal (the positions' derivatives), 12 points of a hemisphere around it
// (0.9 units, rotated per pixel) projected: each one behind the depth stored there (within reach) occludes. The result (R8) blurred
// within the surface (5 x 5, the depths compared) and multiplied into the colour (RGB; the sky - depth 1 - untouched).
namespace {
const char* kAoVs = R"(#version 460 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID & 2) * 2.0 - 1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
const char* kAoFs = R"(#version 460 core
in vec2 vUv;
uniform sampler2D uDepth;
uniform mat4 uInvVP, uVP;
uniform vec3 uCam;
uniform vec4 uParam;  // radius (units), strength, bias, 0
out float FragAo;
vec3 world(vec2 uv, float d) {
    vec4 p = uInvVP * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}
void main() {
    float d = texture(uDepth, vUv).r;
    vec3 P = world(vUv, d);
    vec3 N = cross(dFdx(P), dFdy(P));
    if (d >= 1.0 || dot(N, N) < 1e-12) { FragAo = 1.0; return; }
    N = normalize(N);
    if (dot(N, uCam - P) < 0.0) N = -N;
    float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 t = normalize(abs(N.z) < 0.9 ? cross(N, vec3(0.0, 0.0, 1.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(N, t);
    vec3 T = t * cos(a) + b * sin(a), B = cross(N, T);
    float occ = 0.0;
    const int K = 12;
    for (int i = 0; i < K; ++i) {
        float fi = (float(i) + 0.5) / float(K);
        float z = sqrt(1.0 - fi);                 // (cosine-weighted)
        float r = sqrt(fi), ph = float(i) * 2.3999632;
        vec3 h = T * (r * cos(ph)) + B * (r * sin(ph)) + N * z;
        float s = mix(0.15, 1.0, fi * fi) * uParam.x;
        vec3 S = P + N * uParam.z + h * s;
        vec4 c = uVP * vec4(S, 1.0);
        if (c.w <= 0.0) continue;
        vec2 suv = c.xy / c.w * 0.5 + 0.5;
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
        float sd = texture(uDepth, suv).r;
        float zs = c.z / c.w * 0.5 + 0.5;
        if (sd < zs - 0.00002) {
            float dist = length(world(suv, sd) - S);
            occ += smoothstep(0.0, 1.0, uParam.x * 1.5 / max(dist, 1e-3) - 0.5);
        }
    }
    FragAo = 1.0 - uParam.y * occ / float(K);
}
)";
const char* kAoBlurFs = R"(#version 460 core
in vec2 vUv;
uniform sampler2D uAo, uDepth;
uniform vec2 uTexel;
out vec4 FragColor;
void main() {
    float d0 = texture(uDepth, vUv).r;
    if (d0 >= 1.0) discard;
    float sum = 0.0, w = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            vec2 uv = vUv + vec2(x, y) * uTexel;
            float d = texture(uDepth, uv).r;
            float k = abs(d - d0) < 0.0005 ? 1.0 : 0.0;
            sum += texture(uAo, uv).r * k;
            w += k;
        }
    float ao = w > 0.0 ? sum / w : 1.0;
    FragColor = vec4(vec3(ao), 1.0);
}
)";
GLuint ao_compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[ao] shader: %s\n", log);
    }
    return s;
}
GLuint ao_link(const char* fs) {
    const GLuint p = glCreateProgram();
    const GLuint v = ao_compile(GL_VERTEX_SHADER, kAoVs), f = ao_compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) std::fprintf(stderr, "[ao] link failed\n");
    return p;
}
struct Ao {
    GLuint prog = 0, blur = 0, vao = 0;
    GLuint dfbo = 0, dtex = 0, afbo = 0, atex = 0;
    int w = 0, h = 0;
    GLint last_fb = -1;
    GLenum dfmt = 0;
    GLint u_depth = -1, u_inv_vp = -1, u_vp = -1, u_cam = -1, u_param = -1;
    GLint u_b_depth = -1, u_b_ao = -1, u_b_texel = -1;
} g_ao_s;

// ---- the view covered from the sun -----------------------------------------------------------------------------------
//
// Our choice, not the game's: a room, a corridor, a ship's hold is lit by its zone's key light, which comes from above, but the
// roof stops it, so the sun's shadow covers the whole view and darkens it (Blarg's station, Orxon's base, Gaspar's halls). Each
// frame the 3D's depth is copied at 2 kProbeW x 2 kProbeH (a blit, nearest); per sample its world point and face normal (from its
// neighbours), tested against the cascades (one tap, a few texels toward the sun) when it faces the sun - a face turned away is
// not counted, as the receivers do not shadow it -, the sky as seeing it. Read back the next frame (a pixel buffer): the sun's
// weight is scaled by how much of the view sees the sun - none under kCoverLo, all over kCoverHi, the scale following it slowly
// (kCoverRate a frame, about a second), then faded as between zones. In a room, the original's look (the point lights and the
// occlusion on top); outdoors, where the ground in the sun or the sky shows, the shadows as before.
// Outdoors besides: where the camera is where the sky was in view (kSkySeen of the samples, any time since the level began),
// the sun is kept whatever the view shows. Outdoors a camera often looks at nothing but shaded ground (down at Novalis's
// start valley, all in the shade of its trees and of the plateau beside it): that view alone faded the sun out, the trees' and
// Ratchet's shadows went and came back as the camera turned. A room shows no sky. The places are the camera's positions, one
// every kSkyStep units, kSkyMax of them (the oldest dropped); within kSkyNear of one the sun is kept, beyond kSkyFar it is the
// view's measure alone.
constexpr int kProbeW = 64, kProbeH = 36;
constexpr float kCoverLo = 0.05f, kCoverHi = 0.25f, kCoverRate = 0.03f;
constexpr float kSkySeen = 0.05f, kSkyStep = 4.0f, kSkyNear = 12.0f, kSkyFar = 24.0f;
constexpr size_t kSkyMax = 1024;
const char* kProbeFs = R"(#version 460 core
uniform sampler2D uDepth;
uniform sampler2DArrayShadow uShMap;
uniform mat4 uShCasc[3];
uniform vec3 uShPart;
uniform vec3 uShTexel;
uniform vec3 uShSun;
uniform vec3 uCam;
uniform mat4 uInvVP;
out vec4 FragOut;  // r the sun seen, g counted, b the sky
vec3 world(ivec2 ip, float d) {
    vec2 uv = (vec2(ip) + 0.5) / vec2(textureSize(uDepth, 0));
    vec4 w = uInvVP * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return w.xyz / w.w;
}
void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy) * 2;
    float d = texelFetch(uDepth, ip, 0).r;
    if (d >= 1.0) { FragOut = vec4(1.0, 1.0, 1.0, 1.0); return; }
    float dx = texelFetch(uDepth, ip + ivec2(1, 0), 0).r, dy = texelFetch(uDepth, ip + ivec2(0, 1), 0).r;
    vec3 P = world(ip, d);
    if (dx >= 1.0 || dy >= 1.0) { FragOut = vec4(0.0); return; }
    vec3 N = cross(world(ip + ivec2(1, 0), dx) - P, world(ip + ivec2(0, 1), dy) - P);
    if (dot(N, N) < 1e-12) { FragOut = vec4(0.0); return; }
    N = normalize(N);
    if (dot(N, uCam - P) < 0.0) N = -N;
    if (dot(N, uShSun) < 0.1) { FragOut = vec4(0.0); return; }
    for (int k = 0; k < 3; ++k) {
        vec4 q = uShCasc[k] * vec4(P + N * (uShTexel[k] * 1.5) + uShSun * (uShTexel[k] * 2.0), 1.0);
        if (any(lessThan(q.xy, vec2(0.02))) || any(greaterThan(q.xy, vec2(0.98))) || q.z >= 1.0) continue;
        FragOut = vec4(texture(uShMap, vec4(q.xy * uShPart[k], float(k), q.z)), 1.0, 0.0, 1.0);
        return;
    }
    FragOut = vec4(1.0, 1.0, 0.0, 1.0);
}
)";
struct Probe {
    GLuint prog = 0, vao = 0, dfbo = 0, dtex = 0, ofbo = 0, otex = 0, pbo[2] = {0, 0};
    GLenum dfmt = 0;
    GLint last_fb = -1;
    bool filled[2] = {false, false};
    int next = 0;
    GLint u_depth = -1, u_map = -1, u_casc = -1, u_part = -1, u_texel = -1, u_sun = -1, u_cam = -1, u_inv_vp = -1;
    float open = 1.0f, sky = 1.0f;  // the last measure: the counted samples' share that see the sun, the samples' share of sky
    std::vector<glm::vec3> sky_at;  // where the camera was when the sky was in view (outdoors)
    float outdoors = 1.0f;          // the last measure: 1 near one of them .. 0 away from all
    float k = 1.0f;                 // the sun's weight's scale (smoothed)
    int32_t level = -1;
} g_probe;
}  // namespace

namespace {
// orig: (host) the view covered from the sun (see kProbeW): this frame's samples drawn and read into a pixel buffer, the last
// frame's read (its fence long passed: the map does not wait in practice; always the last frame's - the same picture every run)
void cover_probe(GLint fb, const int vp[4]) {
    static const int on = env_int("RAC1_SUN_COVER", 1);  // (testing: 0 off)
    static const bool trace = std::getenv("RAC1_SUN_COVER_TRACE") != nullptr;
    Probe& P = g_probe;
    const int32_t level = lw(0x0015ed84);
    if (level != P.level) {  // (a new level: nothing measured yet)
        P.level = level;
        P.k = 1.0f;
        P.open = P.sky = 1.0f;
        P.filled[0] = P.filled[1] = false;
        P.sky_at.clear();
    }
    if (on == 0 || !g_casc_on.load()) {
        P.k = 1.0f;
        P.filled[0] = P.filled[1] = false;
        return;
    }
    if (P.prog == 0) {
        P.prog = ao_link(kProbeFs);
        P.u_depth = glGetUniformLocation(P.prog, "uDepth");
        P.u_map = glGetUniformLocation(P.prog, "uShMap");
        P.u_casc = glGetUniformLocation(P.prog, "uShCasc");
        P.u_part = glGetUniformLocation(P.prog, "uShPart");
        P.u_texel = glGetUniformLocation(P.prog, "uShTexel");
        P.u_sun = glGetUniformLocation(P.prog, "uShSun");
        P.u_cam = glGetUniformLocation(P.prog, "uCam");
        P.u_inv_vp = glGetUniformLocation(P.prog, "uInvVP");
        glGenVertexArrays(1, &P.vao);
        glGenFramebuffers(1, &P.dfbo);
        glGenFramebuffers(1, &P.ofbo);
        glGenTextures(1, &P.dtex);
        glGenTextures(1, &P.otex);
        glGenBuffers(2, P.pbo);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, P.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, 4 * kProbeW * kProbeH, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, P.otex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kProbeW, kProbeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, P.ofbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, P.otex, 0);
    }
    // the last frame's measure
    const int prev = P.next ^ 1;
    if (P.filled[prev]) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, P.pbo[prev]);
        if (const auto* px = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * kProbeW * kProbeH, GL_MAP_READ_BIT))) {
            uint32_t lit = 0, counted = 0, sky = 0;
            for (int i = 0; i < kProbeW * kProbeH; ++i) {
                lit += px[4 * i];
                counted += px[4 * i + 1] >= 128 ? 1u : 0u;
                sky += px[4 * i + 2] >= 128 ? 1u : 0u;
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            P.open = counted > 0 ? static_cast<float>(lit) / (255.0f * static_cast<float>(counted)) : 1.0f;
            P.sky = static_cast<float>(sky) / static_cast<float>(kProbeW * kProbeH);
            static const int sky_on = env_int("RAC1_SUN_COVER_SKY", 1);  // (testing: 0 the view alone, as in 0.4.0)
            P.outdoors = 0.0f;
            if (sky_on != 0) {
                const glm::vec3 h = g_sh.cam;  // (the camera: Clank's or a vehicle's sections too)
                float dmin = 1e30f;
                for (const glm::vec3& q : P.sky_at) dmin = std::min(dmin, glm::length(q - h));
                if (P.sky >= kSkySeen && dmin > kSkyStep) {
                    if (P.sky_at.size() >= kSkyMax) P.sky_at.erase(P.sky_at.begin());
                    P.sky_at.push_back(h);
                    dmin = 0.0f;
                }
                P.outdoors = std::clamp((kSkyFar - dmin) / (kSkyFar - kSkyNear), 0.0f, 1.0f);
            }
            const float kv = std::clamp((P.open - kCoverLo) / (kCoverHi - kCoverLo), 0.0f, 1.0f);
            P.k += (std::max(kv, P.outdoors) - P.k) * kCoverRate;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        P.filled[prev] = false;
    }
    if (trace) {
        static uint64_t n = 0;
        if (n++ % 60 == 0)
            std::printf("[sun] cover probe: %.0f %% sees the sun, %.0f %% sky, outdoors %.2f (%zu places): the sun's weight x %.2f\n", 100.0f * P.open, 100.0f * P.sky,
                        P.outdoors, P.sky_at.size(), P.k);
    }
    // this frame's: the depth at 2 kProbeW x 2 kProbeH (a blit of the same format, nearest)
    GLenum fmt = P.dfmt;
    if (fb != P.last_fb || fmt == 0) {
        GLint dbits = 24, sbits = 8, dtype = GL_UNSIGNED_NORMALIZED;
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fb));
        const GLenum datt = fb == 0 ? GL_DEPTH : GL_DEPTH_ATTACHMENT, satt = fb == 0 ? GL_STENCIL : GL_STENCIL_ATTACHMENT;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, datt, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &dbits);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, datt, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &dtype);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, satt, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &sbits);
        while (glGetError() != GL_NO_ERROR) {}
        fmt = dtype == GL_FLOAT ? (sbits > 0 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F)
                                : (sbits > 0 ? GL_DEPTH24_STENCIL8 : (dbits >= 32 ? GL_DEPTH_COMPONENT32 : dbits >= 24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16));
        P.last_fb = fb;
    }
    const bool st = fmt == GL_DEPTH24_STENCIL8 || fmt == GL_DEPTH32F_STENCIL8;
    if (P.dfmt != fmt) {
        P.dfmt = fmt;
        glBindTexture(GL_TEXTURE_2D, P.dtex);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(fmt), 2 * kProbeW, 2 * kProbeH, 0, st ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                     fmt == GL_DEPTH24_STENCIL8 ? GL_UNSIGNED_INT_24_8 : fmt == GL_DEPTH32F_STENCIL8 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, P.dfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, st ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, P.dtex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(fb));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, P.dfbo);
    glBlitFramebuffer(vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3], 0, 0, 2 * kProbeW, 2 * kProbeH, GL_DEPTH_BUFFER_BIT | (st ? GL_STENCIL_BUFFER_BIT : 0), GL_NEAREST);
    // the samples against the cascades (the caller's state saved and restored around)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, P.ofbo);
    glViewport(0, 0, kProbeW, kProbeH);
    glUseProgram(P.prog);
    glBindVertexArray(P.vao);
    constexpr int kProbeUnit = 28;
    glActiveTexture(GL_TEXTURE0 + kProbeUnit);
    glBindTexture(GL_TEXTURE_2D, P.dtex);
    glActiveTexture(GL_TEXTURE0 + kUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_sh.tex);
    glUniform1i(P.u_depth, kProbeUnit);
    glUniform1i(P.u_map, kUnit);
    glm::mat4 m[kCascades];
    for (int k = 0; k < kCascades; ++k) m[k] = g_sh.casc[k].tex;
    glUniformMatrix4fv(P.u_casc, kCascades, GL_FALSE, glm::value_ptr(m[0]));
    glUniform3f(P.u_part, g_sh.casc[0].part, g_sh.casc[1].part, g_sh.casc[2].part);
    glUniform3f(P.u_texel, g_sh.casc[0].texel, g_sh.casc[1].texel, g_sh.casc[2].texel);
    glUniform3f(P.u_sun, g_sh.sun.x, g_sh.sun.y, g_sh.sun.z);
    glUniform3f(P.u_cam, g_sh.cam.x, g_sh.cam.y, g_sh.cam.z);
    float vpf[16];
    mrp_world_view_proj(vpf);
    const glm::mat4 IVP = glm::inverse(glm::make_mat4(vpf));
    glUniformMatrix4fv(P.u_inv_vp, 1, GL_FALSE, glm::value_ptr(IVP));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, P.ofbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, P.pbo[P.next]);
    GLint align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kProbeW, kProbeH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    P.filled[P.next] = true;
    P.next ^= 1;
}
// orig: (host) how much of the sun's weight the view keeps (a room lit by a key light from above that its roof stops: no sky,
// nearly no sample sees the sun - the sun's shadows faded out there; our choice, see kProbeW)
float view_open() { return g_probe.k; }
}  // namespace

// orig: (host) see mrp_lighting.h
void mrp_lighting_ao_pass() {
    if (!g_sh.on) return;
    int vp[4];
    if (!mrp_world_last_viewport(vp)) return;
    {  // the view covered from the sun (the samples' pass, its own state restored)
        GLint fb = 0, rfb = 0, prev_prog = 0, prev_vao = 0, prev_vp[4] = {}, prev_active = 0, prev_t0 = 0, prev_t1 = 0, prev_rb = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        glGetIntegerv(GL_VIEWPORT, prev_vp);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        glGetIntegerv(GL_READ_BUFFER, &prev_rb);
        glActiveTexture(GL_TEXTURE0 + 28);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_t0);
        glActiveTexture(GL_TEXTURE0 + kUnit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev_t1);
        const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);
        GLboolean cmask[4];
        glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        cover_probe(fb, vp);
        glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
        if (blend) glEnable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (cull) glEnable(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0 + 28);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_t0));
        glActiveTexture(GL_TEXTURE0 + kUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(prev_t1));
        glActiveTexture(static_cast<GLenum>(prev_active));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(fb));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(rfb));
        glReadBuffer(static_cast<GLenum>(prev_rb));
        glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
        glUseProgram(static_cast<GLuint>(prev_prog));
        glBindVertexArray(static_cast<GLuint>(prev_vao));
    }
    if (g_ao == 0) return;
    Ao& A = g_ao_s;
    GLint fb = 0, rfb = 0, prev_prog = 0, prev_vao = 0, prev_vp[4] = {}, prev_tex = 0, prev_active = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    const int w = vp[2], h = vp[3];
    if (A.prog == 0) {
        A.prog = ao_link(kAoFs);
        A.blur = ao_link(kAoBlurFs);
        A.u_depth = glGetUniformLocation(A.prog, "uDepth");
        A.u_inv_vp = glGetUniformLocation(A.prog, "uInvVP");
        A.u_vp = glGetUniformLocation(A.prog, "uVP");
        A.u_cam = glGetUniformLocation(A.prog, "uCam");
        A.u_param = glGetUniformLocation(A.prog, "uParam");
        A.u_b_depth = glGetUniformLocation(A.blur, "uDepth");
        A.u_b_ao = glGetUniformLocation(A.blur, "uAo");
        A.u_b_texel = glGetUniformLocation(A.blur, "uTexel");
        glGenVertexArrays(1, &A.vao);
        glGenFramebuffers(1, &A.dfbo);
        glGenFramebuffers(1, &A.afbo);
        glGenTextures(1, &A.dtex);
        glGenTextures(1, &A.atex);
    }
    GLenum fmt = A.dfmt;
    if (fb != A.last_fb || fmt == 0) {
        // the depth format of the framebuffer drawn into (the blit wants the same)
        GLint dbits = 24, sbits = 8, dtype = GL_UNSIGNED_NORMALIZED;
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fb));
        const GLenum datt = fb == 0 ? GL_DEPTH : GL_DEPTH_ATTACHMENT, satt = fb == 0 ? GL_STENCIL : GL_STENCIL_ATTACHMENT;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, datt, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &dbits);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, datt, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &dtype);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, satt, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &sbits);
        while (glGetError() != GL_NO_ERROR) {}
        fmt = dtype == GL_FLOAT ? (sbits > 0 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F)
                                : (sbits > 0 ? GL_DEPTH24_STENCIL8 : (dbits >= 32 ? GL_DEPTH_COMPONENT32 : dbits >= 24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16));
        A.last_fb = fb;
    }
    if (A.w != w || A.h != h || A.dfmt != fmt) {
        A.w = w;
        A.h = h;
        A.dfmt = fmt;
        glBindTexture(GL_TEXTURE_2D, A.dtex);
        const bool st = fmt == GL_DEPTH24_STENCIL8 || fmt == GL_DEPTH32F_STENCIL8;
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(fmt), w, h, 0, st ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                     fmt == GL_DEPTH24_STENCIL8 ? GL_UNSIGNED_INT_24_8 : fmt == GL_DEPTH32F_STENCIL8 ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, A.dfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, st ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, A.dtex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glBindTexture(GL_TEXTURE_2D, A.atex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, A.afbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, A.atex, 0);
    }
    // the depth: the 3D's viewport of the framebuffer into the texture
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(fb));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, A.dfbo);
    const bool st = fmt == GL_DEPTH24_STENCIL8 || fmt == GL_DEPTH32F_STENCIL8;
    glBlitFramebuffer(vp[0], vp[1], vp[0] + w, vp[1] + h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | (st ? GL_STENCIL_BUFFER_BIT : 0), GL_NEAREST);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);
    GLint bsrc = 0, bdst = 0, bsrca = 0, bdsta = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrca);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdsta);
    GLboolean cmask[4], dmask = GL_TRUE;
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glBindVertexArray(A.vao);
    // the occlusion
    float vpf[16];
    mrp_world_view_proj(vpf);
    const glm::mat4 VP = glm::make_mat4(vpf), IVP = glm::inverse(VP);
    static const float radius = static_cast<float>(env_int("RAC1_AO_RADIUS", 90)) / 100.0f, strength = static_cast<float>(env_int("RAC1_AO_STRENGTH", 70)) / 100.0f;  // (testing)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, A.afbo);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(A.prog);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, A.dtex);
    glUniform1i(A.u_depth, 0);
    glUniformMatrix4fv(A.u_inv_vp, 1, GL_FALSE, glm::value_ptr(IVP));
    glUniformMatrix4fv(A.u_vp, 1, GL_FALSE, glm::value_ptr(VP));
    glUniform3f(A.u_cam, g_sh.cam.x, g_sh.cam.y, g_sh.cam.z);
    glUniform4f(A.u_param, radius, strength, 0.02f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // blurred, multiplied into the colour
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(fb));
    glViewport(vp[0], vp[1], w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);
    glUseProgram(A.blur);
    glActiveTexture(GL_TEXTURE1);
    GLint prev_tex1 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex1);
    glBindTexture(GL_TEXTURE_2D, A.atex);
    glUniform1i(A.u_b_depth, 0);
    glUniform1i(A.u_b_ao, 1);
    glUniform2f(A.u_b_texel, 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    glActiveTexture(static_cast<GLenum>(prev_active));
    // the state as it was
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
    glBlendFuncSeparate(static_cast<GLenum>(bsrc), static_cast<GLenum>(bdst), static_cast<GLenum>(bsrca), static_cast<GLenum>(bdsta));
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(fb));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(rfb));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glUseProgram(static_cast<GLuint>(prev_prog));
    glBindVertexArray(static_cast<GLuint>(prev_vao));
}

// ---- (step 3) the lights placed by hand ------------------------------------------------------------------------------

namespace {
std::vector<UserLight> g_user;
std::vector<int> g_user_noauto;  // the tie classes whose automatic emitters are off
std::vector<std::string> g_user_notes;  // the file's own comment lines (why a class is off), kept when the editor saves it
int g_user_level = -1;
std::string g_user_path;
// orig: (host) the level's lights file
std::string user_path(int level) {
    const char* dir = std::getenv("RAC1_LIGHTS_DIR");
    char name[32];
    std::snprintf(name, sizeof name, "level%02d.txt", level);
    return std::string(dir != nullptr ? dir : "data/lights") + "/" + name;
}
// orig: (host) read: one light a line, "light x y z r g b intensity radius flicker on" ('#' comments)
void user_load(int level) {
    g_user.clear();
    g_user_noauto.clear();
    g_user_notes.clear();
    g_user_level = level;
    g_user_path = user_path(level);
    g_user_light_selected = -1;
    FILE* f = std::fopen(g_user_path.c_str(), "r");
    if (f == nullptr) return;
    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        UserLight l;
        int on = 1, cls = 0;
        if (line[0] == '#' && std::strncmp(line, "# rac1-native: lights placed by hand", 36) != 0 && std::strncmp(line, "# light x y z", 13) != 0) {
            std::string n(line);
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            g_user_notes.push_back(n);
            continue;
        }
        if (std::sscanf(line, "noauto %d", &cls) == 1) {
            g_user_noauto.push_back(cls);
            continue;
        }
        if (std::sscanf(line, "light %f %f %f %f %f %f %f %f %f %d", &l.pos[0], &l.pos[1], &l.pos[2], &l.col[0], &l.col[1], &l.col[2], &l.intensity, &l.radius,
                        &l.flicker, &on) >= 9) {
            l.on = on != 0;
            g_user.push_back(l);
        }
    }
    std::fclose(f);
    std::printf("[lights] level %d: %zu lights placed by hand (%s)\n", level, g_user.size(), g_user_path.c_str());
}
}  // namespace

// orig: (host) see mrp_lighting.h
std::vector<UserLight>& mrp_user_lights() { return g_user; }
// orig: (host) see mrp_lighting.h
bool mrp_user_lights_class_off(int o_class) { return std::find(g_user_noauto.begin(), g_user_noauto.end(), o_class) != g_user_noauto.end(); }
// orig: (host) see mrp_lighting.h
void mrp_user_lights_class_set_off(int o_class, bool off) {
    const auto it = std::find(g_user_noauto.begin(), g_user_noauto.end(), o_class);
    if (off && it == g_user_noauto.end()) g_user_noauto.push_back(o_class);
    if (!off && it != g_user_noauto.end()) g_user_noauto.erase(it);
}
// orig: (host) see mrp_lighting.h
int mrp_user_lights_level() { return g_user_level; }
// orig: (host) see mrp_lighting.h
const char* mrp_user_lights_path() { return g_user_path.c_str(); }
// orig: (host) see mrp_lighting.h
void mrp_user_lights_reload() {
    if (g_user_level >= 0) user_load(g_user_level);
}
// orig: (host) see mrp_lighting.h
bool mrp_user_lights_save() {
    if (g_user_level < 0) return false;
    std::error_code ec;
    const std::filesystem::path p(g_user_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    FILE* f = std::fopen(g_user_path.c_str(), "w");
    if (f == nullptr) return false;
    std::fprintf(f, "# rac1-native: lights placed by hand for level %d (the debug menu's Lights tab)\n", g_user_level);
    std::fprintf(f, "# light x y z  r g b  intensity radius flicker on\n");
    for (const std::string& n : g_user_notes) std::fprintf(f, "%s\n", n.c_str());
    for (int c : g_user_noauto) std::fprintf(f, "noauto %d\n", c);
    for (const UserLight& l : g_user)
        std::fprintf(f, "light %.3f %.3f %.3f  %.3f %.3f %.3f  %.3f %.3f %.3f %d\n", l.pos[0], l.pos[1], l.pos[2], l.col[0], l.col[1], l.col[2], l.intensity, l.radius,
                     l.flicker, l.on ? 1 : 0);
    std::fclose(f);
    return true;
}

// orig: (host) see mrp_lighting.h
bool mrp_lighting_casters_wanted() { return g_casc_on.load() && (g_shadow_cache == 0 || g_sh.want.load()); }

// orig: (host) see mrp_lighting.h
float mrp_lighting_caster_grow() { return g_shadow_cache != 0 ? kCacheGrow : 1.0f; }

// orig: (host) see mrp_lighting.h
void mrp_lighting_point_casters(float pts[8][4]) {
    for (uint32_t i = 0; i < 8; ++i) {
        pts[i][0] = pts[i][1] = pts[i][2] = pts[i][3] = 0.0f;
        if (g_modern_shadows == 0 || g_modern_points == 0 || g_point_shadows == 0) continue;
        const uint32_t rec = OV(0x001802c0u) + i * 0x20, use = OV(0x001804c0u) + i * 0x30;
        if (lw(use + 0x10) == 0 || !(lwc1(rec + 0x1c) > 0.0f) || ((g_slot_old.load() >> i) & 1u) != 0) continue;
        pts[i][0] = lwc1(rec + 0x10);
        pts[i][1] = lwc1(rec + 0x14);
        pts[i][2] = lwc1(rec + 0x18);
        pts[i][3] = lwc1(rec + 0x1c);
    }
}

// orig: (host) see mrp_lighting.h
void mrp_lighting_frame_begin() {
    g_sh.on = false;
    if (std::getenv("RAC1_POINT_TRACE") != nullptr) {  // (testing: the game's 8 dynamic point lights, 0x1802c0 / 0x1804c0, every 20 draws)
        static uint64_t n = 0;
        if (n++ % 20 == 0) {
            for (uint32_t i = 0; i < 8; ++i) {
                const uint32_t rec = OV(0x001802c0u) + i * 0x20, use = OV(0x001804c0u) + i * 0x30;
                if (lw(use + 0x10) == 0) continue;
                std::printf("[point] draw %llu slot %u colour %.2f %.2f %.2f k %.2f pos %.1f %.1f %.1f r %.1f\n", static_cast<unsigned long long>(n), i, lwc1(rec), lwc1(rec + 4),
                            lwc1(rec + 8), lwc1(rec + 0xc), lwc1(rec + 0x10), lwc1(rec + 0x14), lwc1(rec + 0x18), lwc1(rec + 0x1c));
            }
        }
    }
}


// orig: (host) see mrp_lighting.h
bool mrp_lighting_cull_params(float sun[3], float splits[3], float* back, float* tan_x, float* tan_y) {
    if (g_modern_shadows == 0 || !g_sun_known.load()) return false;
    if (tan_x != nullptr) *tan_x = g_cull_tan[0];  // (the last drawn frame's projection)
    if (tan_y != nullptr) *tan_y = g_cull_tan[1];
    for (int i = 0; i < 3; ++i) {
        sun[i] = g_cull_sun[i];
        splits[i] = g_sh.splits[i];
    }
    *back = kBack;
    return true;
}

// orig: (host) see mrp_lighting.h
void mrp_lighting_shadow_pass() {
    g_sh.on = false;
    if (g_modern_shadows == 0 || !mrp_world_active() || !mrp_moby_active() || g_mrp_world_projection != 0) return;
    static bool splits_read = false;
    if (!splits_read) {
        read_splits();
        splits_read = true;
    }
    static const bool prof = std::getenv("RAC1_SHADOW_PROF") != nullptr;  // (testing: the pass's GPU time, cascades and point cubes)
    static GLuint q[2] = {};
    static double acc[2] = {};
    static int frames = 0, cubes_acc = 0;
    if (prof && q[0] == 0) glGenQueries(2, q);
    if (prof) glBeginQuery(GL_TIME_ELAPSED, q[0]);
    ensure_resources();
    fit_cascades();
    // the game's dynamic point lights in use (0x1804c0 + 0x10) with a colour and a radius: lit per pixel this frame
    g_sh.npts = 0;
    for (uint32_t i = 0; i < 8 && g_modern_points != 0; ++i) {
        const uint32_t rec = OV(0x001802c0u) + i * 0x20, use = OV(0x001804c0u) + i * 0x30;
        if (lw(use + 0x10) == 0 || ((g_slot_old.load() >> i) & 1u) != 0) continue;  // (the level's own lights: the game's)
        const glm::vec3 col(lwc1(rec), lwc1(rec + 4), lwc1(rec + 8));
        const float r = lwc1(rec + 0x1c);
        if (!(r > 0.0f) || col.x + col.y + col.z <= 0.0f) continue;
        g_sh.pt_pos[g_sh.npts] = glm::vec4(lwc1(rec + 0x10), lwc1(rec + 0x14), lwc1(rec + 0x18), r);
        g_sh.pt_col[g_sh.npts] = glm::vec4(col, -1.0f);
        g_sh.pt_slot[g_sh.npts] = static_cast<int>(i);
        ++g_sh.npts;
    }
    if (g_modern_points != 0 && g_auto_lights != 0) {  // (step 3) the lights placed by hand, before the emitters
        const int level = static_cast<int>(lw(0x0015ed84));
        if (level != g_user_level) user_load(level);
        const double t = static_cast<double>(g_sh.frame) / 60.0;
        for (size_t i = 0; i < g_user.size() && g_sh.npts < kMaxPts; ++i) {
            const UserLight& l = g_user[i];
            if (!l.on || !(l.radius > 0.0f)) continue;
            const glm::vec3 p(l.pos[0], l.pos[1], l.pos[2]);
            if (glm::length(p - g_sh.cam) - l.radius > g_sh.splits[1]) continue;
            float k = l.intensity;
            if (l.flicker > 0.0f) {  // (a flame's: two slow waves and a fast one, per light)
                const double ph = static_cast<double>(i) * 1.7;
                const double n = 0.5 * std::sin(t * 7.3 + ph) + 0.3 * std::sin(t * 13.1 + ph * 2.0) + 0.2 * std::sin(t * 29.7 + ph * 3.0);
                k *= 1.0f - l.flicker * 0.35f * static_cast<float>(1.0 + n);
            }
            if (static_cast<int>(i) == g_user_light_selected) k *= 0.6f + 0.4f * static_cast<float>(std::sin(t * 8.0));  // (the debug menu's: pulsing)
            g_sh.pt_pos[g_sh.npts] = glm::vec4(p, l.radius);
            g_sh.pt_col[g_sh.npts] = glm::vec4(l.col[0] * k, l.col[1] * k, l.col[2] * k, -1.0f);
            g_sh.pt_slot[g_sh.npts] = -1;
            ++g_sh.npts;
        }
    }
    if (g_modern_points != 0 && g_auto_lights != 0) {  // the automatic emitters near the camera, the nearest first
        float pr[4 * kMaxPts], pc[4 * kMaxPts];
        const float cam[3] = {g_sh.cam.x, g_sh.cam.y, g_sh.cam.z};
        const int n = mrp_world_emitters_near(cam, g_sh.splits[1], kMaxPts - g_sh.npts, pr, pc);
        for (int k = 0; k < n; ++k, ++g_sh.npts) {
            g_sh.pt_pos[g_sh.npts] = glm::vec4(pr[4 * k], pr[4 * k + 1], pr[4 * k + 2], pr[4 * k + 3]);
            g_sh.pt_col[g_sh.npts] = glm::vec4(pc[4 * k], pc[4 * k + 1], pc[4 * k + 2], -1.0f);
            g_sh.pt_slot[g_sh.npts] = -1;
        }
    }
    // (the read and draw framebuffers restored apart: the effects copy the screen from the read one - glCopyTexSubImage2D, gs_draw's
    // screen copies -; left at the draw one, an explosion's heat haze sampled a stale picture: a trail when the camera moved)
    GLint prev_fbo = 0, prev_read = 0, prev_vp[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    if (std::getenv("RAC1_SHADOW_FBO_TRACE") != nullptr) {  // (testing)
        static int shown = 0;
        if (prev_read != prev_fbo && shown++ < 5) std::printf("[shadows] the read framebuffer %d is not the draw one %d\n", prev_read, prev_fbo);
    }
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), clamp = glIsEnabled(GL_DEPTH_CLAMP);
    glBindFramebuffer(GL_FRAMEBUFFER, g_sh.fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_CLAMP);  // (the cut plane bounds every caster alike, see kBack)
    glEnable(GL_CLIP_DISTANCE0);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    static GLuint cq[kCascades] = {};
    static double cacc[kCascades] = {};
    static int cframes = 0;
    if (prof && cq[0] == 0) glGenQueries(kCascades, cq);
    if (mrp_world_casters_built()) ++g_sh.built;
    for (int k = 0; k < kCascades; ++k) {
        if (prof) {
            if (k == 0) glEndQuery(GL_TIME_ELAPSED);  // (the "cascades" query: up to here, the frame's world upload and dispatch)
            glBeginQuery(GL_TIME_ELAPSED, cq[k]);
        }
        if (!g_casc_on.load()) {  // (no sun, no cascades drawn - the receivers look at none)
            if (prof) glEndQuery(GL_TIME_ELAPSED);
            continue;
        }
        const int cres = g_sh.casc[k].res;
        const int* moby_rect = nullptr;
        const float within_v[4] = {g_sh.cam.x, g_sh.cam.y, g_sh.cam.z, g_shadow_moby_dist};  // (the shadows' distance)
        const float* within = g_shadow_moby_dist > 0.0f ? within_v : nullptr;
        static const int skip = std::getenv("RAC1_SHADOW_SKIP") != nullptr ? std::atoi(std::getenv("RAC1_SHADOW_SKIP")) : 0;  // (testing: 1 no world, 2 no mobys)
        auto begin_map = [&](bool layer) {  // (a depth target cleared: the cascade's layer, or its cache's map)
            if (layer) glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, g_sh.tex, 0, k);
            else glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, g_sh.cache[k].stex, 0);
            glViewport(0, 0, cres, cres);
            glDepthMask(GL_TRUE);
            glClearDepth(1.0);
            glEnable(GL_SCISSOR_TEST);  // (its square alone)
            glScissor(0, 0, cres, cres);
            glClear(GL_DEPTH_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        };
        if (g_shadow_cache != 0) {  // the static world from the cache (drawn into it when the box moved), the mobys over it
            State::Cache& ca = g_sh.cache[k];
            if (ca.stex == 0 || ca.stex_res != cres) {
                if (ca.stex == 0) glGenTextures(1, &ca.stex);
                glBindTexture(GL_TEXTURE_2D, ca.stex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, cres, cres, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                ca.stex_res = cres;
                g_sh.refresh[k] = true;  // (a new map: drawn this frame - the box is this frame's fit, see fit_cascades)
            }
            if (g_sh.refresh[k]) {
                ca.incomplete = !mrp_world_casters_built();
                if (ca.incomplete) g_sh.want = true;
                begin_map(false);
                if ((skip & 1) == 0) mrp_world_draw_casters(glm::value_ptr(g_sh.casc[k].vp), k);
                glDisable(GL_DEPTH_CLAMP);
                ++g_sh.refreshes;
            }
            ++g_sh.frames_cached;
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, g_sh.tex, 0, k);
            // the layer: the cache copied whole when drawn, else only where last frame's mobys were (the rest is the cache still); the
            // mobys then drawn inside their own rectangle (their parts' spheres), which the next frame puts back
            static const bool whole = std::getenv("RAC1_SHADOW_COPY_WHOLE") != nullptr;  // (testing: the whole cache copied every frame)
            int cx = 0, cy = 0, cw = cres, ch = cres;
            if (!g_sh.refresh[k] && !whole) {
                cx = ca.dirty[0];
                cy = ca.dirty[1];
                cw = ca.dirty[2] - ca.dirty[0];
                ch = ca.dirty[3] - ca.dirty[1];
            }
            int nr[4] = {0, 0, 0, 0};
            float rect[4];
            if ((skip & 2) == 0 && mrp_moby_caster_rect(glm::value_ptr(g_sh.casc[k].vp), rect, within)) {
                auto tx = [&](float v, bool up) {
                    const float t = (v * 0.5f + 0.5f) * static_cast<float>(cres);
                    return std::clamp(static_cast<int>(up ? std::ceil(t) : std::floor(t)) + (up ? 2 : -2), 0, cres);
                };
                nr[0] = tx(rect[0], false);
                nr[1] = tx(rect[1], false);
                nr[2] = tx(rect[2], true);
                nr[3] = tx(rect[3], true);
            }
            std::memcpy(ca.dirty, nr, sizeof nr);
            if (!whole) moby_rect = ca.dirty;
            using PfnCopy = void(APIENTRYP)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
            static const PfnCopy copy = reinterpret_cast<PfnCopy>(glfwGetProcAddress("glCopyImageSubData"));  // (GL 4.3: not in the loader's header)
            if (copy != nullptr) {
                if (cw > 0 && ch > 0) copy(ca.stex, GL_TEXTURE_2D, 0, cx, cy, 0, g_sh.tex, GL_TEXTURE_2D_ARRAY, 0, cx, cy, k, cw, ch, 1);
            } else {  // (no copy: the cache off from now on, this frame's world drawn as before)
                g_shadow_cache = 0;
                begin_map(true);
                if ((skip & 1) == 0) mrp_world_draw_casters(glm::value_ptr(g_sh.casc[k].vp), k);
                glDisable(GL_DEPTH_CLAMP);
            }
            glViewport(0, 0, cres, cres);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        } else {
            begin_map(true);
            if ((skip & 1) == 0) mrp_world_draw_casters(glm::value_ptr(g_sh.casc[k].vp), k);
            glDisable(GL_DEPTH_CLAMP);
        }
        if (moby_rect != nullptr) {  // (inside the rectangle the next frame puts back)
            glEnable(GL_SCISSOR_TEST);
            glScissor(moby_rect[0], moby_rect[1], moby_rect[2] - moby_rect[0], moby_rect[3] - moby_rect[1]);
        }
        if ((skip & 2) == 0 && (moby_rect == nullptr || (moby_rect[2] > moby_rect[0] && moby_rect[3] > moby_rect[1])))
            mrp_moby_draw_casters(glm::value_ptr(g_sh.casc[k].vp), nullptr, within);
        glDisable(GL_DEPTH_CLAMP);
        glDisable(GL_SCISSOR_TEST);
        if (prof) glEndQuery(GL_TIME_ELAPSED);
    }
    if (prof) {
        for (int k = 0; k < kCascades; ++k) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(cq[k], GL_QUERY_RESULT, &ns);
            cacc[k] += static_cast<double>(ns) * 1e-6;
        }
        if (++cframes == 60) {
            std::printf("[shadows] GPU ms/frame per cascade: %.3f %.3f %.3f; the static cache drawn %llu times in %llu cascade frames, the casters outside the view "
                        "made in %llu of 60 frames\n", cacc[0] / 60, cacc[1] / 60, cacc[2] / 60, static_cast<unsigned long long>(g_sh.refreshes),
                        static_cast<unsigned long long>(g_sh.frames_cached), static_cast<unsigned long long>(g_sh.built));
            g_sh.refreshes = g_sh.frames_cached = g_sh.built = 0;
            cacc[0] = cacc[1] = cacc[2] = 0;
            cframes = 0;
        }
        glBeginQuery(GL_TIME_ELAPSED, q[0]);  // (closed below: the rest of the pass)
    }
    if (const char* ref = std::getenv("RAC1_SHADOW_REF")) {  // (testing: a world-fixed box's depth from the sun each frame, shadow_ref_<n>.pgm:
        // with the hero and the camera's target still, it changes only where the casters' set depends on the camera)
        static GLuint rtex = 0, rfbo = 0;
        static int n = 0;
        float x = 0, y = 0, z = 0, r = 25;
        int every = 1;
        if (std::sscanf(ref, "%f,%f,%f,%f,%d", &x, &y, &z, &r, &every) >= 3 && ++n % std::max(1, every) == 0) {
            constexpr int R = 1024;
            if (rtex == 0) {
                glGenTextures(1, &rtex);
                glBindTexture(GL_TEXTURE_2D, rtex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, R, R, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                glGenFramebuffers(1, &rfbo);
                glBindFramebuffer(GL_FRAMEBUFFER, rfbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rtex, 0);
                glDrawBuffer(GL_NONE);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, rfbo);
            glViewport(0, 0, R, R);
            glClear(GL_DEPTH_BUFFER_BIT);
            const glm::vec3 c(x, y, z), up = std::fabs(g_sh.sun.z) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
            const glm::mat4 vp = glm::ortho(-r, r, -r, r, -(r + kBack + glm::length(c - g_sh.cam)), r) * glm::lookAt(c, c - g_sh.sun, up);
            mrp_world_draw_casters(glm::value_ptr(vp), 0);
            glDisable(GL_DEPTH_CLAMP);
            if (std::getenv("RAC1_SHADOW_REF_NOMOBY") == nullptr) mrp_moby_draw_casters(glm::value_ptr(vp));
            glDisable(GL_DEPTH_CLAMP);
            std::vector<float> px(static_cast<size_t>(R) * R);
            glBindTexture(GL_TEXTURE_2D, rtex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, px.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            char fn[64];
            std::snprintf(fn, sizeof fn, "shadow_ref_%d.pgm", n);
            if (FILE* f = std::fopen(fn, "wb")) {
                std::fprintf(f, "P5 %d %d 255\n", R, R);
                for (int yy = R - 1; yy >= 0; --yy)
                    for (int xx = 0; xx < R; ++xx) std::fputc(static_cast<int>(std::clamp(px[static_cast<size_t>(yy) * R + xx], 0.0f, 1.0f) * 255.0f), f);
                std::fclose(f);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, g_sh.fbo);
        }
    }
    glDisable(GL_CLIP_DISTANCE0);
    if (prof) {
        glEndQuery(GL_TIME_ELAPSED);
        glBeginQuery(GL_TIME_ELAPSED, q[1]);
    }
    point_shadows_pass();
    if (prof) {
        glEndQuery(GL_TIME_ELAPSED);
        for (int k = 0; k < 2; ++k) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(q[k], GL_QUERY_RESULT, &ns);
            acc[k] += static_cast<double>(ns) * 1e-6;
        }
        for (int i = 0; i < g_sh.npts; ++i) cubes_acc += g_sh.pt_col[i].w >= 0.0f ? 1 : 0;
        if (++frames == 60) {
            std::printf("[shadows] GPU ms/frame: cascades %.3f, point cubes %.3f (%.2f cubes a frame)\n", acc[0] / frames, acc[1] / frames, static_cast<double>(cubes_acc) / frames);
            acc[0] = acc[1] = 0;
            frames = cubes_acc = 0;
        }
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    if (const char* dump = std::getenv("RAC1_SHADOW_DUMP")) {  // (testing: the cascades' depth at that draw, shadow_casc<k>.pgm)
        static int n = 0;
        if (++n % std::max(1, std::atoi(dump)) == 0) {
            std::vector<float> all(static_cast<size_t>(g_sh.res) * g_sh.res * kCascades);
            glBindTexture(GL_TEXTURE_2D_ARRAY, g_sh.tex);
            glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, all.data());
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            {  // (testing: where cascade 0's nearest texels are in the world, and the camera)
                const float* px = all.data();
                const glm::mat4 inv = glm::inverse(g_sh.casc[0].vp);
                std::printf("[shadows] dump %d: camera %.1f %.1f %.1f\n", n, g_sh.cam.x, g_sh.cam.y, g_sh.cam.z);
                for (int gy = 0; gy < 8; ++gy)
                    for (int gx = 0; gx < 8; ++gx) {
                        float best = 2.0f;
                        int bx = 0, by = 0;
                        for (int y = gy * g_sh.res / 8; y < (gy + 1) * g_sh.res / 8; y += 4)
                            for (int x = gx * g_sh.res / 8; x < (gx + 1) * g_sh.res / 8; x += 4)
                                if (px[static_cast<size_t>(y) * g_sh.res + x] < best) { best = px[static_cast<size_t>(y) * g_sh.res + x]; bx = x; by = y; }
                        if (best >= 1.0f) continue;
                        const glm::vec4 w = inv * glm::vec4((bx + 0.5f) / g_sh.res * 2 - 1, (by + 0.5f) / g_sh.res * 2 - 1, best * 2 - 1, 1);
                        std::printf("[shadows]   cell %d,%d nearest depth %.3f world %.1f %.1f %.1f\n", gx, gy, best, w.x / w.w, w.y / w.w, w.z / w.w);
                    }
            }
            for (int k = 0; k < kCascades; ++k) {
                const float* px = all.data() + static_cast<size_t>(k) * g_sh.res * g_sh.res;
                char fn[64];
                std::snprintf(fn, sizeof fn, "shadow_%d_casc%d.pgm", n, k);
                if (FILE* f = std::fopen(fn, "wb")) {
                    std::fprintf(f, "P5 %d %d 255\n", g_sh.res, g_sh.res);
                    for (int y = g_sh.res - 1; y >= 0; --y)
                        for (int x = 0; x < g_sh.res; ++x) std::fputc(static_cast<int>(std::clamp(px[static_cast<size_t>(y) * g_sh.res + x], 0.0f, 1.0f) * 255.0f), f);
                    std::fclose(f);
                }
            }
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (clamp) glEnable(GL_DEPTH_CLAMP);
    else glDisable(GL_DEPTH_CLAMP);
    glActiveTexture(GL_TEXTURE0 + kUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_sh.tex);
    if (g_sh.ptex != 0) {
        glActiveTexture(GL_TEXTURE0 + kPtUnit);
        glBindTexture(kGlCubeMapArray, g_sh.ptex);
    }
    point_tiles_update();
    glActiveTexture(GL_TEXTURE0 + kTileUnit);
    glBindTexture(GL_TEXTURE_2D, g_sh.tiles);
    {  // the cascades' maps again, read as depths (a sampler without the compare)
        static GLuint smp = 0;
        if (smp == 0) {
            glGenSamplers(1, &smp);
            glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(smp, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
        glActiveTexture(GL_TEXTURE0 + kDepthUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g_sh.tex);
        glBindSampler(kDepthUnit, smp);
    }
    glActiveTexture(GL_TEXTURE0);
    g_sh.on = true;
}

// orig: (host) see mrp_lighting.h
ShadowLocs mrp_lighting_locs(unsigned prog) {
    const GLuint p = static_cast<GLuint>(prog);
    ShadowLocs l;
    l.pass = glGetUniformLocation(p, "uShPass");
    l.on = glGetUniformLocation(p, "uShOn");
    l.inv_view = glGetUniformLocation(p, "uShInvView");
    l.proj = glGetUniformLocation(p, "uShProj");
    l.light = glGetUniformLocation(p, "uShLight");
    l.cut = glGetUniformLocation(p, "uShCut");
    l.map = glGetUniformLocation(p, "uShMap");
    l.casc = glGetUniformLocation(p, "uShCasc");
    l.cam = glGetUniformLocation(p, "uShCam");
    l.sun = glGetUniformLocation(p, "uShSun");
    l.param = glGetUniformLocation(p, "uShParam");
    l.texel = glGetUniformLocation(p, "uShTexel");
    l.part = glGetUniformLocation(p, "uShPart");
    l.depth = glGetUniformLocation(p, "uShDepth");
    l.range = glGetUniformLocation(p, "uShRange");
    l.fade = glGetUniformLocation(p, "uShFade");
    l.sun_k = glGetUniformLocation(p, "uShSunK");
    l.key = glGetUniformLocation(p, "uShKey");
    l.debug = glGetUniformLocation(p, "uShDebug");
    l.pt_n = glGetUniformLocation(p, "uPtN");
    l.pt_pos = glGetUniformLocation(p, "uPtPos");
    l.pt_col = glGetUniformLocation(p, "uPtCol");
    l.pt_map = glGetUniformLocation(p, "uPtMap");
    l.pt_near = glGetUniformLocation(p, "uPtNear");
    l.opaque = glGetUniformLocation(p, "uShOpaque");
    l.pt_tiles = glGetUniformLocation(p, "uPtTiles");
    l.pt_vp = glGetUniformLocation(p, "uPtVP");
    return l;
}

namespace {
void bind_common(const ShadowLocs& l) {
    glUniformMatrix4fv(l.inv_view, 1, GL_FALSE, glm::value_ptr(g_sh.inv_view));
    glUniform2f(l.proj, g_sh.proj.x, g_sh.proj.y);
}
// the lighting's samplers on their own units, whatever the draw (a lit receiver, the sky, a caster, the lighting off): left at
// their default unit 0, the point lights' cube maps (samplerCubeArrayShadow) and the tiles (usampler2D) would share it with uTexs[0]
// (sampler2DArray) and every draw of the program would fail (GL_INVALID_OPERATION). A program's uniforms keep their values: once
// a lit receiver had set them, all was well - but the loading screen's flight from the title's Load Game, the first frames the lighting
// runs in, draws the sky alone (an empty world): its sky was never drawn
void bind_units(const ShadowLocs& l) {
    glUniform1i(l.map, kUnit);
    glUniform1i(l.depth, kDepthUnit);
    glUniform1i(l.pt_map, kPtUnit);
    glUniform1i(l.pt_tiles, kTileUnit);
}
}  // namespace

// orig: (host) see mrp_lighting.h
void mrp_lighting_bind_receiver(const ShadowLocs& l, bool sky) {
    const bool on = mrp_lighting_active() && !sky;
    glUniform1i(l.pass, 0);
    glUniform1i(l.on, on ? 1 : 0);
    bind_units(l);
    if (!on) return;
    bind_common(l);
    glm::mat4 m[kCascades];
    for (int k = 0; k < kCascades; ++k) m[k] = g_sh.casc[k].tex;
    glUniformMatrix4fv(l.casc, kCascades, GL_FALSE, glm::value_ptr(m[0]));
    glUniform3f(l.cam, g_sh.cam.x, g_sh.cam.y, g_sh.cam.z);
    glUniform3f(l.sun, g_sh.sun.x, g_sh.sun.y, g_sh.sun.z);
    glUniform4f(l.param, static_cast<float>(g_shadow_strength) / 100.0f, g_has_sun.load() ? static_cast<float>(kCascades) : 0.0f, 0.08f, 1.0f);
    glUniform3f(l.texel, g_sh.casc[0].texel, g_sh.casc[1].texel, g_sh.casc[2].texel);
    glUniform3f(l.part, g_sh.casc[0].part, g_sh.casc[1].part, g_sh.casc[2].part);
    if (l.depth >= 0) {  // (only the lighting's variants have them)
        static const float fd[3] = {static_cast<float>(env_int("RAC1_SHADOW_FADE_FROM", 25)), static_cast<float>(env_int("RAC1_SHADOW_FADE_TO", 120)),
                                    static_cast<float>(env_int("RAC1_SHADOW_FADE_KEEP", 35)) / 100.0f};  // (testing: units, units, percent)
        glUniform3f(l.range, g_sh.casc[0].range, g_sh.casc[1].range, g_sh.casc[2].range);
        glUniform4f(l.fade, fd[0], fd[1], fd[2], g_shadow_fade != 0 ? 1.0f : 0.0f);
        glUniform1f(l.sun_k, g_sh.sun_k);
        if (l.key >= 0) {  // (RAC1_WORLD_SHADOW=share, testing / no key light known: the old estimate, the strength's share of the colour)
            static const bool share = std::getenv("RAC1_WORLD_SHADOW") != nullptr && std::strcmp(std::getenv("RAC1_WORLD_SHADOW"), "share") == 0;
            const glm::vec4& k = g_sh.key;
            if (share || !(k.x + k.y + k.z > 0.0f)) glUniform4f(l.key, 0.0f, 0.0f, 0.0f, -1.0f);
            else glUniform4f(l.key, k.x * (128.0f / 255.0f), k.y * (128.0f / 255.0f), k.z * (128.0f / 255.0f), k.w);
        }
    }
    glUniform1i(l.debug, g_shadow_debug);
    glUniform1i(l.pt_n, g_modern_points != 0 ? g_sh.npts : -1);
    glUniform1f(l.pt_near, kPtNear);
    glUniformMatrix4fv(l.pt_vp, 1, GL_FALSE, glm::value_ptr(g_sh.view_proj));
    if (g_sh.npts > 0) {
        glUniform4fv(l.pt_pos, g_sh.npts, glm::value_ptr(g_sh.pt_pos[0]));
        glUniform4fv(l.pt_col, g_sh.npts, glm::value_ptr(g_sh.pt_col[0]));
    }
}

// orig: (host) see mrp_lighting.h
void mrp_lighting_bind_caster(const ShadowLocs& l, const float* light_vp) {
    glUniform1i(l.pass, 1);
    glUniform1i(l.on, 0);
    bind_units(l);
    bind_common(l);
    glUniformMatrix4fv(l.light, 1, GL_FALSE, light_vp);
    glUniform4f(l.cut, g_sh.sun.x, g_sh.sun.y, g_sh.sun.z, kBack + glm::dot(g_sh.cam, g_sh.sun));
}

// orig: (host) see mrp_lighting.h
void mrp_lighting_bind_off(const ShadowLocs& l) {
    glUniform1i(l.pass, 0);
    glUniform1i(l.on, 0);
    glUniform1i(l.pt_n, -1);
    bind_units(l);
}

}  // namespace rac1
