// Viewmodel mirror probe - see vm_mirror.h for the design and its reasoning.

#include "core/gfx/vm_mirror.h"
#include "core/ui/overlay.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bvr::vm_mirror {
namespace {

constexpr int kMaxRvas = 4;
constexpr int kMaxTiers = 4;
constexpr int kCacheSlots = 96;
constexpr uint32_t kMaxBytes = 1088;
constexpr uint32_t kMaxFloats = kMaxBytes / 4;
constexpr int kRecentMats = 32;
// The screen-ray helper block every perspective cb0 carries at floats 12..18:
// (2*tanH, 0, -tanH, 0, 0, -2*tanV, tanV). f12 gives the draw's own tanH.
constexpr uint32_t kScreenRayFirst = 12;

// ---- configuration (game thread writes once, then publishes) ----------------
std::atomic<bool> g_configured{false};
uintptr_t g_fgRet[kMaxRvas] = {};
int g_nRet = 0;
TierSpec g_tiers[kMaxTiers] = {};
int g_nTiers = 0;
int g_rowX[kMaxTiers] = {}; // live (discovered) row per tier; render thread owns after configure

// ---- live controls ----------------------------------------------------------
std::atomic<bool> g_armed{false};
std::atomic<float> g_halfIpdUu{3.15f};
std::atomic<float> g_shiftUu{0.0f};     // probe: move the mirror plane sideways
std::atomic<bool> g_invertEye{false};   // probe: if depth looks inside-out
std::atomic<int> g_planeMode{0};        // PlaneMode::Gun
// Published per-eye planes (game thread writes, render thread reads).
std::atomic<float> g_planeN[2][3] = {};
std::atomic<float> g_planeD[2] = {};
std::atomic<bool> g_planeValid[2] = {};
std::atomic<bool> g_flipWinding{true};  // probe: A/B the inside-out fix
std::atomic<bool> g_skipUnmatched{true};
std::atomic<int> g_scanBytes{6144};
std::atomic<float> g_worldScale{100.0f};
std::atomic<float> g_maxReachM{1.3f}; // gun + hands never sit further than this
std::atomic<float> g_reachExcM{0.0f}, g_reachExcTolM{0.0f}; // see set_reach_exception
std::atomic<uint64_t> g_reachExcUntilMs{0};

// ---- per-buffer copy of the last game upload (render thread only) ----------
struct Entry {
    ID3D11Resource* res; // identity only, never dereferenced
    uint32_t bytes;
    uint32_t frame;      // present index of the upload
    bool mirrored;       // buffer currently holds OUR reflected copy
    float data[kMaxFloats];
};
Entry g_cache[kCacheSlots];
int g_cacheNext = 0;
uint32_t g_frame = 0;

// Recent base-tier fg matrices (unreflected) for lighting-tier discovery.
float g_recent[kRecentMats][16];
int g_recentCount = 0, g_recentNext = 0;

std::unordered_map<ID3D11RasterizerState*, ID3D11RasterizerState*> g_flipped;
thread_local bool t_rewriting = false;

// ---- counters (render thread writes, UI reads; relaxed is fine) ------------
std::atomic<uint32_t> g_cFgDraws{0}, g_cRewrites{0}, g_cRewriteFail{0}, g_cSkipped{0},
    g_cCandidates{0}, g_cEyeL{0}, g_cEyeR{0}, g_cEyeNone{0}, g_cMatched{0}, g_cReuse{0},
    g_cRejected{0};
std::atomic<uint32_t> g_rateFg{0}, g_rateRewrites{0}, g_rateSkipped{0}, g_rateCand{0},
    g_rateEyeL{0}, g_rateEyeR{0}, g_rateEyeNone{0}, g_rateMatched{0}, g_rateReuse{0},
    g_rateRejected{0};
uint64_t g_rateStampMs = 0;
uint32_t g_snapFg = 0, g_snapRw = 0, g_snapSk = 0, g_snapCand = 0, g_snapL = 0, g_snapR = 0,
         g_snapN = 0, g_snapM = 0, g_snapU = 0, g_snapX = 0;
std::atomic<bool> g_matchByTransform{true};
std::atomic<void (*)()> g_gameUi{nullptr};
std::mutex g_noteMutex;
char g_note[160] = "";

// ---- v6 effects census --------------------------------------------------------
enum { kCensusOff = 0, kCensusBaseline = 1, kCensusCasting = 2 };
std::atomic<int> g_censusPhase{kCensusOff};
std::atomic<int> g_censusRequest{kCensusOff};
uint64_t g_censusUntilMs = 0;
struct Sig {
    uint32_t count = 0, inWindow = 0, kind = 0, cbBytes = 0, lastCount = 0;
    int n = 0;
    uint32_t rvas[8] = {};
};
std::unordered_map<uint64_t, Sig> g_sigBase, g_sigCast;

// ---- the fg WINDOW (v3) -----------------------------------------------------
// v2's run measured ~5,000-6,700 WORLD draws/s carrying the fg-bake return
// addresses: that marker is a skinning path, not an fg one. What IS fg-only is
// ORDER: the fg scene node renders before the world (ENGINE_NOTES: "the fg draws
// are the first draws of the main pass"). So a stack match only counts while
// the window is open - from the frame's start until N consecutive plain world
// candidates follow an accepted fg draw. After it closes, only exact
// transform identity with this frame's fg matrices (light passes) or a buffer
// we already reflected qualifies.
std::atomic<int> g_windowCloseAfter{4};
bool g_winOpen = true, g_fgSeen = false;
int g_nonFgRun = 0;
std::atomic<uint32_t> g_cRejOrder{0}, g_rateRejOrder{0};
uint32_t g_snapO = 0;
// One-frame classification trace + labelled stack samples (diagnostics).
std::atomic<bool> g_traceArm{false};
bool g_tracing = false;
char g_trace[256];
int g_traceLen = 0;
int g_stackSamplesFg = 0, g_stackSamplesWorld = 0;
void trace_put(char c) {
    if (g_tracing && g_traceLen < static_cast<int>(sizeof g_trace) - 1) g_trace[g_traceLen++] = c;
}
uintptr_t g_exeLo = 0, g_exeHi = 0;

int tier_index(uint32_t bytes) {
    for (int i = 0; i < g_nTiers; ++i)
        if (g_tiers[i].bytes == bytes) return i;
    return -1;
}

Entry* find_entry(ID3D11Resource* res) {
    for (int i = 0; i < kCacheSlots; ++i)
        if (g_cache[i].res == res) return &g_cache[i];
    return nullptr;
}

// Is a known fg-bake return address live on this stack? Bounded by the
// thread's own stack base, so no SEH is needed.
bool stack_has_fg(void* esp) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t lo = reinterpret_cast<uintptr_t>(esp) & ~uintptr_t(3);
    uintptr_t hi = lo + static_cast<uintptr_t>(g_scanBytes.load(std::memory_order_relaxed));
    const uintptr_t base = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (hi > base) hi = base;
    for (uintptr_t p = lo; p + 4 <= hi; p += 4) {
        const uintptr_t v = *reinterpret_cast<const uint32_t*>(p);
        for (int i = 0; i < g_nRet; ++i)
            if (v == g_fgRet[i]) return true;
    }
    return false;
}

bool near_equal(const float* a, const float* b) {
    for (int i = 0; i < 16; ++i) {
        const float tol = 1e-3f * (1.0f + fabsf(b[i]));
        if (fabsf(a[i] - b[i]) > tol) return false;
    }
    return true;
}

// Find where a lighting tier keeps the clip transform: a 16-float block equal
// to a recent base-tier fg matrix (a light pass re-draws the same section).
int discover_row(const Entry& e) {
    const int n = static_cast<int>(e.bytes / 4);
    for (int k = 0; k + 16 <= n; k += 4)
        for (int m = 0; m < g_recentCount; ++m)
            if (near_equal(&e.data[k], g_recent[m])) return k;
    return -1;
}

// Arm's-reach test. A draw's object origin maps to clip (row_x.w, row_y.w,
// row_z.w, row_w.w); with clip.x = x/tanH, clip.y = y/tanV and w = depth, that
// recovers the origin's view-space position. The viewmodel's origin is the rig
// (at the camera) or, on the rigid path, a hand bone - within reach. A world
// mesh that shares the bake code (the pillar ice) sits metres away.
bool within_reach(const Entry& e, int row) {
    if (row < 0 || row + 16 > static_cast<int>(e.bytes / 4)) return true; // cannot tell
    const float tanH = e.data[kScreenRayFirst] * 0.5f;
    const float tanV = e.data[kScreenRayFirst + 6];
    const float x = e.data[row + 3] * tanH;
    const float y = e.data[row + 7] * tanV;
    const float z = e.data[row + 15];
    const float d = sqrtf(x * x + y * y + z * z);
    const float ws = g_worldScale.load(std::memory_order_relaxed);
    const float maxUu = g_maxReachM.load(std::memory_order_relaxed) * ws;
    if (d <= maxUu) return true;
    const float exc = g_reachExcM.load(std::memory_order_relaxed);
    return exc > 0.0f && GetTickCount64() < g_reachExcUntilMs.load(std::memory_order_relaxed) &&
           fabsf(d - exc * ws) <= g_reachExcTolM.load(std::memory_order_relaxed) * ws;
}

// Reflect a draw's component->clip rows (x, y, z, w; 4 floats each) about the
// plane n.X = d in the view space the rows themselves define:
//   view x = tanH * rowX.p,  view y = tanV * rowY.p,  view z = rowW.p,
// and the homogeneous 1 = e.p with e = (0,0,0,1) - exact for any affine
// object-to-view chain, rigid path (bones baked) included. The reflection
//   X' = X - 2 (n.X - d) n
// is applied to those three rows; the depth row keeps its own relation to w,
// rowZ = a*rowW + b*e, with a and b recovered from the matrix, so the depth
// mapping never needs to be known. For n = (1,0,0) this is exactly v1's
// head-plane edit: rowX' = -rowX + (2d/tanH) e.
bool reflect_rows(float* r, float tanH, float tanV, const float n[3], float d) {
    float* rx = r;
    float* ry = r + 4;
    float* rz = r + 8;
    float* rw = r + 12;
    const float ww = rw[0] * rw[0] + rw[1] * rw[1] + rw[2] * rw[2];
    if (!(tanH > 1e-4f) || !(tanV > 1e-4f) || !(ww > 1e-12f)) return false;
    const float a = (rz[0] * rw[0] + rz[1] * rw[1] + rz[2] * rw[2]) / ww;
    const float b = rz[3] - a * rw[3];
    for (int i = 0; i < 4; ++i) {
        const float vx = rx[i] * tanH, vy = ry[i] * tanV, vz = rw[i];
        const float s = n[0] * vx + n[1] * vy + n[2] * vz - (i == 3 ? d : 0.0f);
        rx[i] = (vx - 2.0f * n[0] * s) / tanH;
        ry[i] = (vy - 2.0f * n[1] * s) / tanV;
        rw[i] = vz - 2.0f * n[2] * s;
        rz[i] = a * rw[i] + (i == 3 ? b : 0.0f);
    }
    return true;
}

// Does this upload carry the exact clip transform of a fg draw from THIS
// frame? A light pass re-draws a section with the same component->clip matrix
// as its base pass, so identity with a stack-flagged fg matrix makes a draw fg
// whichever code path issued it. Returns the float offset, or -1.
int match_recent(const Entry& e, int knownRow) {
    if (g_recentCount == 0) return -1;
    if (knownRow >= 0) {
        if (knownRow + 16 > static_cast<int>(e.bytes / 4)) return -1;
        for (int m = 0; m < g_recentCount; ++m)
            if (near_equal(&e.data[knownRow], g_recent[m])) return knownRow;
        return -1;
    }
    return discover_row(e);
}

void push_recent(const float* m16) {
    memcpy(g_recent[g_recentNext], m16, sizeof(float) * 16);
    g_recentNext = (g_recentNext + 1) % kRecentMats;
    if (g_recentCount < kRecentMats) ++g_recentCount;
}

// Log the game-exe return addresses on this stack (the discovery instrument
// for the code path that draws the passes the stack marker misses).
void log_stack_sample(void* esp, uint32_t bytes, const char* what = "transform-matched fg draw") {
    if (!g_exeLo) return;
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    uintptr_t lo = reinterpret_cast<uintptr_t>(esp) & ~uintptr_t(3);
    uintptr_t hi = lo + 4096;
    const uintptr_t base = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (hi > base) hi = base;
    char line[320];
    int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                          "[mirror] %s (%u-byte cb) - exe return stack:", what, bytes);
    int found = 0;
    for (uintptr_t p = lo; p + 4 <= hi && found < 14; p += 4) {
        const uintptr_t v = *reinterpret_cast<const uint32_t*>(p);
        if (v < g_exeLo + 0x1006 || v >= g_exeHi) continue;
        const uint8_t* c = reinterpret_cast<const uint8_t*>(v);
        if (!(c[-5] == 0xE8 || c[-2] == 0xFF || c[-3] == 0xFF || c[-6] == 0xFF)) continue;
        ++found;
        if (len > 0 && len < 300)
            len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " 0x%X",
                               static_cast<uint32_t>(v - g_exeLo));
    }
    BVR_LOG("%s", line);
}

ID3D11RasterizerState* flipped_state(ID3D11DeviceContext* ctx, ID3D11RasterizerState* orig) {
    auto it = g_flipped.find(orig);
    if (it != g_flipped.end()) return it->second;
    D3D11_RASTERIZER_DESC d{};
    if (orig) {
        orig->GetDesc(&d);
        orig->AddRef(); // pin the key so its address cannot be reused
    } else {
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.FrontCounterClockwise = !d.FrontCounterClockwise;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11RasterizerState* flip = nullptr;
    if (dev) {
        if (FAILED(dev->CreateRasterizerState(&d, &flip))) flip = nullptr;
        dev->Release();
    }
    g_flipped.emplace(orig, flip);
    BVR_LOG("[mirror] rasterizer %p -> flipped %p (cull %d, frontCCW now %d)", orig, flip,
            static_cast<int>(d.CullMode), static_cast<int>(d.FrontCounterClockwise));
    return flip;
}

void tick_rates() {
    const uint64_t now = GetTickCount64();
    if (now - g_rateStampMs < 1000) return;
    g_rateStampMs = now;
    auto roll = [](std::atomic<uint32_t>& total, uint32_t& snap, std::atomic<uint32_t>& rate) {
        const uint32_t t = total.load(std::memory_order_relaxed);
        rate.store(t - snap, std::memory_order_relaxed);
        snap = t;
    };
    roll(g_cFgDraws, g_snapFg, g_rateFg);
    roll(g_cRewrites, g_snapRw, g_rateRewrites);
    roll(g_cSkipped, g_snapSk, g_rateSkipped);
    roll(g_cCandidates, g_snapCand, g_rateCand);
    roll(g_cEyeL, g_snapL, g_rateEyeL);
    roll(g_cEyeR, g_snapR, g_rateEyeR);
    roll(g_cEyeNone, g_snapN, g_rateEyeNone);
    roll(g_cMatched, g_snapM, g_rateMatched);
    roll(g_cReuse, g_snapU, g_rateReuse);
    roll(g_cRejected, g_snapX, g_rateRejected);
    roll(g_cRejOrder, g_snapO, g_rateRejOrder);
    static uint64_t lastLog = 0;
    if (g_armed.load(std::memory_order_relaxed) && now - lastLog >= 5000) {
        lastLog = now;
        BVR_LOG("[mirror] per s: fg draws %u (by stack %u, by transform %u, reused %u), "
                "reflected uploads %u, skipped %u, rejected as world %u (by order %u), "
                "candidates %u "
                "| eye L %u R %u none %u | rewrite fails %u total | rows 576:%d 832:%d 1088:%d",
                g_rateFg.load() + g_rateMatched.load() + g_rateReuse.load(), g_rateFg.load(),
                g_rateMatched.load(), g_rateReuse.load(),
                g_rateRewrites.load(), g_rateSkipped.load(), g_rateRejected.load(),
                g_rateRejOrder.load(), g_rateCand.load(),
                g_rateEyeL.load(), g_rateEyeR.load(), g_rateEyeNone.load(), g_cRewriteFail.load(),
                tier_index(576) >= 0 ? g_rowX[tier_index(576)] : -9,
                tier_index(832) >= 0 ? g_rowX[tier_index(832)] : -9,
                tier_index(1088) >= 0 ? g_rowX[tier_index(1088)] : -9);
    }
}

} // namespace

void configure(const uint32_t* fgRetRvas, int nRvas, const TierSpec* tiers, int nTiers) {
    if (g_configured.load(std::memory_order_acquire)) return;
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
        g_exeLo = exe;
        g_exeHi = exe + nt->OptionalHeader.SizeOfImage;
    }
    g_nRet = 0;
    for (int i = 0; i < nRvas && i < kMaxRvas; ++i) {
        const uintptr_t a = exe + fgRetRvas[i];
        // A return address must follow a CALL: E8 rel32 (5 bytes back) or an
        // FF /2 form (2, 3 or 6 bytes back). Anything else = wrong build.
        const uint8_t* c = reinterpret_cast<const uint8_t*>(a);
        const bool call = c[-5] == 0xE8 || c[-2] == 0xFF || c[-3] == 0xFF || c[-6] == 0xFF;
        BVR_LOG("[mirror] fg-bake return address rva 0x%X %s", fgRetRvas[i],
                call ? "follows a CALL - ok" : "does NOT follow a CALL - wrong exe build?");
        g_fgRet[g_nRet++] = a;
    }
    g_nTiers = 0;
    for (int i = 0; i < nTiers && i < kMaxTiers; ++i) {
        g_tiers[g_nTiers] = tiers[i];
        g_rowX[g_nTiers] = tiers[i].rowX;
        ++g_nTiers;
    }
    g_configured.store(true, std::memory_order_release);
}

bool configured() { return g_configured.load(std::memory_order_acquire); }

void set_armed(bool on) {
    const bool was = g_armed.exchange(on && configured(), std::memory_order_relaxed);
    if (!was && on && configured()) {
        static bool s_autoTraced = false;
        if (!s_autoTraced) {
            s_autoTraced = true;
            g_traceArm.store(true, std::memory_order_relaxed);
        }
    }
    if (was != (on && configured()))
        BVR_LOG("[mirror] viewmodel mirror %s", on ? "ARMED" : "off");
}
bool armed() { return g_armed.load(std::memory_order_relaxed); }
void set_eye_half_uu(float h) { g_halfIpdUu.store(h, std::memory_order_relaxed); }
void set_plane_shift_uu(float uu) { g_shiftUu.store(uu, std::memory_order_relaxed); }
void set_game_ui(void (*fn)()) { g_gameUi.store(fn); }

void set_ui_note(const char* text) {
    std::lock_guard<std::mutex> lock(g_noteMutex);
    strncpy_s(g_note, text ? text : "", _TRUNCATE);
}

bool census_active() {
    return g_censusPhase.load(std::memory_order_relaxed) != kCensusOff ||
           g_censusRequest.load(std::memory_order_relaxed) != kCensusOff;
}

void census_draw(ID3D11DeviceContext* ctx, void* esp, int kind, unsigned count) {
    const uint64_t now = GetTickCount64();
    const int req = g_censusRequest.exchange(kCensusOff, std::memory_order_relaxed);
    if (req != kCensusOff) {
        (req == kCensusBaseline ? g_sigBase : g_sigCast).clear();
        g_censusPhase.store(req, std::memory_order_relaxed);
        g_censusUntilMs = now + 4000;
        BVR_LOG("[mirror] census: recording %s for 4 s", req == kCensusBaseline
                                                              ? "BASELINE (not casting)"
                                                              : "WHILE CASTING");
    }
    const int phase = g_censusPhase.load(std::memory_order_relaxed);
    if (phase == kCensusOff) return;
    if (now >= g_censusUntilMs) {
        g_censusPhase.store(kCensusOff, std::memory_order_relaxed);
        if (phase == kCensusBaseline) {
            BVR_LOG("[mirror] census: baseline done, %u draw signatures",
                    static_cast<unsigned>(g_sigBase.size()));
            return;
        }
        std::vector<const Sig*> only;
        for (const auto& [h, sig] : g_sigCast)
            if (!g_sigBase.count(h)) only.push_back(&sig);
        std::sort(only.begin(), only.end(),
                  [](const Sig* a, const Sig* b) { return a->count > b->count; });
        BVR_LOG("[mirror] census: %u signatures while casting, %u of them NOT in the "
                "baseline%s",
                static_cast<unsigned>(g_sigCast.size()), static_cast<unsigned>(only.size()),
                g_sigBase.empty() ? " (no baseline recorded - press step 1 first)" : "");
        static const char* kKinds[] = {"DrawIndexed", "Draw", "DrawIndexedInst", "DrawInst"};
        for (size_t i = 0; i < only.size() && i < 25; ++i) {
            const Sig& s = *only[i];
            char line[400];
            int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                                  "[mirror] census NEW: %s x%u (in fg window %u) cb0 %u B, "
                                  "last count %u | stack:",
                                  kKinds[s.kind & 3], s.count, s.inWindow, s.cbBytes,
                                  s.lastCount);
            for (int k = 0; k < s.n && len > 0 && len < 380; ++k)
                len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " 0x%X", s.rvas[k]);
            BVR_LOG("%s", line);
        }
        return;
    }
    // Signature: the first 8 game-exe return addresses on the stack.
    uint32_t rvas[8];
    int n = 0;
    if (g_exeLo) {
        const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        uintptr_t lo = reinterpret_cast<uintptr_t>(esp) & ~uintptr_t(3);
        uintptr_t hi = lo + 3072;
        const uintptr_t base = reinterpret_cast<uintptr_t>(tib->StackBase);
        if (hi > base) hi = base;
        for (uintptr_t p = lo; p + 4 <= hi && n < 8; p += 4) {
            const uintptr_t v = *reinterpret_cast<const uint32_t*>(p);
            if (v < g_exeLo + 0x1006 || v >= g_exeHi) continue;
            const uint8_t* c = reinterpret_cast<const uint8_t*>(v);
            if (!(c[-5] == 0xE8 || c[-2] == 0xFF || c[-3] == 0xFF || c[-6] == 0xFF)) continue;
            rvas[n++] = static_cast<uint32_t>(v - g_exeLo);
        }
    }
    uint64_t h = 1469598103934665603ull ^ static_cast<uint64_t>(kind);
    for (int k = 0; k < n; ++k) h = (h ^ rvas[k]) * 1099511628211ull;
    uint32_t cbBytes = 0;
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (cb) {
        D3D11_BUFFER_DESC bd{};
        cb->GetDesc(&bd);
        cbBytes = bd.ByteWidth;
        cb->Release();
    }
    Sig& s = (phase == kCensusBaseline ? g_sigBase : g_sigCast)[h];
    if (s.count == 0) {
        s.kind = static_cast<uint32_t>(kind);
        s.n = n;
        memcpy(s.rvas, rvas, sizeof(uint32_t) * n);
    }
    ++s.count;
    if (g_winOpen) ++s.inWindow;
    s.cbBytes = cbBytes;
    s.lastCount = count;
}
void set_plane(int eye, const float n[3], float d, bool valid) {
    if (eye < 0 || eye > 1) return;
    for (int i = 0; i < 3; ++i) g_planeN[eye][i].store(n[i], std::memory_order_relaxed);
    g_planeD[eye].store(d, std::memory_order_relaxed);
    g_planeValid[eye].store(valid, std::memory_order_relaxed);
}
PlaneMode plane_mode() {
    return static_cast<PlaneMode>(g_planeMode.load(std::memory_order_relaxed));
}
float plane_shift_uu() { return g_shiftUu.load(std::memory_order_relaxed); }
void set_reach_exception(float distM, float tolM, int ttlMs) {
    g_reachExcM.store(distM > 0.0f ? distM : 0.0f, std::memory_order_relaxed);
    g_reachExcTolM.store(tolM, std::memory_order_relaxed);
    g_reachExcUntilMs.store(distM > 0.0f ? GetTickCount64() + static_cast<uint64_t>(ttlMs) : 0,
                            std::memory_order_relaxed);
}
void set_world_scale(float s) {
    if (s > 1.0f) g_worldScale.store(s, std::memory_order_relaxed);
}

bool wants_map_tracking() { return g_armed.load(std::memory_order_relaxed); }

void on_present() {
    ++g_frame;
    g_recentCount = 0; // the transform ring is per eye frame
    g_recentNext = 0;
    if (g_tracing) {
        g_trace[g_traceLen] = 0;
        BVR_LOG("[mirror] one-frame trace (S stack-fg, M transform, U reused, w world, "
                "o stack-hit rejected by order, d rejected by distance, | window closed): %s",
                g_trace);
        g_tracing = false;
    }
    if (g_traceArm.exchange(false, std::memory_order_relaxed)) {
        g_tracing = true;
        g_traceLen = 0;
    }
    g_winOpen = true;
    g_fgSeen = false;
    g_nonFgRun = 0;
    tick_rates();
}

void on_unmap(ID3D11Resource* res, const void* data, uint32_t bytes) {
    if (!g_armed.load(std::memory_order_relaxed) || t_rewriting || !res || !data) return;
    if (tier_index(bytes) < 0) return;
    const float* f = static_cast<const float*>(data);
    if (!(f[kScreenRayFirst] > 0.1f)) return; // not a perspective draw
    Entry* e = find_entry(res);
    if (!e) {
        e = &g_cache[g_cacheNext];
        g_cacheNext = (g_cacheNext + 1) % kCacheSlots;
        e->res = res;
    }
    e->bytes = bytes;
    e->frame = g_frame;
    e->mirrored = false;
    memcpy(e->data, data, bytes);
}

DrawToken before_draw(ID3D11DeviceContext* ctx, void* esp) {
    DrawToken t;
    if (!g_armed.load(std::memory_order_relaxed)) return t;
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return t;
    Entry* e = find_entry(cb);
    // A copy older than two presents may belong to a freed-and-reused buffer.
    if (!e || g_frame - e->frame > 2) {
        cb->Release();
        return t;
    }
    g_cCandidates.fetch_add(1, std::memory_order_relaxed);
    const int ti = tier_index(e->bytes);
    int matchedRow = -1;
    const bool stackHit = stack_has_fg(esp);
    if (stackHit && !g_winOpen) {
        // The skinning path, after the fg scene has finished: a world mesh.
        g_cRejOrder.fetch_add(1, std::memory_order_relaxed);
        trace_put('o');
        if (g_stackSamplesWorld < 4) {
            ++g_stackSamplesWorld;
            log_stack_sample(esp, e->bytes, "stack-hit WORLD draw (rejected by order)");
        }
        cb->Release();
        return t;
    }
    if (stackHit) {
        if (!within_reach(*e, ti >= 0 ? g_rowX[ti] : -1)) {
            g_cRejected.fetch_add(1, std::memory_order_relaxed);
            trace_put('d');
            if (g_stackSamplesWorld < 8) {
                ++g_stackSamplesWorld;
                log_stack_sample(esp, e->bytes, "stack-hit draw rejected by DISTANCE");
            }
            cb->Release();
            return t; // a world mesh on the same bake path
        }
        g_cFgDraws.fetch_add(1, std::memory_order_relaxed);
        g_fgSeen = true;
        g_nonFgRun = 0;
        trace_put('S');
        if (g_stackSamplesFg < 4) {
            ++g_stackSamplesFg;
            log_stack_sample(esp, e->bytes, "stack-hit FG draw (in window)");
        }
    } else if (e->mirrored) {
        // A later pass re-using a buffer we already reflected this frame: the
        // geometry is reflected, so it needs the winding flip too.
        g_cReuse.fetch_add(1, std::memory_order_relaxed);
        trace_put('U');
    } else if (g_matchByTransform.load(std::memory_order_relaxed) &&
               (matchedRow = match_recent(*e, ti >= 0 ? g_rowX[ti] : -1)) >= 0) {
        g_cMatched.fetch_add(1, std::memory_order_relaxed);
        trace_put('M');
        if (ti >= 0 && g_rowX[ti] < 0) {
            g_rowX[ti] = matchedRow;
            BVR_LOG("[mirror] %u-byte tier: clip transform discovered at float %d (by transform "
                    "match)",
                    e->bytes, matchedRow);
        }
        static int s_samples = 0;
        if (s_samples < 6) {
            ++s_samples;
            log_stack_sample(esp, e->bytes);
        }
    } else {
        trace_put('w');
        if (g_winOpen && g_fgSeen &&
            ++g_nonFgRun >= g_windowCloseAfter.load(std::memory_order_relaxed)) {
            g_winOpen = false;
            trace_put('|');
        }
        cb->Release();
        return t;
    }

    if (!e->mirrored) {
        int row = ti >= 0 ? g_rowX[ti] : -1;
        if (row < 0 && ti >= 0) {
            row = discover_row(*e);
            if (row >= 0) {
                g_rowX[ti] = row;
                BVR_LOG("[mirror] %u-byte tier: clip transform discovered at float %d",
                        e->bytes, row);
            }
        }
        if (row < 0) {
            static uint32_t s_logged = 0;
            if (s_logged < 3) {
                ++s_logged;
                BVR_LOG("[mirror] fg draw in the %u-byte tier: no clip transform found yet - "
                        "%s",
                        e->bytes, g_skipUnmatched.load() ? "skipped" : "drawn unmirrored");
            }
            cb->Release();
            if (g_skipUnmatched.load(std::memory_order_relaxed)) {
                g_cSkipped.fetch_add(1, std::memory_order_relaxed);
                t.skip = true;
            }
            return t;
        }
        // Remember this fg matrix (unreflected) for this frame's other passes.
        push_recent(&e->data[row]);

        const int eye = bvr::vr::sr_peek_eye();
        (eye < 0 ? g_cEyeL : eye > 0 ? g_cEyeR : g_cEyeNone)
            .fetch_add(1, std::memory_order_relaxed);
        int slot = eye < 0 ? 0 : eye > 0 ? 1 : -1;
        if (slot >= 0 && g_invertEye.load(std::memory_order_relaxed)) slot = 1 - slot;
        if (slot < 0 || !g_planeValid[slot].load(std::memory_order_relaxed)) {
            cb->Release();
            return t; // no plane for this frame: draw as the game made it
        }
        const float n[3] = {g_planeN[slot][0].load(std::memory_order_relaxed),
                            g_planeN[slot][1].load(std::memory_order_relaxed),
                            g_planeN[slot][2].load(std::memory_order_relaxed)};
        const float d = g_planeD[slot].load(std::memory_order_relaxed);

        float out[kMaxFloats];
        memcpy(out, e->data, e->bytes);
        if (!reflect_rows(&out[row], e->data[kScreenRayFirst] * 0.5f,
                          e->data[kScreenRayFirst + 6], n, d)) {
            cb->Release();
            return t;
        }

        D3D11_MAPPED_SUBRESOURCE m{};
        t_rewriting = true;
        const HRESULT hr = ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (SUCCEEDED(hr) && m.pData) {
            memcpy(m.pData, out, e->bytes);
            ctx->Unmap(cb, 0);
            e->mirrored = true;
            g_cRewrites.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cRewriteFail.fetch_add(1, std::memory_order_relaxed);
        }
        t_rewriting = false;
        if (!e->mirrored) {
            cb->Release();
            return t; // could not reflect: draw it as the game made it
        }
    }
    cb->Release();

    if (g_flipWinding.load(std::memory_order_relaxed)) {
        ID3D11RasterizerState* orig = nullptr;
        ctx->RSGetState(&orig); // +1 ref, released in after_draw
        ID3D11RasterizerState* flip = flipped_state(ctx, orig);
        if (flip) {
            ctx->RSSetState(flip);
            t.restore = true;
            t.original = orig;
        } else if (orig) {
            orig->Release();
        }
    }
    return t;
}

void after_draw(ID3D11DeviceContext* ctx, DrawToken& t) {
    if (!t.restore) return;
    ctx->RSSetState(t.original);
    if (t.original) t.original->Release();
    t.restore = false;
    t.original = nullptr;
}

void draw_debug_ui() {
    ImGui::Indent();
    if (!configured()) {
        ImGui::TextWrapped("This game does not support the viewmodel mirror yet.");
        ImGui::Unindent();
        return;
    }
    // Player-facing: where the mirror plane sits, and this weapon's trim.
    {
        std::lock_guard<std::mutex> lock(g_noteMutex);
        if (g_note[0]) ImGui::TextWrapped("%s", g_note);
    }
    int pm = g_planeMode.load(std::memory_order_relaxed);
    ImGui::Text("Mirror about:");
    ImGui::SameLine();
    if (ImGui::RadioButton("the gun (effects follow)", pm == 0)) g_planeMode.store(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("the head", pm == 1)) g_planeMode.store(1);
    float shift = g_shiftUu.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Muzzle trim for this weapon (UU)", &shift, -10.0f, 10.0f, "%.2f"))
        g_shiftUu.store(shift, std::memory_order_relaxed);
    if (auto fn = g_gameUi.load()) fn();

    if (!bvr::overlay::dev_tools()) {
        ImGui::Unindent();
        return;
    }
    // Developer tools: counters, probes and the classifier's internals.
    ImGui::SeparatorText("Mirror diagnostics");
    ImGui::Text("fg draws/s: stack %u  transform %u  reused %u | reflected/s %u  skipped/s %u",
                g_rateFg.load(), g_rateMatched.load(), g_rateReuse.load(),
                g_rateRewrites.load(), g_rateSkipped.load());
    ImGui::Text("eye L %u R %u none %u", g_rateEyeL.load(), g_rateEyeR.load(),
                g_rateEyeNone.load());
    ImGui::Text("rejected as world/s: distance %u  order %u", g_rateRejected.load(),
                g_rateRejOrder.load());
    int wc = g_windowCloseAfter.load(std::memory_order_relaxed);
    if (ImGui::SliderInt("Gun window closes after N world draws", &wc, 1, 20))
        g_windowCloseAfter.store(wc, std::memory_order_relaxed);
    if (ImGui::Button("Log one-frame draw trace")) g_traceArm.store(true);
    ImGui::Text("Plasmid effects census (may stutter while recording):");
    if (ImGui::Button("Record baseline (4 s, don't cast)")) g_censusRequest.store(kCensusBaseline);
    ImGui::SameLine();
    if (ImGui::Button("Record while casting (4 s)")) g_censusRequest.store(kCensusCasting);
    {
        const int ph = g_censusPhase.load(std::memory_order_relaxed);
        if (ph != kCensusOff)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "recording %s...",
                               ph == kCensusBaseline ? "baseline" : "while casting");
    }
    float reach = g_maxReachM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Max gun distance (m)", &reach, 0.5f, 3.0f, "%.2f"))
        g_maxReachM.store(reach, std::memory_order_relaxed);
    bool mt = g_matchByTransform.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Also mirror passes that share a gun draw's transform", &mt))
        g_matchByTransform.store(mt, std::memory_order_relaxed);
    bool inv = g_invertEye.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Invert eye sign (if the gun's depth looks inside-out)", &inv))
        g_invertEye.store(inv, std::memory_order_relaxed);
    bool wind = g_flipWinding.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Flip triangle winding (off = see the inside-out model)", &wind))
        g_flipWinding.store(wind, std::memory_order_relaxed);
    bool skip = g_skipUnmatched.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Skip lighting passes with no transform found", &skip))
        g_skipUnmatched.store(skip, std::memory_order_relaxed);
    int scan = g_scanBytes.load(std::memory_order_relaxed);
    if (ImGui::SliderInt("Stack scan depth (bytes)", &scan, 1024, 16384))
        g_scanBytes.store(scan, std::memory_order_relaxed);
    ImGui::Unindent();
}

} // namespace bvr::vm_mirror
