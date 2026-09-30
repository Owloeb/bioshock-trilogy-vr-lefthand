#include "game/bioshock1r/twohand.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/hands.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace bvr::b1r::twohand {
namespace {

// ---- settings (twohand.ini) --------------------------------------------------
std::atomic<bool> g_offHand{true};      // draw the off hand at its controller
std::atomic<bool> g_twoHand{true};      // two-handed grip
std::atomic<bool> g_buzz{true};         // haptic while in the grab zone
std::atomic<float> g_grabCm{12.0f};     // enter radius (BioVR default)
std::atomic<float> g_releaseCm{35.0f};  // slide-along release distance
std::atomic<float> g_buzzAmp{0.35f};

constexpr float kSqueezePress = 0.70f;  // same hysteresis as the bumper composer
constexpr float kSqueezeRelease = 0.55f;
constexpr int kBuzzMs = 60;             // pulse length ...
constexpr uint64_t kBuzzEveryMs = 50;   // ... re-fired faster, so it reads continuous
constexpr uint64_t kCaptureTimeoutMs = 30000;

struct GrabPoint {
    float g[3];   // off-hand grip position, weapon-grip local frame (m)
    float rel[4]; // off-hand grip orientation, same frame
};

// Keyed "<WeaponClass>@<setup>". Game thread writes; the UI reads under the lock.
std::mutex g_mx;
std::map<std::string, GrabPoint> g_points;
bool g_loaded = false;

// Live state (game thread), mirrored to atomics for the UI.
std::atomic<bool> g_gripped{false};
std::atomic<bool> g_eligible{false};
std::atomic<float> g_distCm{-1.0f};
std::atomic<bool> g_captureArmed{false};
std::atomic<int> g_clearRequest{0};
std::atomic<uint64_t> g_captureStartMs{0};
bool g_squeezeLatched = false;
bool g_reservedLast = false;
uint64_t g_lastBuzzMs = 0;
char g_keyUi[96] = "";
std::atomic<bool> g_saveRequest{false};

// Grab points are recorded against what you SEE, so each hand-placement mode
// keeps its own set: the grip-pose placement ("G") moves both drawn hands, and
// switching back must find the old points exactly where they were.
const char* setup_tag() {
    const bool g = hands::grip_placement();
    if (!bvr::input::left_handed()) return g ? "RG" : "R";
    if (bvr::input::mirror_viewmodel()) return g ? "LMG" : "LM";
    return g ? "LG" : "L";
}

void ini_path(wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%s\\twohand.ini", bvr::log::data_dir());
}

void save() {
    wchar_t path[MAX_PATH];
    ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[twohand] could not write twohand.ini");
        return;
    }
    fprintf(f, "# BioShock VR - off hand + two-handed grip\n");
    fprintf(f, "offHand=%d\ntwoHand=%d\nbuzz=%d\ngrabCm=%.1f\nslideOffCm=%.1f\nbuzzAmp=%.2f\n",
            g_offHand.load() ? 1 : 0, g_twoHand.load() ? 1 : 0, g_buzz.load() ? 1 : 0,
            g_grabCm.load(), g_releaseCm.load(), g_buzzAmp.load());
    fprintf(f, "# grab points: <Weapon>@<R|L|LM> = x y z (m, weapon grip frame) qx qy qz qw\n");
    std::lock_guard<std::mutex> lk(g_mx);
    for (const auto& [k, p] : g_points)
        fprintf(f, "%s = %.4f %.4f %.4f %.5f %.5f %.5f %.5f\n", k.c_str(), p.g[0], p.g[1],
                p.g[2], p.rel[0], p.rel[1], p.rel[2], p.rel[3]);
    fclose(f);
}

void load() {
    g_loaded = true;
    wchar_t path[MAX_PATH];
    ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    char line[256];
    int points = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char key[96] = {};
        GrabPoint p{};
        if (sscanf_s(line, "%95[^= ] = %f %f %f %f %f %f %f", key,
                     static_cast<unsigned>(sizeof key), &p.g[0], &p.g[1], &p.g[2], &p.rel[0],
                     &p.rel[1], &p.rel[2], &p.rel[3]) == 8 &&
            strchr(key, '@')) {
            std::lock_guard<std::mutex> lk(g_mx);
            g_points[key] = p;
            ++points;
            continue;
        }
        float v = 0.0f;
        if (sscanf_s(line, "%95[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2)
            continue;
        if (strcmp(key, "offHand") == 0) g_offHand.store(v != 0.0f);
        else if (strcmp(key, "twoHand") == 0) g_twoHand.store(v != 0.0f);
        else if (strcmp(key, "buzz") == 0) g_buzz.store(v != 0.0f);
        else if (strcmp(key, "grabCm") == 0) g_grabCm.store(v);
        else if (strcmp(key, "slideOffCm") == 0) g_releaseCm.store(v); // v2 key: the old
        // 20 cm "releaseCm" is deliberately ignored - the grip got stickier
        else if (strcmp(key, "buzzAmp") == 0) g_buzzAmp.store(v);
    }
    fclose(f);
    BVR_LOG("[twohand] loaded %d grab point(s)", points);
}

void let_go(const char* why) {
    if (g_gripped.load(std::memory_order_relaxed))
        BVR_LOG("[twohand] let go - %s", why);
    g_gripped.store(false, std::memory_order_relaxed);
}

void idle(const char* why) {
    let_go(why);
    g_eligible.store(false, std::memory_order_relaxed);
    g_distCm.store(-1.0f, std::memory_order_relaxed);
    bvr::vr::set_two_hand_grip(false, nullptr, nullptr);
    g_reservedLast = false;
}

void conj_rotate(const float q[4], const float v[3], float out[3]) {
    float qc[4];
    bvr::xrmath::quat_conj(q, qc);
    bvr::xrmath::quat_rotate(qc[0], qc[1], qc[2], qc[3], v, out);
}

} // namespace

bool off_hand_enabled() { return g_offHand.load(std::memory_order_relaxed); }
bool gripped() { return g_gripped.load(std::memory_order_relaxed); }
bool preview_grip() {
    return g_captureArmed.load(std::memory_order_relaxed) ||
           g_eligible.load(std::memory_order_relaxed);
}

void tick(bool weaponRaised, bool gameplay) {
    if (!g_loaded) load();
    if (g_saveRequest.exchange(false)) save();

    std::string key = aim::active_weapon_key();
    if (key.empty()) key = "UnknownWeapon";
    key += "@";
    key += setup_tag();
    strncpy_s(g_keyUi, key.c_str(), _TRUNCATE);

    if (g_clearRequest.exchange(0) == 1) {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_points.erase(key);
        }
        save();
        BVR_LOG("[twohand] grab point cleared for %s", key.c_str());
    }

    // Raw squeeze of the OFF (plasmid-role) hand, own hysteresis.
    const float sq = bvr::vr::hand_squeeze(0);
    const bool wasLatched = g_squeezeLatched;
    g_squeezeLatched = g_squeezeLatched ? sq >= kSqueezeRelease : sq >= kSqueezePress;
    const bool rising = g_squeezeLatched && !wasLatched;

    if (g_captureArmed.load(std::memory_order_relaxed) &&
        GetTickCount64() - g_captureStartMs.load() > kCaptureTimeoutMs) {
        g_captureArmed.store(false);
        BVR_LOG("[twohand] grab point capture timed out");
    }
    const bool capture = g_captureArmed.load(std::memory_order_relaxed);
    if (!gameplay || !weaponRaised || (!g_twoHand.load() && !capture)) {
        idle(!gameplay ? "left gameplay" : !weaponRaised ? "weapon lowered" : "feature off");
        return;
    }

    bvr::vr::HeadPose rear{}, front{};
    if (!bvr::vr::get_raw_hand_pose(1, false, rear) || !bvr::vr::get_raw_hand_pose(0, false, front)) {
        idle("controller not tracked");
        return;
    }
    const float rp[3] = {rear.px, rear.py, rear.pz};
    const float rq[4] = {rear.qx, rear.qy, rear.qz, rear.qw};
    const float fp[3] = {front.px, front.py, front.pz};
    const float fq[4] = {front.qx, front.qy, front.qz, front.qw};
    const uint64_t now = GetTickCount64();

    // ---- capture: "Set grab point" armed, the next off-hand squeeze records --
    if (capture) {
        bvr::vr::set_two_hand_grip(false, nullptr, nullptr);
        let_go("recording a grab point");
        bvr::vr::reserve_grip_bumper(0, true); // this squeeze must not raise plasmids
        g_reservedLast = true;
        if (!rising) return;
        GrabPoint p{};
        const float d[3] = {fp[0] - rp[0], fp[1] - rp[1], fp[2] - rp[2]};
        conj_rotate(rq, d, p.g);
        float rqc[4];
        bvr::xrmath::quat_conj(rq, rqc);
        bvr::xrmath::quat_mul(rqc, fq, p.rel);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_points[key] = p;
        }
        g_captureArmed.store(false);
        save();
        BVR_LOG("[twohand] grab point set for %s: (%.3f %.3f %.3f) m from the weapon hand",
                key.c_str(), p.g[0], p.g[1], p.g[2]);
        bvr::vr::haptic_pulse(0, 0.8f, 120);
        bvr::vr::haptic_pulse(1, 0.8f, 120);
        return;
    }

    GrabPoint p{};
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_points.find(key);
        if (it == g_points.end()) {
            idle("no grab point for this weapon");
            return;
        }
        p = it->second;
    }

    // Distance from the off hand to the grab point on the UNROTATED gun.
    float gw[3];
    bvr::xrmath::quat_rotate(rq[0], rq[1], rq[2], rq[3], p.g, gw);
    const float dx = fp[0] - (rp[0] + gw[0]), dy = fp[1] - (rp[1] + gw[1]),
                dz = fp[2] - (rp[2] + gw[2]);
    const float distCm = sqrtf(dx * dx + dy * dy + dz * dz) * 100.0f;
    g_distCm.store(distCm, std::memory_order_relaxed);

    bool held = g_gripped.load(std::memory_order_relaxed);
    const bool inZone = distCm < g_grabCm.load();
    if (!held && inZone && rising) {
        held = true;
        BVR_LOG("[twohand] gripped %s (%.1f cm from the grab point)", key.c_str(), distCm);
    }
    // Let go the instant the grip is released (same hysteresis as the bumper
    // composer). A grace period here read as lag in the headset.
    if (held && !g_squeezeLatched) {
        held = false;
        let_go("the grip came up");
    }
    if (held) {
        // Held, the gun turns to meet the hand, so only sliding ALONG it can
        // take the hand off the grab point: compare hand-to-hand distance with
        // the recorded one.
        const float span = sqrtf((fp[0] - rp[0]) * (fp[0] - rp[0]) +
                                 (fp[1] - rp[1]) * (fp[1] - rp[1]) +
                                 (fp[2] - rp[2]) * (fp[2] - rp[2]));
        const float rec = sqrtf(p.g[0] * p.g[0] + p.g[1] * p.g[1] + p.g[2] * p.g[2]);
        if (fabsf(span - rec) * 100.0f > g_releaseCm.load()) {
            held = false;
            let_go("the hand slid off the grab point");
        }
    }
    g_gripped.store(held, std::memory_order_relaxed);
    g_eligible.store(inZone, std::memory_order_relaxed);

    // The buzz means "you could grab here", so it stops once you have.
    if (g_buzz.load() && inZone && !held && now - g_lastBuzzMs >= kBuzzEveryMs) {
        g_lastBuzzMs = now;
        bvr::vr::haptic_pulse(0, g_buzzAmp.load(), kBuzzMs);
    }

    // The off-hand bumper stays reserved from the zone through the whole hold,
    // and until that squeeze is let go even if the hand leaves the zone.
    const bool reserve = inZone || held || (g_squeezeLatched && g_reservedLast);
    bvr::vr::reserve_grip_bumper(0, reserve);
    g_reservedLast = reserve;

    bvr::vr::set_two_hand_grip(held, p.g, p.rel);
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Off hand + two-handed grip")) return;

    bool b = g_offHand.load();
    if (ImGui::Checkbox("Show the off hand (it follows your other controller)", &b)) {
        g_offHand.store(b);
        g_saveRequest.store(true);
    }
    b = g_twoHand.load();
    if (ImGui::Checkbox("Two-handed grip", &b)) {
        g_twoHand.store(b);
        g_saveRequest.store(true);
    }
    b = g_buzz.load();
    if (ImGui::Checkbox("Buzz the off hand in the grab zone", &b)) {
        g_buzz.store(b);
        g_saveRequest.store(true);
    }
    float f = g_grabCm.load();
    if (ImGui::SliderFloat("grab radius (cm)", &f, 4.0f, 30.0f)) {
        g_grabCm.store(f);
        g_saveRequest.store(true);
    }
    f = g_releaseCm.load();
    if (ImGui::SliderFloat("slide-off distance (cm)", &f, 8.0f, 50.0f)) {
        g_releaseCm.store(f);
        g_saveRequest.store(true);
    }

    ImGui::Separator();
    char key[96];
    strncpy_s(key, g_keyUi, _TRUNCATE);
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        have = g_points.count(key) != 0;
    }
    ImGui::Text("Weapon: %s  |  grab point: %s", key[0] ? key : "(none raised)",
                have ? "set" : "NOT SET");
    if (g_captureArmed.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                           "Recording: hold the gun, put your off hand on the grip, squeeze "
                           "the off-hand grip.");
        if (ImGui::Button("Cancel")) g_captureArmed.store(false);
    } else {
        if (ImGui::Button("Set grab point")) {
            g_captureStartMs = GetTickCount64();
            g_captureArmed.store(true);
        }
        if (have) {
            ImGui::SameLine();
            if (ImGui::Button("Clear grab point")) g_clearRequest.store(1);
        }
    }
    ImGui::TextDisabled("One press per weapon: the next off-hand squeeze records the spot.");
    const float d = g_distCm.load();
    if (d >= 0.0f)
        ImGui::Text("off hand %.1f cm from the grab point  %s%s", d,
                    g_eligible.load() ? "IN ZONE" : "", g_gripped.load() ? "  - HOLDING" : "");
}

} // namespace bvr::b1r::twohand
