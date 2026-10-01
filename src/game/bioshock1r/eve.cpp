#include "game/bioshock1r/eve.h"

#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/console_exec.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <functional>
#include <string>

namespace bvr::b1r::eve {
namespace {

std::atomic<bool> g_probe{false};
DWORD g_gameTid = 0;

bool read_block(const void* src, void* out, size_t n) {
    __try {
        memcpy(out, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool may_log();

bool write_block(void* dst, const void* in, size_t n) {
    __try {
        memcpy(dst, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string class_of(const void* obj) {
    if (!obj) return "(none)";
    const wchar_t* w = patterns::object_class_name(obj);
    if (!w) return "?";
    std::string s;
    for (size_t i = 0; w[i] && i < 63; ++i) s += w[i] < 128 ? static_cast<char>(w[i]) : '?';
    return s;
}

// ---- memory watch ------------------------------------------------------------
// Every dword of a few objects, compared frame to frame. A word that changes
// more than 6 times in 10 s is noise (timers, positions, animation state) and
// goes quiet for good; the rest - counts, levels, flags, pointers that only
// change on an event - are logged the moment they change, as int and float.
constexpr int kMaxWords = 0x1000 / 4;
struct Watch {
    const char* name;
    uint32_t words;
    void* obj = nullptr;
    uint32_t prev[kMaxWords];
    uint8_t changes[kMaxWords];
    bool noisy[kMaxWords];
    bool havePrev = false;
    uint64_t boundMs = 0, windowMs = 0;
};
constexpr int kLinks = 8;
Watch g_watch[5 + kLinks] = {{"pawn", 0x1000 / 4}, {"pc", 0x1000 / 4}, {"hands", 0x800 / 4},
                             {"holdable", 0x800 / 4}, {"hypo", 0x800 / 4},
                             {"item0", 0x400 / 4}, {"item1", 0x400 / 4}, {"item2", 0x400 / 4},
                             {"item3", 0x400 / 4}, {"item4", 0x400 / 4}, {"item5", 0x400 / 4},
                             {"item6", 0x400 / 4}, {"item7", 0x400 / 4}};
// Research offsets on the syringe actor (from probe v2; they move into
// patterns.h with ENGINE_NOTES once the feature uses them):
//   +0x0B0 the actor it rides (PlayerHands while injecting, else 0)
//   +0x0D0 flag word; bit 0x100000 is SET while it is put away
//   +0x0F0 FName index of the bone it hangs from
[[maybe_unused]] constexpr uint32_t kHypoBaseOff = 0x0B0;
constexpr uint32_t kHypoFlagsOff = 0x0D0, kHypoBoneOff = 0x0F0;
constexpr uint32_t kHypoHiddenBit = 0x100000;
std::atomic<bool> g_showTest{false};
std::atomic<int> g_hiddenReq{0}; // console bHidden test: 1 = False, 2 = True
bool g_showWas = false;
uint32_t g_flagsSaved = 0;
float g_locSaved[3] = {};
// The syringe actor (BioAmmoHypoTool), learned from its attach.
void* g_hypo = nullptr;
bool g_hypoDumped = false;
// Objects linked from the pawn / controller / syringe whose class reads like
// inventory (ammo, hypo, EVE, bio, inventory) - the hypo COUNT lives in one.
void* g_link[kLinks] = {};
int g_linkCount = 0;
void* g_censused[8];
int g_censusCount = 0;

bool lower_has(const std::string& s, const char* sub) {
    std::string l;
    for (char c : s) l += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return l.find(sub) != std::string::npos;
}

// Every dword of `obj` that points at a live UObject: log the classes once,
// and adopt the inventory-looking ones as extra watches.
void census(void* obj, const char* name, uint32_t bytes) {
    if (!obj) return;
    for (int i = 0; i < g_censusCount; ++i)
        if (g_censused[i] == obj) return;
    if (g_censusCount < 8) g_censused[g_censusCount++] = obj;
    uint32_t words[0x1000 / 4];
    if (!read_block(obj, words, bytes)) return;
    std::string line;
    for (uint32_t i = 0; i < bytes / 4; ++i) {
        const uint32_t v = words[i];
        if (v < 0x10000 || (v & 3)) continue;
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(v));
        if (!patterns::object_class_name(p)) continue;
        const std::string cls = class_of(p);
        char b[96];
        _snprintf_s(b, sizeof b, _TRUNCATE, " +0x%03X=%s", i * 4, cls.c_str());
        if (line.size() < 1800) line += b;
        const bool inv = (lower_has(cls, "ammo") || lower_has(cls, "hypo") || lower_has(cls, "eve") ||
                          lower_has(cls, "bio")) &&
                         !lower_has(cls, "level") && !lower_has(cls, "subsystem") && !lower_has(cls, "tool");
        if (inv && p != g_hypo && g_linkCount < kLinks) {
            bool dup = false;
            for (int k = 0; k < g_linkCount; ++k) dup |= g_link[k] == p;
            if (!dup) {
                g_link[g_linkCount++] = p;
                BVR_LOG("[eve] watching %s (linked from %s+0x%03X)", cls.c_str(), name, i * 4);
            }
        }
    }
    BVR_LOG("[eve] %s links:%s", name, line.c_str());
}

// TArray<UObject*> fields {data, count, max} of `obj`: log every element's
// class and adopt the ammo/hypo/EVE-looking ones as watches (the inventory
// manager keeps its items in lists like these).
void census_arrays(void* obj, const char* name, uint32_t bytes) {
    if (!obj) return;
    uint32_t w[0x800 / 4];
    if (!read_block(obj, w, bytes)) return;
    for (uint32_t i = 0; i + 2 < bytes / 4; ++i) {
        const uint32_t data = w[i], n = w[i + 1], mx = w[i + 2];
        if (data < 0x10000 || (data & 3) || n == 0 || n > 256 || mx < n || mx > 4096) continue;
        uint32_t el[256];
        if (!read_block(reinterpret_cast<void*>(static_cast<uintptr_t>(data)), el, n * 4)) continue;
        int objs = 0;
        std::string line;
        for (uint32_t k = 0; k < n; ++k) {
            void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(el[k]));
            if (el[k] < 0x10000 || !patterns::object_class_name(p)) continue;
            ++objs;
            const std::string cls = class_of(p);
            char b[80];
            _snprintf_s(b, sizeof b, _TRUNCATE, " [%u]%s", k, cls.c_str());
            if (line.size() < 1500) line += b;
            const bool inv = lower_has(cls, "ammo") || lower_has(cls, "hypo") || lower_has(cls, "eve") ||
                             lower_has(cls, "bio") || lower_has(cls, "med");
            if (inv && p != g_hypo && g_linkCount < kLinks) {
                bool dup = false;
                for (int j = 0; j < g_linkCount; ++j) dup |= g_link[j] == p;
                if (!dup) {
                    g_link[g_linkCount++] = p;
                    BVR_LOG("[eve] watching item%d = %s (%s+0x%03X[%u])", g_linkCount - 1, cls.c_str(),
                            name, i * 4, k);
                }
            }
        }
        if (objs) BVR_LOG("[eve] %s+0x%03X array of %u:%s", name, i * 4, n, line.c_str());
    }
}

// ---- inventory lists (v4) ------------------------------------------------------
// Every TArray {data, count, max} in the inventory manager: its elements once
// (object/class names where they are objects, else ints), then its data block
// watched for changes - the hypo count is an element of one of these.
struct ArrWatch {
    uint32_t off = 0, n = 0;
    uintptr_t data = 0;
    uint32_t prev[64];
    bool have = false;
};
ArrWatch g_arr[12];
int g_arrCount = 0;
void* g_arrOwner = nullptr;

std::string obj_name(const void* obj) {
    int32_t idx = 0;
    if (!read_block(static_cast<const uint8_t*>(obj) + patterns::kUObjectNameIndexOffset, &idx, 4)) return "?";
    const wchar_t* w = patterns::fname_text(idx);
    std::string s;
    if (w)
        for (size_t i = 0; w[i] && i < 47; ++i) s += w[i] < 128 ? static_cast<char>(w[i]) : '?';
    return s.empty() ? "?" : s;
}

void inv_arrays(void* im) {
    if (!im) return;
    if (im != g_arrOwner) {
        g_arrOwner = im;
        g_arrCount = 0;
        uint32_t w[0x800 / 4];
        if (!read_block(im, w, sizeof w)) return;
        for (uint32_t i = 0; i + 2 < 0x800 / 4 && g_arrCount < 12; ++i) {
            const uint32_t data = w[i], n = w[i + 1], mx = w[i + 2];
            if (data < 0x10000 || (data & 3) || n == 0 || n > 64 || mx < n || mx > 4096) continue;
            uint32_t el[64];
            if (!read_block(reinterpret_cast<void*>(static_cast<uintptr_t>(data)), el, n * 4)) continue;
            std::string line;
            for (uint32_t k = 0; k < n; ++k) {
                char b[96];
                void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(el[k]));
                if (el[k] >= 0x10000 && patterns::object_class_name(p))
                    _snprintf_s(b, sizeof b, _TRUNCATE, " [%u]%s:%s", k, class_of(p).c_str(), obj_name(p).c_str());
                else
                    _snprintf_s(b, sizeof b, _TRUNCATE, " [%u]%d", k, static_cast<int32_t>(el[k]));
                if (line.size() < 1700) line += b;
            }
            BVR_LOG("[eve] inv+0x%03X (%u):%s", i * 4, n, line.c_str());
            ArrWatch& a = g_arr[g_arrCount++];
            a.off = i * 4;
            a.n = n;
            a.data = data;
            memcpy(a.prev, el, n * 4);
            a.have = true;
            i += 2;
        }
    }
    for (int j = 0; j < g_arrCount; ++j) {
        ArrWatch& a = g_arr[j];
        uint32_t cur[64];
        if (!read_block(reinterpret_cast<void*>(a.data), cur, a.n * 4)) continue;
        for (uint32_t k = 0; k < a.n; ++k)
            if (cur[k] != a.prev[k] && may_log())
                BVR_LOG("[eve] inv+0x%03X[%u]: %d -> %d", a.off, k, static_cast<int32_t>(a.prev[k]),
                        static_cast<int32_t>(cur[k]));
        memcpy(a.prev, cur, a.n * 4);
    }
}

// ---- the hands' animation clock (v4) -------------------------------------------
// Snapshots of the hands actor and its SkeletonInstance 0.25 s before X and
// at +0.25/+0.5/+1.0/+1.5/+2.0 s after it; floats that change across them are
// logged as a sequence. An animation time ramps; its rate holds steady.
constexpr int kSnapWords = 0x800 / 4;
struct Snap {
    float h[kSnapWords];
    float k[kSnapWords];
};
Snap g_snapRing[4];   // rolling pre-X history (one every ~80 ms)
int g_snapRingAt = 0;
uint64_t g_snapRingMs = 0;
Snap g_snap[6];
int g_snapTaken = 0;
uint64_t g_xMs = 0;

void take_snap(Snap& s) {
    void* h = hands::hands_actor();
    void* k = bones::skeleton_instance();
    if (!h || !read_block(h, s.h, sizeof s.h)) memset(s.h, 0, sizeof s.h);
    if (!k || !read_block(k, s.k, sizeof s.k)) memset(s.k, 0, sizeof s.k);
}

void clock_tick(bool xPressed) {
    const uint64_t now = GetTickCount64();
    if (now - g_snapRingMs >= 80) {
        g_snapRingMs = now;
        take_snap(g_snapRing[g_snapRingAt]);
        g_snapRingAt = (g_snapRingAt + 1) % 4;
    }
    if (xPressed && g_snapTaken == 0) {
        g_snap[0] = g_snapRing[(g_snapRingAt + 1) % 4]; // ~0.25 s before
        g_snapTaken = 1;
        g_xMs = now;
    }
    static const uint64_t kAt[6] = {0, 250, 500, 1000, 1500, 2000};
    if (g_snapTaken > 0 && g_snapTaken < 6 && now - g_xMs >= kAt[g_snapTaken]) take_snap(g_snap[g_snapTaken++]);
    if (g_snapTaken == 6) {
        g_snapTaken = 0;
        int lines = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < kSnapWords && lines < 80; ++i) {
                float v[6];
                bool diff = false, sane = true;
                for (int t = 0; t < 6; ++t) {
                    v[t] = pass ? g_snap[t].k[i] : g_snap[t].h[i];
                    if (!(fabsf(v[t]) < 1e6f) || (v[t] != 0.0f && fabsf(v[t]) < 1e-6f)) sane = false;
                    if (t && v[t] != v[0]) diff = true;
                }
                if (!diff || !sane) continue;
                ++lines;
                BVR_LOG("[eve] clock %s+0x%03X: %.4g | %.4g %.4g %.4g %.4g %.4g", pass ? "skel" : "hands",
                        i * 4, v[0], v[1], v[2], v[3], v[4], v[5]);
            }
    }
}

// Test: un-hide the syringe and park it 40 UU in front of the left eye, every
// frame, to see whether the mod can show it on its own (the whole design
// rests on this). Restores the flags and location when switched off.
void show_test() {
    const bool on = g_showTest.load() && g_hypo;
    uint8_t* h = static_cast<uint8_t*>(g_hypo);
    if (on && !g_showWas) {
        read_block(h + kHypoFlagsOff, &g_flagsSaved, 4);
        read_block(h + patterns::kActorLocOffset, g_locSaved, 12);
        BVR_LOG("[eve] show test ON (flags 0x%X)", g_flagsSaved);
    }
    if (!on && g_showWas && h) {
        write_block(h + kHypoFlagsOff, &g_flagsSaved, 4);
        write_block(h + patterns::kActorLocOffset, g_locSaved, 12);
        BVR_LOG("[eve] show test off - restored");
    }
    g_showWas = on;
    if (!on) return;
    float eye[3];
    int32_t rot[3];
    if (!camera::driven_eye_cam(0, eye, rot)) return;
    const float yaw = rot[1] * (3.14159265f / 32768.0f), pitch = rot[0] * (3.14159265f / 32768.0f);
    const float f[3] = {cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch)};
    const float loc[3] = {eye[0] + f[0] * 40.0f, eye[1] + f[1] * 40.0f, eye[2] + f[2] * 40.0f - 8.0f};
    uint32_t flags = 0;
    if (read_block(h + kHypoFlagsOff, &flags, 4)) {
        flags &= ~kHypoHiddenBit;
        write_block(h + kHypoFlagsOff, &flags, 4);
    }
    write_block(h + patterns::kActorLocOffset, loc, 12);
}

// ---- episodes ----------------------------------------------------------------
// From an input edge / holdable change / animation start until 1.5 s after the
// last of them with the rig settled: how far each of Jack's wrists and the
// attach bone moved, which holdables and attached actors appeared.
struct Episode {
    bool on = false;
    uint64_t startMs = 0, lastEventMs = 0;
    float lw0[3], rw0[3], a0[3];
    float lwPeak = 0, rwPeak = 0, aPeak = 0;
    std::string events;
};
Episode g_ep;

void ep_event(const char* what) {
    const uint64_t now = GetTickCount64();
    if (!g_ep.on) {
        g_ep = Episode{};
        g_ep.on = true;
        g_ep.startMs = now;
        if (!bones::live_wrists(g_ep.lw0, g_ep.rw0, g_ep.a0)) {
            memset(g_ep.lw0, 0, 12);
            memset(g_ep.rw0, 0, 12);
            memset(g_ep.a0, 0, 12);
        }
    }
    g_ep.lastEventMs = now;
    if (g_ep.events.size() < 400) {
        char b[96];
        _snprintf_s(b, sizeof b, _TRUNCATE, " +%ums %s;", static_cast<unsigned>(now - g_ep.startMs), what);
        g_ep.events += b;
    }
}

float dist3(const float a[3], const float b[3]) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

void ep_tick(bool animating) {
    if (!g_ep.on) return;
    float lw[3], rw[3], a[3];
    if (bones::live_wrists(lw, rw, a)) {
        g_ep.lwPeak = fmaxf(g_ep.lwPeak, dist3(lw, g_ep.lw0));
        g_ep.rwPeak = fmaxf(g_ep.rwPeak, dist3(rw, g_ep.rw0));
        g_ep.aPeak = fmaxf(g_ep.aPeak, dist3(a, g_ep.a0));
    }
    const uint64_t now = GetTickCount64();
    if (animating) g_ep.lastEventMs = now;
    if (now - g_ep.lastEventMs < 1000) return;
    g_ep.on = false;
    BVR_LOG("[eve] episode %u ms: Jack's L wrist moved %.1f UU, R wrist %.1f UU, attach bone %.1f UU "
            "| events:%s",
            static_cast<unsigned>(g_ep.lastEventMs - g_ep.startMs), g_ep.lwPeak, g_ep.rwPeak, g_ep.aPeak,
            g_ep.events.c_str());
}

// The syringe in the hands' frame: which drawn wrist it rides, 10x a second
// while an episode runs.
uint64_t g_hypoLogMs = 0;
void hypo_track() {
    if (!g_hypo || !g_ep.on) return;
    const uint64_t now = GetTickCount64();
    if (now - g_hypoLogMs < 100) return;
    g_hypoLogMs = now;
    float loc[3];
    if (!read_block(static_cast<uint8_t*>(g_hypo) + patterns::kActorLocOffset, loc, 12)) return;
    float lw[3], rw[3];
    const bool hl = bones::written_world(patterns::kBoneLWrist, lw);
    const bool hr = bones::written_world(patterns::kBoneRClusterFirst, rw);
    BVR_LOG("[eve] hypo +%ums at (%.1f %.1f %.1f): %.1f UU from the drawn L wrist, %.1f UU from the R wrist",
            static_cast<unsigned>(now - g_ep.startMs), loc[0], loc[1], loc[2],
            hl ? dist3(loc, lw) : -1.0f, hr ? dist3(loc, rw) : -1.0f);
}
int g_linesThisSec = 0;
uint64_t g_secMs = 0;

bool may_log() {
    const uint64_t now = GetTickCount64();
    if (now - g_secMs >= 1000) {
        g_secMs = now;
        g_linesThisSec = 0;
    }
    return ++g_linesThisSec <= 40;
}

void watch_tick(Watch& w, void* obj) {
    const uint64_t now = GetTickCount64();
    if (obj != w.obj) {
        if (w.obj || obj) BVR_LOG("[eve] watch %s -> %p (%s)", w.name, obj, class_of(obj).c_str());
        w.obj = obj;
        w.havePrev = false;
        memset(w.changes, 0, sizeof w.changes);
        memset(w.noisy, 0, sizeof w.noisy);
        w.boundMs = w.windowMs = now;
    }
    if (!obj) return;
    uint32_t cur[kMaxWords];
    if (!read_block(obj, cur, w.words * 4)) {
        w.havePrev = false;
        return;
    }
    if (now - w.windowMs >= 10000) {
        w.windowMs = now;
        memset(w.changes, 0, sizeof w.changes);
    }
    const bool warm = now - w.boundMs < 3000; // learn the noise first
    if (w.havePrev) {
        for (uint32_t i = 0; i < w.words; ++i) {
            if (cur[i] == w.prev[i] || w.noisy[i]) continue;
            if (w.changes[i] < 255) ++w.changes[i];
            if (w.changes[i] > 6) {
                w.noisy[i] = true;
                continue;
            }
            if (warm || !may_log()) continue;
            float fa, fb;
            memcpy(&fa, &w.prev[i], 4);
            memcpy(&fb, &cur[i], 4);
            BVR_LOG("[eve] %s+0x%03X: %d -> %d  (float %.4g -> %.4g)", w.name, i * 4,
                    static_cast<int32_t>(w.prev[i]), static_cast<int32_t>(cur[i]), fa, fb);
        }
    }
    memcpy(w.prev, cur, w.words * 4);
    w.havePrev = true;
}

// ---- inputs + holdable --------------------------------------------------------
uint16_t g_btnPrev = 0;
bool g_ltPrev = false, g_rtPrev = false, g_lbPrev = false, g_rbPrev = false;
void* g_holdPrev = reinterpret_cast<void*>(1);
std::string g_holdClass;
bool g_animPrev = false;
void* g_seenChild[64];
int g_seenCount = 0;

void input_edges() {
    uint16_t btn = 0;
    uint8_t lt = 0, rt = 0;
    bool lb = false, rb = false;
    bvr::input::last_composed_buttons(&btn);
    bvr::input::last_composed_triggers(&lt, &rt);
    bvr::input::last_composed_bumpers(&lb, &rb);
    const char* hand = hands::active_hand() == 0 ? "plasmid up" : "weapon up";
    struct B {
        uint16_t bit;
        const char* name;
    };
    static const B kBtn[] = {{0x4000, "X (reload/hack/EVE)"}, {0x2000, "B (med hypo)"},
                             {0x1000, "A (use)"}, {0x8000, "Y (jump)"}};
    for (const B& b : kBtn) {
        if ((btn & b.bit) && !(g_btnPrev & b.bit)) {
            BVR_LOG("[eve] press %s - %s, holding %s", b.name, hand, g_holdClass.c_str());
            ep_event(b.name);
        }
    }
    g_btnPrev = btn;
    auto edge = [&](bool now, bool& prev, const char* name) {
        if (now && !prev) {
            BVR_LOG("[eve] press %s - %s, holding %s", name, hand, g_holdClass.c_str());
            ep_event(name);
        }
        prev = now;
    };
    edge(lt >= 30, g_ltPrev, "LT (plasmid)");
    edge(rt >= 30, g_rtPrev, "RT (fire)");
    edge(lb, g_lbPrev, "LB (raise plasmid)");
    edge(rb, g_rbPrev, "RB (raise weapon)");
}


// =============================================================================
// THE HOLSTER (feature). Weapon hand at the hip + grip = Jack's EVE syringe in
// that hand (the game's own BioAmmoHypoTool, un-hidden and placed on the hand's
// gun socket every frame). Needle tip inside the plasmid forearm = it sticks,
// both hands buzz; weapon trigger = the game's REAL injection (X with the
// plasmid raised): it attaches the same syringe to the same socket, runs the
// plunger, and the EVE lands ~1.85 s later. Let go of the grip first and the
// syringe goes back. Casts the EVE cannot pay for are held back, so the game
// never injects on its own.
// =============================================================================
std::atomic<bool> g_holsterOn{true};
std::atomic<bool> g_blockAuto{true};
std::atomic<float> g_hipDropM{0.62f};  // below the eyes
std::atomic<float> g_hipSideM{0.20f};  // out to the weapon hand's side
std::atomic<float> g_hipFwdM{0.02f};   // forward of the eyes
std::atomic<float> g_zoneM{0.20f};     // grab radius
std::atomic<float> g_needleUu{14.0f};  // needle tip below the grip (syringe axis)
std::atomic<float> g_surgeAmt{1.0f};   // injection tremor strength (0 = off)
std::atomic<bool> g_cfgDirty{false};
bool g_cfgLoaded = false;

enum class Hs { Idle, Held, In, Injecting, Linger };
Hs g_hs = Hs::Idle;
std::atomic<int> g_hsUi{0};
Targets g_tg;
uint64_t g_tgMs = 0;
bool g_squeeze = false;
bool g_inZone = false;
bool g_zoneSqueeze = false; // this squeeze began in the holster: never reaches the game
bool g_drawSpent = false;     // this squeeze already drew (or was refused)
uint64_t g_squeezeRiseMs = 0; // when the current squeeze began (a grab may finish on arrival)
uint64_t g_missLogMs = 0;
float g_hipDistPrev = -1.0f;  // last tick's hand-to-holster distance (approach speed)
uint64_t g_hipDistMs = 0;
float g_approach = 0.0f;      // m/s toward the holster, smoothed
int g_xTries = 0;            // X presses sent for this injection
uint64_t g_xSentMs = 0;       // when X went to the game for this injection
bool g_sawAttach = false;     // the game took the syringe for this injection
// The injection surge: a tremor that builds while the plunger runs, peaks as
// the EVE lands and dies away in the hand.
uint64_t g_surgeStartMs = 0, g_surgeLandMs = 0;
bool g_trigPrev = false;
bool g_shown = false;
uint64_t g_stateMs = 0, g_lastBuzzMs = 0, g_xQueuedMs = 0;
constexpr uint64_t kLingerMs = 700; // the empty syringe stays in the hand this long
bool g_xQueued = false;
float g_bodyFwd[2] = {0.0f, -1.0f}; // XR horizontal body forward (x, z)
bool g_bodyInit = false;
float g_eveAtInject = 0.0f;
int g_socket = -1;
void* g_socketSkel = nullptr;

// Syringe discovery: from its attach (the first injection) or, before that, a
// bounded search of objects linked from the pawn, hands and plasmid manager.
void* g_hypoChecked[64];
int g_hypoCheckedAt = 0;
uint64_t g_hypoSearchMs = 0;
int g_hypoSearches = 0;
void* g_hypoSearchWorld = nullptr;

// EVE costs per plasmid class (learned from the drop at each cast).
struct Cost {
    char cls[48];
    float cost;
};
Cost g_costs[16];
int g_costCount = 0;
float g_evePrev = -1.0f;
uint64_t g_lastLtMs = 0;
std::atomic<float> g_eveUi{-1.0f}, g_costUi{-1.0f};
std::atomic<int> g_countUi{-1};

std::string g_curPlasmid;

constexpr int kSnapBytes = 0x800;
struct HidCand {
    uint16_t off;
    uint8_t mask;
    uint8_t flips; // bit 0: seen going hidden, bit 1: seen going visible
};
HidCand g_hidCand[32];
int g_hidCandN = -1; // -1 = no toggle observed yet
int g_hidToggles = 0;
int g_hidOff = -1;   // learned (or loaded) byte offset of bHidden
uint8_t g_hidMask = 0;
bool g_hidVerified = false; // the loaded offset agreed with a console toggle this session
uint8_t g_snapPre[kSnapBytes], g_snapPost[kSnapBytes];

void cfg_path(wchar_t* out, size_t n) { swprintf_s(out, n, L"%s\\eve.ini", bvr::log::data_dir()); }
void cfg_save() {
    wchar_t path[MAX_PATH];
    cfg_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# BioShock VR - EVE holster\n");
    fprintf(f, "holster=%d\nblockAutoInject=%d\nhipDropM=%.3f\nhipSideM=%.3f\nhipFwdM=%.3f\nzoneM=%.3f\nneedleUu=%.1f\nsurge=%.2f\nhiddenOff=%d\nhiddenMask=%d\n",
            g_holsterOn.load() ? 1 : 0, g_blockAuto.load() ? 1 : 0, g_hipDropM.load(), g_hipSideM.load(),
            g_hipFwdM.load(), g_zoneM.load(), g_needleUu.load(), g_surgeAmt.load(), g_hidOff, static_cast<int>(g_hidMask));
    fclose(f);
}
void cfg_load() {
    g_cfgLoaded = true;
    wchar_t path[MAX_PATH];
    cfg_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[48] = {};
        float v = 0.0f;
        if (line[0] == '#' || sscanf_s(line, "%47[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2) continue;
        if (!strcmp(key, "holster")) g_holsterOn.store(v != 0.0f);
        else if (!strcmp(key, "blockAutoInject")) g_blockAuto.store(v != 0.0f);
        else if (!strcmp(key, "hipDropM")) g_hipDropM.store(v);
        else if (!strcmp(key, "hipSideM")) g_hipSideM.store(v);
        else if (!strcmp(key, "hipFwdM")) g_hipFwdM.store(v);
        else if (!strcmp(key, "zoneM")) g_zoneM.store(v);
        else if (!strcmp(key, "needleUu")) g_needleUu.store(v);
        else if (!strcmp(key, "hiddenOff") && v >= 0.0f && v < kSnapBytes) g_hidOff = static_cast<int>(v);
        else if (!strcmp(key, "hiddenMask") && v >= 1.0f && v <= 128.0f) g_hidMask = static_cast<uint8_t>(v);
        else if (!strcmp(key, "surge")) g_surgeAmt.store(v < 0.0f ? 0.0f : v > 2.0f ? 2.0f : v);
    }
    fclose(f);
}

void* pawn_of() {
    void* h = hands::hands_actor();
    void* pawn = nullptr;
    if (h) read_block(static_cast<uint8_t*>(h) + patterns::kActorBaseOffset, &pawn, 4);
    return pawn;
}
float eve_level() {
    void* pawn = pawn_of();
    float v = -1.0f;
    if (!pawn || !read_block(static_cast<uint8_t*>(pawn) + patterns::kPawnEveOffset, &v, 4)) return -1.0f;
    return (v >= 0.0f && v < 10000.0f) ? v : -1.0f;
}
bool hypo_valid() { return g_hypo && class_of(g_hypo) == "BioAmmoHypoTool"; }
bool hypo_attached() {
    void* base = nullptr;
    return g_hypo && read_block(static_cast<uint8_t*>(g_hypo) + patterns::kHypoToolBaseOffset, &base, 4) && base;
}
// ---- The syringe's hidden flag, learned -------------------------------------
// The console `set ... bHidden` is the only proven show/hide, but it walks
// every object (too slow to repeat) and it is write-only: the game hides the
// syringe on its own hand events (the plasmid coming up during a draw among
// them) and the mod could not see that, so a held hypo sometimes was never
// drawn. Learn where the flag lives instead: snapshot the actor around each
// console toggle and keep the bits that always read hidden after a hide and
// visible after a show, and that flipped both ways. One survivor over several
// toggles is AActor::bHidden. From then on it is READ every frame, so a hide
// by the game is answered the same frame. Showing still goes through the
// console: in this build the property change also refreshes render-state
// fields (ENGINE_NOTES), which a raw bit write would skip. Learned once,
// saved to eve.ini, re-verified against the next console toggle each session.

bool hid_known() { return g_hidOff >= 0 && g_hidVerified; }
bool hid_read(bool* hidden) {
    uint8_t b = 0;
    if (!g_hypo || g_hidOff < 0 || !read_block(static_cast<uint8_t*>(g_hypo) + g_hidOff, &b, 1)) return false;
    *hidden = (b & g_hidMask) != 0;
    return true;
}

void hid_learn(bool hidden) {
    const int want = hidden ? 1 : 0;
    if (g_hidOff >= 0 && !g_hidVerified) { // a loaded offset: one toggle confirms or discards it
        const int got = (g_snapPost[g_hidOff] & g_hidMask) ? 1 : 0;
        const int was = (g_snapPre[g_hidOff] & g_hidMask) ? 1 : 0;
        if (got != want) {
            BVR_LOG("[eve] syringe hidden flag +0x%X/0x%02X disagreed with the console - relearning", g_hidOff,
                    g_hidMask);
            g_hidOff = -1;
            g_hidCandN = -1;
            g_hidToggles = 0;
        } else if (got != was) {
            g_hidVerified = true;
            BVR_LOG("[eve] syringe hidden flag +0x%X/0x%02X confirmed", g_hidOff, g_hidMask);
        }
        return;
    }
    if (g_hidOff >= 0) return;
    ++g_hidToggles;
    if (g_hidCandN < 0) { // first toggle: every bit that flipped into the wanted state
        g_hidCandN = 0;
        for (int i = 0; i < kSnapBytes && g_hidCandN < 32; ++i) {
            const uint8_t d = g_snapPre[i] ^ g_snapPost[i];
            // A flag, not a value: the aligned dword around it must change by
            // this one bit alone (the console's render-state words change by
            // whole values, and a pointer bit must never be mistaken for it).
            const int w = i & ~3;
            int changed = 0;
            for (int j = w; j < w + 4; ++j) changed += g_snapPre[j] != g_snapPost[j];
            if (changed != 1 || (d & (d - 1)) != 0) continue;
            for (int b = 0; b < 8 && d; ++b) {
                const uint8_t m = static_cast<uint8_t>(1u << b);
                if ((d & m) && ((g_snapPost[i] & m) ? 1 : 0) == want && g_hidCandN < 32)
                    g_hidCand[g_hidCandN++] = {static_cast<uint16_t>(i), m, static_cast<uint8_t>(hidden ? 1 : 2)};
            }
        }
        return;
    }
    int n = 0;
    for (int k = 0; k < g_hidCandN; ++k) {
        HidCand c = g_hidCand[k];
        const int got = (g_snapPost[c.off] & c.mask) ? 1 : 0;
        if (got != want) continue; // wrong after this toggle: not the flag
        if ((g_snapPre[c.off] ^ g_snapPost[c.off]) & c.mask) c.flips |= hidden ? 1 : 2;
        g_hidCand[n++] = c;
    }
    g_hidCandN = n;
    if (n == 0) { // nothing consistent (the actor changed?): start over
        g_hidCandN = -1;
        g_hidToggles = 0;
        return;
    }
    if (n == 1 && g_hidCand[0].flips == 3 && g_hidToggles >= 3) {
        g_hidOff = g_hidCand[0].off;
        g_hidMask = g_hidCand[0].mask;
        g_hidVerified = true;
        BVR_LOG("[eve] syringe hidden flag learned: +0x%X mask 0x%02X (after %d toggles)", g_hidOff, g_hidMask,
                g_hidToggles);
        cfg_save();
    }
}

void hypo_show(bool on) {
    if (on == g_shown) return;
    g_shown = on;
    const bool snap = g_hypo && read_block(g_hypo, g_snapPre, kSnapBytes);
    console_exec::run_engine(on ? "set BioAmmoHypoTool bHidden False" : "set BioAmmoHypoTool bHidden True");
    if (snap && read_block(g_hypo, g_snapPost, kSnapBytes)) hid_learn(!on);
}

// Held, needle in or lingering: the syringe must be visible. With the flag
// known, a hide by the game is undone the frame it happens (logged, so the
// event is on record); before that, re-show after hand changes.
uint64_t g_heldSinceMs = 0;
int g_gameHides = 0;
void keep_shown(uint64_t now) {
    if (hid_known()) {
        bool hidden = false;
        static uint64_t s_lastShowMs = 0;
        if (hid_read(&hidden) && hidden && now - s_lastShowMs >= 100) { // (a game that re-hides every frame
            s_lastShowMs = now;                                           //  must not get a console walk each)
            g_shown = false;
            hypo_show(true);
            if (++g_gameHides <= 6)
                BVR_LOG("[eve] the game hid the held syringe %llu ms into the draw (raised hand %d) - re-shown",
                        static_cast<unsigned long long>(now - g_heldSinceMs), hands::active_hand());
        }
        return;
    }
    // Not learned yet: re-show (console) a beat after the raised hand,
    // weapon or plasmid changes - each such toggle also teaches the learner.
    void* hold = nullptr;
    hands::current_holdable(&hold);
    const uintptr_t key = reinterpret_cast<uintptr_t>(hold) ^ (static_cast<uintptr_t>(hands::active_hand()) << 1) ^
                          std::hash<std::string>{}(g_curPlasmid);
    static uintptr_t s_key = 0;
    static int s_stage = 0;
    static uint64_t s_at = 0;
    if (key != s_key) {
        s_key = key;
        s_at = now;
        s_stage = 2;
    }
    const uint64_t due = s_stage == 2 ? 60 : 350;
    if (s_stage > 0 && now - s_at >= due) {
        --s_stage;
        g_shown = false;
        hypo_show(true);
    }
}

// Bounded breadth-first search for the syringe actor among objects linked
// from the hands, pawn and plasmid manager (pointer fields + TArray elements).
// The seeds are read 4 KB deep - the hands actor's own fields run past
// 0x5C0 - and engine plumbing (classes, packages, levels, assets, nav points)
// is never expanded, so the budget goes on gameplay objects.
bool plumbing(const std::string& c) {
    static const char* kSkip[] = {"Class", "Package", "Level", "LevelInfo", "ZoneInfo", "Texture", "Shader",
                                  "Material", "Model", "Font", "Sound", "StaticMesh", "SkeletalMesh", "Mesh",
                                  "DefaultPhysicsVolume", "PhysicsVolume", "GameReplicationInfo",
                                  "PlayerReplicationInfo", "FloorPoint", "PathNode", "PlayerPathNode",
                                  "PlayerStart", "Script", "WindowsViewport", "WindowsClient", "Console",
                                  "StaticMeshInstance", "SkeletalMeshInstance", "TriggerVolume", "Function",
                                  "State", "Property"};
    for (const char* k : kSkip)
        if (c == k) return true;
    return false;
}
void* search_hypo() {
    void* pawn = pawn_of();
    void* seeds[4] = {hands::hands_actor(), pawn, nullptr, camera::player_controller()};
    if (pawn) read_block(static_cast<uint8_t*>(pawn) + patterns::kPawnPlasmidManagerOffset, &seeds[2], 4);
    constexpr int kQ = 320;
    static void* queue[kQ];
    static int depth[kQ];
    int qn = 0, qi = 0;
    for (void* sd : seeds)
        if (sd && qn < kQ) {
            queue[qn] = sd;
            depth[qn++] = 0;
        }
    static uint32_t w[0x1000 / 4];
    while (qi < qn) {
        void* o = queue[qi];
        const int d = depth[qi++];
        const uint32_t bytes = d == 0 ? 0x1000 : 0x800;
        if (!read_block(o, w, bytes)) continue;
        auto visit = [&](uint32_t v) -> void* {
            if (v < 0x10000 || (v & 3)) return nullptr;
            void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(v));
            if (!patterns::object_class_name(p)) return nullptr;
            const std::string c = class_of(p);
            if (c == "BioAmmoHypoTool") return p;
            if (d < 2 && qn < kQ && !plumbing(c)) {
                for (int k = 0; k < qn; ++k)
                    if (queue[k] == p) return nullptr;
                queue[qn] = p;
                depth[qn++] = d + 1;
            }
            return nullptr;
        };
        for (uint32_t i = 0; i < bytes / 4; ++i) {
            if (void* hit = visit(w[i])) return hit;
            if (i + 2 < bytes / 4 && w[i] >= 0x10000 && !(w[i] & 3) && w[i + 1] && w[i + 1] <= 64 &&
                w[i + 2] >= w[i + 1] && w[i + 2] <= 4096) {
                uint32_t el[64];
                if (read_block(reinterpret_cast<void*>(static_cast<uintptr_t>(w[i])), el, w[i + 1] * 4))
                    for (uint32_t k = 0; k < w[i + 1]; ++k)
                        if (void* hit = visit(el[k])) return hit;
            }
        }
    }
    return nullptr;
}

// ---- hypo count (learned): the int in the plasmid manager (or its lists)
// that drops by exactly one across an injection and only then.
constexpr int kCountWords = 0x800 / 4;
struct CountSnap {
    uint32_t obj[kCountWords];
    uint32_t arr[6][32];
    uint32_t arrOff[6], arrN[6];
    uintptr_t arrData[6];
    int arrs = 0;
    bool ok = false;
};
CountSnap g_cs[6]; // rolling, every 500 ms
int g_csAt = 0;
uint64_t g_csMs = 0;
uint8_t g_cntScore[kCountWords + 6 * 32];
int g_cntLearned = -1; // index into the combined space
void* g_cntPm = nullptr;

void count_snap(CountSnap& c) {
    void* pawn = pawn_of();
    void* pm = nullptr;
    c.ok = false;
    if (!pawn || !read_block(static_cast<uint8_t*>(pawn) + patterns::kPawnPlasmidManagerOffset, &pm, 4) || !pm) return;
    if (pm != g_cntPm) {
        g_cntPm = pm;
        g_cntLearned = -1;
        memset(g_cntScore, 0, sizeof g_cntScore);
    }
    if (!read_block(pm, c.obj, sizeof c.obj)) return;
    c.arrs = 0;
    for (uint32_t i = 0; i + 2 < kCountWords && c.arrs < 6; ++i) {
        const uint32_t data = c.obj[i], n = c.obj[i + 1], mx = c.obj[i + 2];
        if (data < 0x10000 || (data & 3) || n == 0 || n > 32 || mx < n || mx > 4096) continue;
        if (!read_block(reinterpret_cast<void*>(static_cast<uintptr_t>(data)), c.arr[c.arrs], n * 4)) continue;
        c.arrOff[c.arrs] = i * 4;
        c.arrN[c.arrs] = n;
        c.arrData[c.arrs] = data;
        ++c.arrs;
        i += 2;
    }
    c.ok = true;
}
int count_value(const CountSnap& c, int idx) {
    if (idx < kCountWords) return static_cast<int32_t>(c.obj[idx]);
    const int a = (idx - kCountWords) / 32, k = (idx - kCountWords) % 32;
    if (a >= c.arrs || static_cast<uint32_t>(k) >= c.arrN[a]) return -1;
    return static_cast<int32_t>(c.arr[a][k]);
}
// An injection just landed: compare with the snapshot from ~3 s before.
void count_learn() {
    CountSnap now;
    count_snap(now);
    const CountSnap& old = g_cs[g_csAt]; // oldest in the ring
    if (!now.ok || !old.ok || now.arrs != old.arrs) return;
    int best = -1, bestScore = 0, cands = 0;
    for (int idx = 0; idx < kCountWords + 6 * 32; ++idx) {
        const int a = count_value(old, idx), b = count_value(now, idx);
        if (a >= 1 && a < 100 && b == a - 1) {
            if (g_cntScore[idx] < 255) ++g_cntScore[idx];
            ++cands;
        } else if (a != b) {
            g_cntScore[idx] = 0;
        }
        if (g_cntScore[idx] > bestScore) {
            bestScore = g_cntScore[idx];
            best = idx;
        }
    }
    if (bestScore >= 2 && best != g_cntLearned) {
        g_cntLearned = best;
        BVR_LOG("[eve] hypo count learned: plasmid manager %s 0x%X = %d", best < kCountWords ? "field" : "list",
                best < kCountWords ? best * 4 : best, count_value(now, best));
    }
    (void)cands;
}
int hypo_count() {
    if (g_cntLearned < 0) return -1;
    CountSnap c;
    count_snap(c);
    return c.ok ? count_value(c, g_cntLearned) : -1;
}

float cost_of(const std::string& cls) {
    for (int i = 0; i < g_costCount; ++i)
        if (cls == g_costs[i].cls) return g_costs[i].cost;
    return -1.0f;
}
void cost_learn(const std::string& cls, float drop) {
    if (cls.empty() || drop <= 0.5f) return;
    for (int i = 0; i < g_costCount; ++i)
        if (cls == g_costs[i].cls) {
            if (drop > g_costs[i].cost) g_costs[i].cost = drop;
            return;
        }
    if (g_costCount < 16) {
        strncpy_s(g_costs[g_costCount].cls, sizeof g_costs[g_costCount].cls, cls.c_str(), _TRUNCATE);
        g_costs[g_costCount++].cost = drop;
        BVR_LOG("[eve] %s costs %.1f EVE", cls.c_str(), drop);
    }
}

void v3_sub(const float a[3], const float b[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = a[i] - b[i];
}
float v3_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float seg_dist(const float p[3], const float a[3], const float b[3]) {
    float ab[3], ap[3];
    v3_sub(b, a, ab);
    v3_sub(p, a, ap);
    const float l2 = v3_dot(ab, ab);
    float t = l2 > 1e-6f ? v3_dot(ap, ab) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = a[i] + ab[i] * t - p[i];
    return sqrtf(v3_dot(c, c));
}

// FRotator from forward + up (UE basis; right implied - a proper frame).
void basis_to_rot(const float f[3], const float u[3], int32_t out[3]) {
    constexpr float k = 32768.0f / 3.14159265f;
    const float len2d = sqrtf(f[0] * f[0] + f[1] * f[1]);
    out[1] = static_cast<int32_t>(atan2f(f[1], f[0]) * k);
    out[0] = static_cast<int32_t>(atan2f(f[2], len2d) * k);
    out[2] = 0;
    if (len2d > 0.001f) {
        const float rn[3] = {-f[1] / len2d, f[0] / len2d, 0.0f};
        const float un[3] = {-f[2] * rn[1], f[2] * rn[0], f[0] * rn[1] - f[1] * rn[0]};
        out[2] = static_cast<int32_t>(atan2f(u[0] * rn[0] + u[1] * rn[1], u[0] * un[0] + u[1] * un[1] + u[2] * un[2]) * k);
    }
}

void set_state(Hs s, const char* why) {
    if (s == g_hs) return;
    static const char* kName[] = {"idle", "held", "needle in", "injecting", "lingering"};
    BVR_LOG("[eve] holster: %s -> %s (%s)", kName[static_cast<int>(g_hs)], kName[static_cast<int>(s)], why);
    bones::set_keep_weapon_socket(s == Hs::Injecting);
    g_hs = s;
    g_hsUi.store(static_cast<int>(s));
    g_stateMs = GetTickCount64();
}

// Where the weapon controller is relative to the holster, in BODY axes
// (metres: out to the weapon side, up, forward). False without poses.
//
// The holster hangs from the NECK, not the eyes: the eyes swing ~10 cm forward
// and down about the neck when you look down at your hip or lean into a
// fight, and a zone hung from the eyes swung with them - the hand arrived
// where the holster had been and the squeeze missed. The neck sits 10 cm below
// and 8 cm behind the eyes; the settings still read "from your eyes" with the
// head level, so existing positions carry over unchanged.
bool hip_offset(float out[3]) {
    bvr::vr::HeadPose head{}, hand{};
    if (!bvr::vr::peek_head_pose(head) || !bvr::vr::get_raw_hand_pose(1, false, hand)) return false;
    const float fz[3] = {0.0f, 0.0f, -1.0f};
    float f[3];
    bvr::xrmath::quat_rotate(head.qx, head.qy, head.qz, head.qw, fz, f);
    float hl = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (hl > 0.2f) {
        const float hx = f[0] / hl, hz = f[2] / hl;
        if (!g_bodyInit) {
            g_bodyFwd[0] = hx;
            g_bodyFwd[1] = hz;
            g_bodyInit = true;
        }
        // The body follows the head only past 35 deg (glancing down at the hip
        // or looking around must not swing the holster).
        const float c = g_bodyFwd[0] * hx + g_bodyFwd[1] * hz;
        if (c < 0.819f) {
            g_bodyFwd[0] += (hx - g_bodyFwd[0]) * 0.08f;
            g_bodyFwd[1] += (hz - g_bodyFwd[1]) * 0.08f;
            const float n = sqrtf(g_bodyFwd[0] * g_bodyFwd[0] + g_bodyFwd[1] * g_bodyFwd[1]);
            g_bodyFwd[0] /= n;
            g_bodyFwd[1] /= n;
        }
    }
    const float neckLocal[3] = {0.0f, -0.10f, 0.08f}; // XR head space: below, behind
    float nk[3];
    bvr::xrmath::quat_rotate(head.qx, head.qy, head.qz, head.qw, neckLocal, nk);
    const float neck[3] = {head.px + nk[0], head.py + nk[1], head.pz + nk[2]};
    const float side = bvr::input::left_handed() ? -1.0f : 1.0f; // weapon hand's side
    const float rx = -g_bodyFwd[1], rz = g_bodyFwd[0];               // body right (XR)
    const float drop = g_hipDropM.load() - 0.10f, fwd = g_hipFwdM.load() + 0.08f, out_ = g_hipSideM.load();
    const float hip[3] = {neck[0] + rx * side * out_ + g_bodyFwd[0] * fwd, neck[1] - drop,
                          neck[2] + rz * side * out_ + g_bodyFwd[1] * fwd};
    const float d[3] = {hand.px - hip[0], hand.py - hip[1], hand.pz - hip[2]};
    out[0] = (d[0] * rx + d[2] * rz) * side;
    out[1] = d[1];
    out[2] = d[0] * g_bodyFwd[0] + d[2] * g_bodyFwd[1];
    return true;
}

// Inside the holster's reach, scaled: an ellipsoid a quarter taller than it
// is wide (the hip does not sit at one exact height under a head that bobs).
// Kept tight on purpose - every grip squeeze inside it belongs to the holster,
// not to the weapon wheel.
bool in_reach(const float o[3], float scale) {
    const float r = g_zoneM.load() * scale, h = r * 1.25f;
    return (o[0] * o[0] + o[2] * o[2]) / (r * r) + o[1] * o[1] / (h * h) < 1.0f;
}

void buzz(int role, float a, int ms) { bvr::vr::haptic_pulse(role, a, ms); }

void put_away(const char* why) {
    if (g_hs == Hs::Idle) return;
    if (!hypo_attached()) hypo_show(false);
    g_xQueued = false;
    g_surgeStartMs = g_surgeLandMs = 0;
    set_state(Hs::Idle, why);
}

// Place the syringe on the weapon hand's gun socket - the same pose the game
// gives it when it attaches it there for its own injection.
void place_hypo() {
    if (!g_tg.socketOk || !g_hypo || hypo_attached()) return;
    int32_t rot[3];
    basis_to_rot(g_tg.sockF, g_tg.sockU, rot);
    write_block(static_cast<uint8_t*>(g_hypo) + patterns::kActorLocOffset, g_tg.socket, 12);
    write_block(static_cast<uint8_t*>(g_hypo) + patterns::kActorRotOffset, rot, 12);
}

// The needle tip: down the syringe's axis from the grip (its mesh runs along
// local -Z, plunger on top), as you see it.
void needle_tip(float out[3]) {
    float r[3]; // right = up x forward (UE, left-handed)
    const float* f = g_tg.sockF;
    const float* u = g_tg.sockU;
    r[0] = u[1] * f[2] - u[2] * f[1];
    r[1] = u[2] * f[0] - u[0] * f[2];
    r[2] = u[0] * f[1] - u[1] * f[0];
    const float lx = 5.3f, ly = -3.3f, lz = -g_needleUu.load(); // syringe-local (authored UU)
    for (int i = 0; i < 3; ++i) out[i] = g_tg.socket[i] + f[i] * lx + r[i] * ly + u[i] * lz;
}

// The plasmid forearm as a capsule: wrist -> elbow (arms on), else toward a
// point below the eyes, ~26 cm.
bool needle_in_arm(float slack) {
    if (!g_tg.socketOk || !g_tg.wristOk) return false;
    const float ws = g_tg.worldScale;
    float elbow[3];
    if (g_tg.elbowOk) {
        memcpy(elbow, g_tg.elbow, 12);
    } else {
        float eye[3];
        int32_t er[3];
        if (!camera::driven_eye_cam(0, eye, er)) return false;
        eye[2] -= 0.35f * ws;
        float d[3];
        v3_sub(eye, g_tg.wrist, d);
        const float l = sqrtf(v3_dot(d, d));
        if (l < 1e-3f) return false;
        for (int i = 0; i < 3; ++i) elbow[i] = g_tg.wrist[i] + d[i] / l * 0.26f * ws;
    }
    float tip[3];
    needle_tip(tip);
    return seg_dist(tip, g_tg.wrist, elbow) < 0.06f * ws * slack;
}

void holster_tick() {
    const uint64_t now = GetTickCount64();
    if (!g_cfgLoaded) cfg_load();
    if (g_cfgDirty.exchange(false)) cfg_save();

    // EVE bookkeeping (costs, auto-inject guard, count learning) runs always.
    const float eve = eve_level();
    g_eveUi.store(eve);
    {
        void* h = hands::hands_actor();
        void* ab = nullptr;
        if (h) read_block(static_cast<uint8_t*>(h) + patterns::kHandsCurrentAbilityOffset, &ab, 4);
        g_curPlasmid = ab ? class_of(ab) : std::string();
    }
    uint8_t ltRaw = 0, rtRaw = 0;
    bvr::input::last_unsuppressed_triggers(&ltRaw, &rtRaw);
    if (ltRaw >= 30) g_lastLtMs = now;
    if (now - g_csMs >= 500) {
        g_csMs = now;
        count_snap(g_cs[g_csAt]);
        g_csAt = (g_csAt + 1) % 6;
    }
    if (g_evePrev >= 0.0f && eve >= 0.0f) {
        if (eve < g_evePrev - 0.5f && now - g_lastLtMs < 1500) cost_learn(g_curPlasmid, g_evePrev - eve);
        if (eve > g_evePrev + 1.0f) count_learn(); // an injection (ours, X, or the game's) landed
    }
    g_evePrev = eve;
    g_countUi.store(hypo_count());
    const float cost = cost_of(g_curPlasmid);
    g_costUi.store(cost);
    bool supLt = false, supRt = false;
    struct Apply { // one hold-back slot: set it once, on every exit path
        bool& lt;
        bool& rt;
        ~Apply() {
            if (lt || rt) bvr::input::suppress_input(0, lt, rt, 150);
        }
    } apply{supLt, supRt};
    if (g_blockAuto.load() && hands::active_hand() == 0 && cost > 0.0f && eve >= 0.0f && eve + 0.01f < cost) {
        // Not enough EVE for this plasmid: the cast never reaches the game, so
        // it cannot inject on its own. An empty click instead.
        supLt = true;
        static bool s_ltPrev = false;
        const bool lt = ltRaw >= 30;
        if (lt && !s_ltPrev) {
            buzz(0, 0.5f, 25);
            buzz(0, 0.3f, 15);
        }
        s_ltPrev = lt;
    }

    // Find the syringe (once per world).
    {
        void* world = hands::hands_actor();
        if (world != g_hypoSearchWorld) {
            g_hypoSearchWorld = world;
            g_hypoSearches = 0;
            g_socket = -1;
            if (g_hypo && !hypo_valid()) g_hypo = nullptr;
        }
        if (!g_hypo && world && g_hypoSearches < 4 && now - g_hypoSearchMs > 3000) {
            g_hypoSearchMs = now;
            ++g_hypoSearches;
            g_hypo = search_hypo();
            BVR_LOG("[eve] syringe search %d: %p", g_hypoSearches, g_hypo);
        }
        void* sk = bones::skeleton_instance();
        if (sk && sk != g_socketSkel) {
            g_socketSkel = sk;
            const int byName = bones::hands_bone_index(L"Pistol");
            // "Pistol" is the attach name the game uses; when it is a socket
            // rather than a skeleton bone, the weapon attach bone is the same
            // place (every gun's R_Grip hangs there).
            g_socket = byName >= 0 ? byName : patterns::kBoneWeaponAttach;
            BVR_LOG("[eve] syringe socket: hands bone %d (%s)", g_socket,
                    byName >= 0 ? "named Pistol" : "no bone named Pistol - weapon attach bone");
        }
    }

    // The hand drive skips frames while the raised hand switches (the draw
    // itself raises the plasmid) - ride through that; only a real loss of the
    // rig (menus, cutscenes) puts the syringe away.
    const bool fresh = now - g_tgMs < 1500;
    if (!g_holsterOn.load() || !fresh) {
        if (g_holsterOn.load() && g_hs != Hs::Idle)
            BVR_LOG("[eve] holster: rig not drawn for %llu ms - putting the syringe away",
                    static_cast<unsigned long long>(now - g_tgMs));
        put_away(!fresh ? "no rig" : "holster off");
        static uint64_t s_noRigLogMs = 0;
        if (g_holsterOn.load() && bvr::vr::hand_squeeze(1) >= 0.70f && now - s_noRigLogMs > 1500) {
            s_noRigLogMs = now;
            BVR_LOG("[eve] holster: grip squeezed but the hands rig has not been drawn for %llu ms - "
                    "no draw possible", static_cast<unsigned long long>(now - g_tgMs));
        }
        return;
    }
    const bool rigNow = now - g_tgMs < 150;

    const float sq = bvr::vr::hand_squeeze(1);
    const bool was = g_squeeze;
    // Holding a syringe the release point is low: in a fight the grip relaxes
    // without meaning to let go, and 0.55 dropped the hypo mid-reach.
    const float release = g_hs == Hs::Idle ? 0.55f : 0.30f;
    g_squeeze = g_squeeze ? sq >= release : sq >= 0.70f;
    const bool rising = g_squeeze && !was;
    const bool trig = rtRaw >= 150;
    const bool trigRise = trig && !g_trigPrev;
    g_trigPrev = trig;

    switch (g_hs) {
    case Hs::Idle: {
        // Reach for the hip and squeeze in one movement, as you would in a
        // fight: the squeeze often lands a moment BEFORE the hand is inside
        // the reach. A squeeze that begins in the approach ring (1.5x the
        // reach) WHILE THE HAND IS MOVING TOWARD THE HOLSTER is held for it and
        // draws if the hand arrives within 450 ms. The motion is what makes it
        // a reach: a still hand squeezing near your side is the weapon wheel,
        // and the ring alone (v1, no motion test) swallowed it everywhere
        // below the shoulder.
        float o[3] = {};
        const bool posed = hip_offset(o);
        const float dist = posed ? sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) : -1.0f;
        if (posed && g_hipDistPrev >= 0.0f && now > g_hipDistMs) {
            const float dt = (now - g_hipDistMs) / 1000.0f;
            if (dt < 0.1f) {
                const float v = (g_hipDistPrev - dist) / dt;
                g_approach += (v - g_approach) * fminf(1.0f, dt / 0.04f);
            }
        }
        g_hipDistPrev = dist;
        g_hipDistMs = now;
        const bool zone = posed && in_reach(o, 1.0f);
        const bool ring = posed && in_reach(o, 1.5f);
        if (rising) {
            g_squeezeRiseMs = now;
            if (zone || (ring && g_approach > 0.35f)) g_zoneSqueeze = true;
        }
        if (!g_squeeze) g_zoneSqueeze = g_drawSpent = false;
        if (rising && !g_zoneSqueeze && posed && dist < 0.45f && now - g_missLogMs > 1500) {
            g_missLogMs = now; // near the hip but not taken: tuning data
            BVR_LOG("[eve] holster: squeeze %.0f cm from the holster (side %+.0f, up %+.0f, fwd %+.0f cm, "
                    "approach %.2f m/s) - not a draw",
                    dist * 100.0f, o[0] * 100.0f, o[1] * 100.0f, o[2] * 100.0f, g_approach);
        }
        // A holster squeeze never reaches the game, for its whole length
        // (leaving the zone with the grip still held used to open the wheel).
        if (zone || g_zoneSqueeze) bvr::vr::reserve_grip_bumper(1, true);
        if (zone && !g_squeeze && now - g_lastBuzzMs >= 90) { // in reach: a steady light buzz
            g_lastBuzzMs = now;
            buzz(1, 0.22f, 60);
        }
        g_inZone = zone;
        const bool grab = zone && g_squeeze && g_zoneSqueeze && !g_drawSpent && now - g_squeezeRiseMs <= 450;
        if (grab) g_drawSpent = true; // one draw per squeeze (a refusal must not repeat)
        if (g_zoneSqueeze && g_squeeze && !g_drawSpent && now - g_squeezeRiseMs > 450 && !zone) {
            g_drawSpent = true;
            BVR_LOG("[eve] holster: reach squeeze never arrived (%.0f cm away after 450 ms)", dist * 100.0f);
        }
        if (grab) {
            BVR_LOG("[eve] holster: grip at the hip (syringe %p, socket bone %d, socket %s)", g_hypo, g_socket,
                    g_tg.socketOk ? "ok" : "missing");
            const int cnt = hypo_count();
            if (cnt == 0 || !g_tg.socketOk) {
                buzz(1, 0.9f, 250); // one long buzz: nothing to draw
                BVR_LOG("[eve] holster: can't draw - %s (socket bone %d, wrist %s)",
                        cnt == 0 ? "no hypos" : "no socket pose", g_socket, g_tg.wristOk ? "ok" : "missing");
                break;
            }
            if (!g_hypo) {
                // The syringe has not been seen yet this session (the game may
                // only create it on first use): inject the classic way once -
                // its attach hands us the actor, and the holster is physical
                // from then on.
                BVR_LOG("[eve] holster: syringe unknown - first draw injects directly to find it");
                if (hands::active_hand() != 0) bvr::input::pulse_buttons(0x0100, 150);
                g_eveAtInject = eve;
                g_xQueued = true;
                g_xQueuedMs = now;
                g_xSentMs = 0;
                g_sawAttach = false;
                buzz(1, 0.6f, 60);
                set_state(Hs::Injecting, "first draw (finding the syringe)");
                break;
            }
            if (hands::active_hand() != 0) bvr::input::pulse_buttons(0x0100, 150); // LB: raise the plasmid
            hypo_show(true);
            place_hypo();
            buzz(1, 0.6f, 60);
            g_heldSinceMs = now;
            g_gameHides = 0;
            set_state(Hs::Held, "drawn from the hip");
        }
        break;
    }
    case Hs::Held:
    case Hs::In: {
        bvr::vr::reserve_grip_bumper(1, true);
        supRt = true; // the trigger is the plunger now
        if (!g_squeeze) {
            char why[48];
            _snprintf_s(why, sizeof why, _TRUNCATE, "let go (grip %.2f)", sq);
            put_away(why);
            break;
        }
        keep_shown(now);
        place_hypo();
        const bool in = rigNow && needle_in_arm(g_hs == Hs::In ? 1.5f : 1.0f);
        if (in && g_hs == Hs::Held) {
            buzz(0, 0.5f, 50);
            buzz(1, 0.5f, 50);
            set_state(Hs::In, "needle in the arm");
        } else if (!in && g_hs == Hs::In) {
            set_state(Hs::Held, "needle out");
        }
        if (g_hs == Hs::In && trigRise) {
            g_eveAtInject = eve;
            g_xQueued = true;
            g_xQueuedMs = now;
            g_xSentMs = 0;
            g_sawAttach = false;
            set_state(Hs::Injecting, "trigger");
        }
        break;
    }
    case Hs::Linger: {
        bvr::vr::reserve_grip_bumper(1, true);
        supRt = true;
        if (!hypo_attached()) keep_shown(now);
        place_hypo();
        if (now - g_lastBuzzMs > 110 && now - g_stateMs < 450) { // the rush fading out
            g_lastBuzzMs = now;
            const float a = 0.55f * expf(-(now - g_stateMs) / 220.0f);
            buzz(0, a, 35);
            buzz(1, a, 35);
        }
        if (now - g_stateMs > kLingerMs || !g_squeeze) {
            g_shown = true;
            hypo_show(false);
            g_surgeStartMs = g_surgeLandMs = 0;
            set_state(Hs::Idle, "put away");
        }
        break;
    }
    case Hs::Injecting: {
        bvr::vr::reserve_grip_bumper(1, true);
        supRt = true;
        // X only reaches a game that can act on it: plasmid raised AND the
        // hands at rest. Pressed while the plasmid is still coming up (a draw
        // with a gun out, or mid-run) the game drops it - the log had one
        // such "never took the syringe". Not taken within 700 ms: press again,
        // three times in all.
        int action = -1;
        if (void* h = hands::hands_actor())
            read_block(static_cast<uint8_t*>(h) + patterns::kHandsActionStateOffset, &action, 4);
        const bool ready = hands::active_hand() == 0 && action == 5;
        if (g_xQueued && ready) {
            bvr::input::pulse_buttons(0x4000, 150); // X with the plasmid raised: the game injects
            g_xQueued = false;
            g_xSentMs = now;
            g_xTries = 1;
            g_sawAttach = false;
            g_surgeStartMs = now; // the plunger starts: the surge builds
            g_surgeLandMs = 0;
        } else if (!g_xQueued && g_xSentMs && !g_sawAttach && g_xTries < 3 && now - g_xSentMs > 700 && ready) {
            ++g_xTries;
            g_xSentMs = now;
            g_surgeStartMs = now;
            bvr::input::pulse_buttons(0x4000, 150);
            BVR_LOG("[eve] injection: X not taken - pressing again (try %d)", g_xTries);
        }
        if (g_xQueued && now - g_xQueuedMs > 2000) {
            g_xQueued = false;
            BVR_LOG("[eve] injection: the hands never came to rest with the plasmid up (hand %d, action %d)",
                    hands::active_hand(), action);
        }
        if (hypo_attached()) g_sawAttach = true;
        else place_hypo(); // until the game takes it over
        if (now - g_lastBuzzMs > 120) { // a pulse that swells with the plunge
            g_lastBuzzMs = now;
            const float ramp = g_surgeStartMs ? fminf(1.0f, (now - g_surgeStartMs) / 1500.0f) : 0.0f;
            buzz(0, 0.22f + 0.25f * ramp, 40);
            buzz(1, 0.22f + 0.25f * ramp, 40);
        }
        const bool landed = eve >= 0.0f && eve > g_eveAtInject + 1.0f;
        if (landed) {
            buzz(0, 0.8f, 90);
            buzz(1, 0.8f, 90);
            // The game lets go of the syringe as the EVE lands and hides it;
            // keep the emptied hypo in the hand a moment longer.
            g_shown = false;
            hypo_show(true);
            place_hypo();
            g_surgeLandMs = now; // the rush: peak tremor, then it fades in the hand
            set_state(Hs::Linger, "EVE in");
        } else {
            // Refused = the game never took the syringe within 1.2 s of X, or
            // took it and let go with no EVE. Timed from X, not from the
            // trigger: raising the plasmid first can take most of a second,
            // and the old 1.6 s-from-trigger test put a good injection away.
            const bool neverTook = g_xSentMs && !g_sawAttach && g_hypo && g_xTries >= 3 && now - g_xSentMs > 900;
            const bool letGoEmpty = g_sawAttach && !hypo_attached() && now - g_xSentMs > 2600;
            if (neverTook || letGoEmpty || now - g_stateMs > 6000) {
                buzz(1, 0.4f, 20);
                BVR_LOG("[eve] injection refused (%s)", neverTook    ? "the game never took the syringe - EVE full?"
                                                        : letGoEmpty ? "the game let go with no EVE"
                                                                     : "timed out");
                put_away("refused");
            }
        }
        break;
    }
    }
}

} // namespace

bool probe_on() { return g_probe.load(std::memory_order_relaxed); }

// The injection surge, as a small rotation/translation of the weapon hand.
// A tremor (a few incommensurate 8-17 Hz sines, so it never reads as a loop)
// whose envelope builds over the plunge, jumps as the EVE lands, with one
// jolt of the wrist, then dies away over the linger. A few degrees at most:
// felt more than seen.
bool surge(float* pitchDeg, float* yawDeg, float* rollDeg, float* backCm) {
    const float amt = g_surgeAmt.load(std::memory_order_relaxed);
    if (amt <= 0.0f || !g_surgeStartMs || (g_hs != Hs::Injecting && g_hs != Hs::Linger)) return false;
    const uint64_t now = GetTickCount64();
    float env, jolt = 0.0f;
    if (!g_surgeLandMs) {
        const float t = (now - g_surgeStartMs) / 1000.0f;
        env = 0.15f + 0.45f * fminf(1.0f, t / 1.6f); // the plunge: a growing shiver
    } else {
        const float t = (now - g_surgeLandMs) / 1000.0f;
        env = 1.0f * expf(-t / 0.35f) + 0.05f;    // the rush, fading
        jolt = expf(-t / 0.09f);                  // one sharp jerk as it hits
        if (t > 1.2f) return false;
    }
    env *= amt;
    const float t = (now % 100000) / 1000.0f;
    constexpr float k2Pi = 6.2831853f;
    *pitchDeg = env * 1.4f * (sinf(k2Pi * 11.0f * t) + 0.6f * sinf(k2Pi * 17.3f * t + 1.3f)) / 1.6f +
                jolt * 3.0f * amt;
    *yawDeg = env * 1.0f * (sinf(k2Pi * 13.1f * t + 0.7f) + 0.5f * sinf(k2Pi * 7.9f * t + 2.1f)) / 1.5f;
    *rollDeg = env * 0.9f * sinf(k2Pi * 9.7f * t + 0.4f) - jolt * 1.5f * amt;
    *backCm = env * 0.25f * sinf(k2Pi * 15.2f * t + 2.6f) + jolt * 0.6f * amt;
    return true;
}

void on_attach(void* parent, void* child) {
    if (!child || GetCurrentThreadId() != g_gameTid) return;
    if (parent == hands::hands_actor() && child != g_hypo) {
        bool seen = false;
        for (void* c : g_hypoChecked) seen |= c == child;
        if (!seen) {
            g_hypoChecked[g_hypoCheckedAt] = child;
            g_hypoCheckedAt = (g_hypoCheckedAt + 1) % 64;
            if (class_of(child) == "BioAmmoHypoTool") {
                g_hypo = child;
                BVR_LOG("[eve] syringe found on attach: %p", child);
            }
        }
    }
    if (!probe_on()) return;
    void* hold = nullptr;
    hands::current_holdable(&hold);
    void* handsA = hands::hands_actor();
    void* pawn = nullptr;
    if (handsA) read_block(static_cast<uint8_t*>(handsA) + patterns::kActorBaseOffset, &pawn, 4);
    const char* who = parent == handsA ? "hands" : parent == hold ? "holdable" : parent == pawn ? "pawn" : nullptr;
    if (!who) return;
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seenChild[i] == child) return;
    if (g_seenCount < 64) g_seenChild[g_seenCount++] = child;
    const std::string cls = class_of(child);
    BVR_LOG("[eve] attached to %s: %s (%p)", who, cls.c_str(), child);
    if (cls == "BioAmmoHypoTool") {
        if (child != g_hypo) {
            g_hypo = child;
            g_hypoDumped = false;
        }
    }
    ep_event(("attach " + cls).c_str());
}

void set_targets(const Targets& t) {
    g_tg = t;
    g_tgMs = GetTickCount64();
    // Right after the rig is drawn: the syringe lands on this frame's hand.
    if (g_hs != Hs::Idle && g_shown) place_hypo(); // (no-op while the game has it attached)
}
int socket_bone() { return g_socket; }

void tick() {
    g_gameTid = GetCurrentThreadId();
    holster_tick();
    if (!probe_on()) {
        g_ep.on = false;
        return;
    }
    void* handsA = hands::hands_actor();
    void* pawn = nullptr;
    if (handsA) read_block(static_cast<uint8_t*>(handsA) + patterns::kActorBaseOffset, &pawn, 4);
    void* hold = nullptr;
    hands::current_holdable(&hold);
    if (hold != g_holdPrev) {
        g_holdClass = class_of(hold);
        if (g_holdPrev != reinterpret_cast<void*>(1)) {
            BVR_LOG("[eve] holdable -> %s (%p)", g_holdClass.c_str(), hold);
            ep_event(("holdable " + g_holdClass).c_str());
        }
        g_holdPrev = hold;
    }
    input_edges();
    const bool anim = bones::ref_animating_now();
    if (anim && !g_animPrev) ep_event("rig animating");
    g_animPrev = anim;
    ep_tick(anim);
    if (g_hypo && !g_hypoDumped) {
        g_hypoDumped = true;
        BVR_LOG("[eve] syringe found: BioAmmoHypoTool %p", g_hypo);
        bones::log_skeleton(g_hypo, "eve");
        census(g_hypo, "hypo", 0x800);
    }
    census(pawn, "pawn", 0x1000);
    census(camera::player_controller(), "pc", 0x1000);
    hypo_track();
    {
        void* im = nullptr;
        if (pawn) read_block(static_cast<uint8_t*>(pawn) + 0x948, &im, 4); // InventoryManager (probe v2)
        static void* s_imDone = nullptr;
        if (im && im != s_imDone) {
            s_imDone = im;
            BVR_LOG("[eve] InventoryManager %p (%s)", im, class_of(im).c_str());
            census_arrays(im, "inv", 0x800);
        }
    }
    if (g_hypo) {
        static int32_t s_bone = -1;
        int32_t bone = 0;
        if (read_block(static_cast<uint8_t*>(g_hypo) + kHypoBoneOff, &bone, 4) && bone != s_bone) {
            s_bone = bone;
            const wchar_t* bn = bone ? patterns::fname_text(bone) : nullptr;
            BVR_LOG("[eve] syringe hangs from bone %d = %S", bone, bn ? bn : L"(none)");
        }
    }
    show_test();
    {
        void* im = nullptr;
        if (pawn) read_block(static_cast<uint8_t*>(pawn) + 0x948, &im, 4);
        inv_arrays(im);
    }
    {
        static uint16_t s_prevBtn = 0;
        uint16_t btn = 0;
        bvr::input::last_composed_buttons(&btn);
        const bool xEdge = (btn & 0x4000) && !(s_prevBtn & 0x4000) && hands::active_hand() == 0;
        s_prevBtn = btn;
        clock_tick(xEdge);
    }
    {
        const int req = g_hiddenReq.exchange(0);
        if (req) {
            const char* cmd = req == 1 ? "set BioAmmoHypoTool bHidden False" : "set BioAmmoHypoTool bHidden True";
            BVR_LOG("[eve] console: %s", cmd);
            console_exec::run_engine(cmd);
        }
    }
    watch_tick(g_watch[0], pawn);
    watch_tick(g_watch[1], camera::player_controller());
    watch_tick(g_watch[2], handsA);
    watch_tick(g_watch[3], hold);
    watch_tick(g_watch[4], g_hypo);
    for (int k = 0; k < kLinks; ++k) watch_tick(g_watch[5 + k], k < g_linkCount ? g_link[k] : nullptr);
}

void draw_debug_ui() {
    if (ImGui::CollapsingHeader("EVE holster")) {
        bool b = g_holsterOn.load();
        if (ImGui::Checkbox("EVE holster at the hip (grip to draw, needle in your arm, trigger)", &b)) {
            g_holsterOn.store(b);
            g_cfgDirty.store(true);
        }
        b = g_blockAuto.load();
        if (ImGui::Checkbox("No automatic EVE injection (a cast you can't afford just clicks)", &b)) {
            g_blockAuto.store(b);
            g_cfgDirty.store(true);
        }
        ImGui::TextDisabled("Holster position, from your eyes (on your weapon hand's side):");
        float v = g_hipDropM.load() * 100.0f;
        if (ImGui::SliderFloat("down (cm)", &v, 30.0f, 90.0f, "%.0f")) {
            g_hipDropM.store(v / 100.0f);
            g_cfgDirty.store(true);
        }
        v = g_hipSideM.load() * 100.0f;
        if (ImGui::SliderFloat("out to the side (cm)", &v, 0.0f, 40.0f, "%.0f")) {
            g_hipSideM.store(v / 100.0f);
            g_cfgDirty.store(true);
        }
        v = g_hipFwdM.load() * 100.0f;
        if (ImGui::SliderFloat("forward (cm)", &v, -25.0f, 25.0f, "%.0f")) {
            g_hipFwdM.store(v / 100.0f);
            g_cfgDirty.store(true);
        }
        v = g_zoneM.load() * 100.0f;
        if (ImGui::SliderFloat("reach (cm)", &v, 6.0f, 30.0f, "%.0f")) {
            g_zoneM.store(v / 100.0f);
            g_cfgDirty.store(true);
        }
        v = g_surgeAmt.load();
        if (ImGui::SliderFloat("injection surge (hand shake)", &v, 0.0f, 2.0f, "%.2f")) {
            g_surgeAmt.store(v);
            g_cfgDirty.store(true);
        }
        static const char* kState[] = {"ready", "holding a hypo", "needle in - pull the trigger", "injecting",
                                       "done"};
        const int st = g_hsUi.load();
        const int cnt = g_countUi.load();
        ImGui::Text("%s | EVE %.0f | hypos %s", kState[st < 0 || st > 4 ? 0 : st], g_eveUi.load(),
                    cnt >= 0 ? std::to_string(cnt).c_str() : "(learned after two injections)");
        if (!g_hypo) ImGui::TextDisabled("Syringe not found yet - your first draw injects straight away and finds it.");
        if (bvr::overlay::dev_tools()) {
            float n = g_needleUu.load();
            if (ImGui::SliderFloat("needle length (UU)", &n, 4.0f, 30.0f, "%.1f")) {
                g_needleUu.store(n);
                g_cfgDirty.store(true);
            }
            ImGui::Text("socket bone %d | syringe %p | plasmid %s costs %.1f", g_socket, g_hypo,
                        g_curPlasmid.empty() ? "-" : g_curPlasmid.c_str(), g_costUi.load());
        }
    }
    if (!bvr::overlay::dev_tools()) return;
    if (!ImGui::CollapsingHeader("EVE probe")) return;
    bool on = g_probe.load();
    if (ImGui::Checkbox("Log EVE / hypo / injection facts to bioshockvr.log", &on)) {
        g_probe.store(on);
        g_seenCount = 0;
        g_censusCount = 0;
        g_linkCount = 0;
        g_hypoDumped = false;
        BVR_LOG("[eve] probe %s", on ? "ON" : "off");
    }
    ImGui::TextDisabled("Read-only. With it on: inject EVE with a plasmid raised (X), cast until");
    ImGui::TextDisabled("empty and cast again, pick up an EVE hypo, use a med hypo, reload a gun.");
    bool st = g_showTest.load();
    if (ImGui::Checkbox("TEST: show the syringe in front of me (inject once first)", &st)) g_showTest.store(st);
    if (ImGui::Button("TEST: console bHidden False")) g_hiddenReq.store(1);
    ImGui::SameLine();
    if (ImGui::Button("TEST: console bHidden True")) g_hiddenReq.store(2);
}

} // namespace bvr::b1r::eve
