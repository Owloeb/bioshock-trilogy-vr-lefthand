#include "game/bioshock1r/eve.h"

#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
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

} // namespace

bool probe_on() { return g_probe.load(std::memory_order_relaxed); }

void on_attach(void* parent, void* child) {
    if (!probe_on() || !child || GetCurrentThreadId() != g_gameTid) return;
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

void tick() {
    g_gameTid = GetCurrentThreadId();
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
    watch_tick(g_watch[0], pawn);
    watch_tick(g_watch[1], camera::player_controller());
    watch_tick(g_watch[2], handsA);
    watch_tick(g_watch[3], hold);
    watch_tick(g_watch[4], g_hypo);
    for (int k = 0; k < kLinks; ++k) watch_tick(g_watch[5 + k], k < g_linkCount ? g_link[k] : nullptr);
}

void draw_debug_ui() {
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
}

} // namespace bvr::b1r::eve
