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
Watch g_watch[4] = {{"pawn", 0x1000 / 4}, {"pc", 0x1000 / 4}, {"hands", 0x800 / 4}, {"holdable", 0x800 / 4}};
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
    if (now - g_ep.lastEventMs < 1500) return;
    g_ep.on = false;
    BVR_LOG("[eve] episode %u ms: Jack's L wrist moved %.1f UU, R wrist %.1f UU, attach bone %.1f UU "
            "| events:%s",
            static_cast<unsigned>(g_ep.lastEventMs - g_ep.startMs), g_ep.lwPeak, g_ep.rwPeak, g_ep.aPeak,
            g_ep.events.c_str());
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
    watch_tick(g_watch[0], pawn);
    watch_tick(g_watch[1], camera::player_controller());
    watch_tick(g_watch[2], handsA);
    watch_tick(g_watch[3], hold);
}

void draw_debug_ui() {
    if (!bvr::overlay::dev_tools()) return;
    if (!ImGui::CollapsingHeader("EVE probe")) return;
    bool on = g_probe.load();
    if (ImGui::Checkbox("Log EVE / hypo / injection facts to bioshockvr.log", &on)) {
        g_probe.store(on);
        g_seenCount = 0;
        BVR_LOG("[eve] probe %s", on ? "ON" : "off");
    }
    ImGui::TextDisabled("Read-only. With it on: inject EVE with a plasmid raised (X), cast until");
    ImGui::TextDisabled("empty and cast again, pick up an EVE hypo, use a med hypo, reload a gun.");
}

} // namespace bvr::b1r::eve
