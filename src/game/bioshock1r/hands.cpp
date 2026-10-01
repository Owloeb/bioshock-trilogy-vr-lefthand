// M7 visible hands + weapons. See hands.h for the design; ENGINE_NOTES
// "Viewmodel / AHands" for the derivations.
//
// Two drivable targets, learned from the first in-headset test (2026-07-25):
//
//   HANDS mode (default): drive the AHands actor - the one whose transform the
//   renderer actually honors. Its origin is the EYE anchor (Hands.UpdateLocation
//   places it at PawnOwner.Location + EyeHeight + PlayerViewOffset, rotated by
//   the view rotation - decompile, summarized in ENGINE_NOTES), so the mesh's
//   gun hangs ~50 UU in front of the pivot and rotations swing it on that
//   lever. The full pivot correction (pull the origin ~-100 cm so the gun sits
//   at the controller) is CULLED - the engine drops the rig once the origin
//   goes behind the camera - so how much correction is affordable is a
//   headset-side tuning question, bounded by how far out the hand is held.
//
//   GUN mode (experimental, inert): drive the WEAPON actor, whose origin sits
//   AT the visible gun. Would be the ideal pivot - but live-proven ineffective:
//   the renderer draws an ATTACHED weapon from its attachment matrix and
//   ignores the actor fields (full-rate writes to the live pistol moved
//   nothing). Kept only as the anchor for a future detach experiment - the
//   weapon's Base pointer (its attach parent, = the AHands actor) sits at
//   +0x450, adjacent to Owner at +0x454, the classic UE2 pair.

#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/twohand.h"

#include "core/gfx/hud_capture.h"
#include "core/gfx/vm_mirror.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/bonewatch.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>

namespace bvr::b1r::hands {
namespace {

const uint8_t* g_imageBase = nullptr;

std::atomic<bool> g_enabled{false};
std::atomic<int> g_pendingEnable{-1}; // overlay -> game thread (see aim.cpp)
std::atomic<int> g_mode{2};           // 0 = gun (inert), 1 = hands (actor pin,
                                      // retired), 2 = bones (M7-v2, default)
std::atomic<bool> g_useAimPose{true}; // aim pose = the ray the laser/bullet use
// Grip-pose placement (arms branch): the hands sit where YOUR hands are - the
// OpenXR grip pose matched to Jack's measured palm frame (bones::grip_to_anchor)
// - instead of pinning the gun at the controller's pointing ray. With it, the
// bullets follow the rendered barrel so wherever the gun you see points is where
// it shoots, independent of hand placement.
std::atomic<bool> g_gripPlace{true};
std::atomic<float> g_palmDepthCm{2.0f};
std::atomic<bool> g_barrelAim{true};
std::atomic<bool> g_palmCast{true};  // plasmid casts leave Jack's palm (else the controller)
std::atomic<bool> g_palmPlane{true}; // plasmid mirror plane through the palm (else the wrist)
std::atomic<int> g_handMode{2};       // 0 left, 1 right, 2 auto
std::atomic<int> g_autoHand{1};       // the latched auto choice
// Model offsets, PER HAND (0 left / 1 right, same convention as aim.cpp): the
// pistol and the plasmid hand sit differently in the mesh, so one shared set
// meant tuning the weapon also moved the plasmid hand. Position is in
// CENTIMETRES in the model's final (trimmed) frame; the rotation trim is
// degrees, applied in the CONTROLLER'S LOCAL frame as a quaternion compose -
// euler adds after conversion only behave at one controller orientation (the
// first headset test's "pivot" bug).
// Defaults are ZERO on purpose. The ideal pivot correction (pull the mesh's gun
// to the controller, ~-100 cm forward) is CULLED: the engine drops the whole
// rig the moment the actor origin goes behind the camera (live-proven - the
// rig vanished with the origin 32 UU back). Forward pull is therefore limited
// to roughly the controller's own distance from the face, and where that line
// sits is the user's in-headset call, not a default.
std::atomic<float> g_posFwdCm[2]{0.0f, 0.0f}, g_posRightCm[2]{0.0f, 0.0f},
    g_posUpCm[2]{0.0f, 0.0f};
std::atomic<float> g_rotPitchDeg[2]{0.0f, 0.0f}, g_rotYawDeg[2]{0.0f, 0.0f},
    g_rotRollDeg[2]{0.0f, 0.0f};

std::atomic<bool> g_writeRot{true}; // rotation write can be disabled on its own
int32_t g_probeLeft = 0;

// Cached actors, revalidated by vtable on every use.
void* g_handsActor = nullptr;
void* g_weaponActor = nullptr;
uint64_t g_lastHandsScanMs = 0;
uint64_t g_lastWeaponScanMs = 0;
// Search state at file scope, not function-local, because the retry gates below
// have to be able to ASK whether a sliced sweep is mid-flight. Getting that
// wrong starves the sweep: the first smoke test of the sliced scanner had the
// rate limit refusing to continue an in-progress sweep for up to 32 s, while
// the caller counted every "still working" return as a miss and latched the
// scanner dormant before it had ever completed a single pass (session 27).
patterns::ObjectScan g_handsScan;
patterns::ObjectScan g_weaponScan;
uint32_t g_handsScanFails = 0; // consecutive empty scans -> backoff
void* g_lastPc = nullptr;
std::atomic<uint32_t> g_writes{0};
std::atomic<int32_t> g_lastMatches{0};

// Self-expiring synthetic lanes (the command file polls at 1 Hz, so holds
// outlive their command inside the DLL):
//   test    - camera-relative placement; proves the WRITE lands, no pose math.
//   simpose - a synthetic XR controller pose fed through the REAL mapping path
//             (trim quat, xr_pose_to_game, offsets), so the transform chain is
//             testable with no headset.
struct TestOffset {
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    float distUu = 60.0f;
    uint64_t deadline = 0;
};
TestOffset g_test;

struct SimPose {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    uint64_t deadline = 0;
};
SimPose g_sim;

// Last values written, for the overlay + the flat-test assertions.
std::atomic<float> g_lastX{0.0f}, g_lastY{0.0f}, g_lastZ{0.0f};
std::atomic<int32_t> g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read12(const void* src, void* out) {
    __try {
        memcpy(out, src, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write12(void* dst, const void* in) {
    __try {
        memcpy(dst, in, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* src, void** out) {
    __try {
        *out = *static_cast<void* const*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool has_vtable(void* obj, uint32_t wantRva) {
    if (!obj) return false;
    void* vtbl = nullptr;
    if (!read_ptr(obj, &vtbl)) return false;
    return to_rva(vtbl) == wantRva;
}

// ---- finding the actors ------------------------------------------------------

struct ScanCtx {
    float camX, camY, camZ;
    bool chooseAny; // probe: choose nothing, so the whole list gets logged
};

// Both accept callbacks run inside the scan's SEH guard (heap_scan.cpp), so
// neither may LOG or ALLOCATE: MSVC does not run C++ destructors during SEH
// unwinding, and a fault taken while the log mutex is held would wedge logging
// for the life of the process (session 27). Diagnostics go out through `probe`
// and are formatted by the caller afterwards.
//
// A UClass default object carries the same vtable as a live actor but sits at
// the origin with zeroed fields; proximity to the camera separates the live
// viewmodel from it (and from `0xCCCCCCCC` stack debris).
//
// Probe slots: [0..2] location truncated to UU, [3] distance to the camera.
bool accept_hands(void* obj, void* user, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    ScanCtx* c = static_cast<ScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);
    float loc[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);

    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    probe[0] = static_cast<int32_t>(loc[0]);
    probe[1] = static_cast<int32_t>(loc[1]);
    probe[2] = static_cast<int32_t>(loc[2]);
    probe[3] = static_cast<int32_t>(dist);
    if (!c->chooseAny) return false;
    return dist < 2000.0f && (loc[0] != 0.0f || loc[1] != 0.0f || loc[2] != 0.0f);
}

// The player's weapon: an APlayerWeapon whose owning pawn ([w+0x454]) is the
// player. Both carried weapons pass that (BioShock keeps the stowed one as a
// live actor too, parked at the pawn - live: nearest-to-CAMERA picked the
// stowed wrench, 28 UU below the eye, over the pistol 60 UU ahead of it). So
// the anchor is the EXPECTED GUN SPOT, ~50 UU along the view, which only the
// equipped weapon hovers near. The aim map's learned object, which is the
// weapon that actually FIRES, takes priority over this scan entirely.
struct WeaponScanCtx {
    float camX, camY, camZ;
    const uint8_t* imageBase;
    void* handsActor; // structural accept: Base(+0x450) == the AHands rig
    void* best;
    float bestDist;
};

// Probe slots: [0..2] location truncated to UU, [3] distance to the expected
// gun spot, or -1 when the structural (attachment) accept fired instead.
bool accept_weapon(void* obj, void* user, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    WeaponScanCtx* c = static_cast<WeaponScanCtx*>(user);
    const uint8_t* p = static_cast<const uint8_t*>(obj);

    void* owner = *reinterpret_cast<void* const*>(p + patterns::kWeaponOwnerOffset);
    if (!owner) return false;
    void* ownerVtbl = *reinterpret_cast<void* const*>(owner);
    uint32_t ownerRva = static_cast<uint32_t>(static_cast<const uint8_t*>(ownerVtbl) -
                                              c->imageBase);
    if (ownerRva != patterns::kShockPlayerVtableRva) return false;

    // Structural accept first (session 21): the EQUIPPED weapon is attached
    // to the AHands rig - its Base (+0x450) is the hands actor. Distance to
    // the expected gun spot depends on pose/state and misses in some boot
    // states (live: 2 owner-matched candidates, both >120 UU); attachment
    // does not.
    void* base = *reinterpret_cast<void* const*>(p + patterns::kActorBaseOffset);

    float loc[3];
    memcpy(loc, p + patterns::kActorLocOffset, sizeof loc);
    probe[0] = static_cast<int32_t>(loc[0]);
    probe[1] = static_cast<int32_t>(loc[1]);
    probe[2] = static_cast<int32_t>(loc[2]);

    if (c->handsActor && base == c->handsActor) {
        c->best = obj;
        c->bestDist = 0.0f;
        probe[3] = -1; // structural accept: attached to the rig
        return false;
    }

    float dx = loc[0] - c->camX, dy = loc[1] - c->camY, dz = loc[2] - c->camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    probe[3] = static_cast<int32_t>(dist);
    if (dist < 120.0f && dist < c->bestDist) {
        c->best = obj;
        c->bestDist = dist;
    }
    return false; // never "accept" - attachment/nearest wins after the walk
}

bool weapon_valid(void* w) {
    if (!has_vtable(w, patterns::kPlayerWeaponVtableRva)) return false;
    void* owner = nullptr;
    if (!read_ptr(static_cast<const uint8_t*>(w) + patterns::kWeaponOwnerOffset, &owner))
        return false;
    return has_vtable(owner, patterns::kShockPlayerVtableRva);
}

void* find_hands_actor(const FrameContext& ctx, bool probeOnly) {
    if (!probeOnly) {
        if (has_vtable(g_handsActor, patterns::kHandsVtableRva)) return g_handsActor;
        g_handsActor = nullptr;
        uint64_t now = GetTickCount64();
        // Exponential backoff on consecutive empty scans (2s -> 32s cap).
        // The rig legitimately does not exist for long stretches (the NG+
        // intro runs minutes with no hands), and the full heap scan can take
        // SECONDS on a grown late-game heap - without backoff the 2 s
        // cooldown dragged the whole game to ~1 fps for the entire intro
        // (session 18 part 3, live). Reset on success, world change, and
        // re-enable, so pickup stays prompt when the rig actually appears.
        uint32_t shift = g_handsScanFails > 4 ? 4 : g_handsScanFails;
        // The backoff must never gate a sweep that has already started, or the
        // slices are spread minutes apart and the pass never finishes.
        if (!g_handsScan.sweeping && now - g_lastHandsScanMs < (2000ull << shift)) return nullptr;
        g_lastHandsScanMs = now;
    }
    ScanCtx sc{ctx.camX, ctx.camY, ctx.camZ, !probeOnly};
    patterns::ScanResult r{};
    if (!patterns::run_object_scan(g_handsScan, patterns::kHandsVtableRva,
                                   patterns::kActorViewDirOffset + 12, &accept_hands, &sc, r))
        return nullptr; // sliced sweep still running - no stall, retry next frame
    patterns::log_scan_result("AHands", g_handsScan, r, probeOnly || g_handsScanFails == 0);
    g_lastMatches.store(r.matches, std::memory_order_relaxed);
    if (probeOnly) return nullptr;
    g_handsActor = r.object;
    if (r.object) {
        g_handsScanFails = 0;
    } else {
        ++g_handsScanFails;
    }
    return g_handsActor;
}

void* find_weapon_actor(const FrameContext& ctx, bool probeOnly) {
    // The object the aim seam learned from the trigger IS the equipped gun.
    void* learned = bvr::b1r::aim::learned_weapon_object();
    if (!probeOnly && weapon_valid(learned)) {
        g_weaponActor = learned;
        return learned;
    }
    if (!probeOnly) {
        if (weapon_valid(g_weaponActor)) return g_weaponActor;
        g_weaponActor = nullptr;
        uint64_t now = GetTickCount64();
        // As above: never gate an in-flight sweep.
        if (!g_weaponScan.sweeping && now - g_lastWeaponScanMs < patterns::kScanRetryMs)
            return nullptr;
        g_lastWeaponScanMs = now;
    }
    // Anchor at the expected gun spot: 50 UU along the current view.
    float dir[3];
    FRotator viewRot{ctx.camPitch, ctx.camYaw, 0};
    ue_rot_to_dir(viewRot, dir);
    WeaponScanCtx wc{ctx.camX + dir[0] * 50.0f,
                     ctx.camY + dir[1] * 50.0f,
                     ctx.camZ + dir[2] * 50.0f,
                     g_imageBase,
                     has_vtable(g_handsActor, patterns::kHandsVtableRva) ? g_handsActor
                                                                         : nullptr,
                     nullptr,
                     1e9f};
    patterns::ScanResult r{};
    if (!patterns::run_object_scan(g_weaponScan, patterns::kPlayerWeaponVtableRva,
                                   patterns::kWeaponOwnerOffset + sizeof(void*), &accept_weapon,
                                   &wc, r))
        return nullptr; // sliced sweep still running - no stall, retry next frame

    // This resolver picks through WeaponScanCtx::best rather than by accepting a
    // candidate, so ScanResult::object is ALWAYS null here. Reporting that as
    // "chosen=00000000" is what made the shipped log unreadable: an external
    // crash log could not tell whether the resolver had succeeded or failed
    // (session 27). Log the real outcome.
    patterns::log_scan_result("APlayerWeapon", g_weaponScan, r, probeOnly);
    g_lastMatches.store(r.matches, std::memory_order_relaxed);
    BVR_LOG("[hands] player weapon resolver: %d match(es) -> %s @ %p%s", r.matches,
            wc.best ? (wc.bestDist == 0.0f ? "ATTACHED to the rig" : "nearest to the gun spot")
                    : "NONE",
            wc.best, wc.best && wc.bestDist > 0.0f ? " (distance pick)" : "");
    if (probeOnly) return nullptr;
    g_weaponActor = wc.best;
    return g_weaponActor;
}

void load_config();
void save_config();

void log_status() {
    int mode = g_mode.load(std::memory_order_relaxed);
    BVR_LOG("[hands] status: %s | mode=%s pose=%s | hand=%s | writes=%u",
            g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
            mode == 0 ? "GUN" : mode == 1 ? "HANDS" : "BONES",
            g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip",
            g_handMode.load(std::memory_order_relaxed) == 0 ? "LEFT"
            : g_handMode.load(std::memory_order_relaxed) == 1
                ? "RIGHT"
                : (g_autoHand.load(std::memory_order_relaxed) == 0 ? "auto(L)" : "auto(R)"),
            g_writes.load(std::memory_order_relaxed));
    BVR_LOG("[hands]   weapon actor=%p (learned %p) | hands actor=%p (matches %d)",
            g_weaponActor, bvr::b1r::aim::learned_weapon_object(), g_handsActor,
            g_lastMatches.load(std::memory_order_relaxed));
    for (int h = 0; h < 2; ++h) {
        BVR_LOG("[hands]   offset %s pos fwd%+.1f right%+.1f up%+.1f cm | trim pitch%+.1f "
                "yaw%+.1f roll%+.1f deg%s",
                h == 0 ? "L" : "R",
                g_posFwdCm[h].load(std::memory_order_relaxed),
                g_posRightCm[h].load(std::memory_order_relaxed),
                g_posUpCm[h].load(std::memory_order_relaxed),
                g_rotPitchDeg[h].load(std::memory_order_relaxed),
                g_rotYawDeg[h].load(std::memory_order_relaxed),
                g_rotRollDeg[h].load(std::memory_order_relaxed),
                h == 1 ? (g_writeRot.load(std::memory_order_relaxed) ? " | writeRot=1"
                                                                     : " | writeRot=0")
                       : "");
    }
    uint64_t now = GetTickCount64();
    BVR_LOG("[hands]   last write loc=(%.1f %.1f %.1f) rot=(%d %d %d) testHold=%dms "
            "simHold=%dms",
            g_lastX.load(std::memory_order_relaxed), g_lastY.load(std::memory_order_relaxed),
            g_lastZ.load(std::memory_order_relaxed),
            g_lastPitch.load(std::memory_order_relaxed),
            g_lastYaw.load(std::memory_order_relaxed),
            g_lastRoll.load(std::memory_order_relaxed),
            g_test.deadline > now ? static_cast<int>(g_test.deadline - now) : 0,
            g_sim.deadline > now ? static_cast<int>(g_sim.deadline - now) : 0);
}

// ---- persistence -----------------------------------------------------------
// Every weapon model sits differently in the hand, so these numbers are found
// by eye in the headset and must survive the session that found them. Plain
// key=value text in the mod's own data dir - no new dependency, and the user
// can read it.

void config_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\hands.ini", bvr::log::data_dir());
}

void save_config() {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[hands] could not write hands.ini");
        return;
    }
    fprintf(f, "# BioShock VR - M7 viewmodel offsets (cm / degrees, model-local frame)\n");
    fprintf(f, "# Per-hand keys: ...L = left (plasmid), ...R = right (weapon). A legacy\n");
    fprintf(f, "# suffix-less key (posFwdCm=...) still loads and applies to BOTH hands.\n");
    fprintf(f, "mode=%d\n", g_mode.load(std::memory_order_relaxed));
    fprintf(f, "aimPose=%d\n", g_useAimPose.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "gripPlacement=%d\npalmDepthCm=%.2f\nbarrelAim=%d\n", g_gripPlace.load() ? 1 : 0,
            g_palmDepthCm.load(), g_barrelAim.load() ? 1 : 0);
    fprintf(f, "palmCast=%d\npalmPlane=%d\n", g_palmCast.load() ? 1 : 0, g_palmPlane.load() ? 1 : 0);
    for (int h = 0; h < 2; ++h) {
        const char* s = h == 0 ? "L" : "R";
        fprintf(f, "posFwdCm%s=%.2f\n", s, g_posFwdCm[h].load(std::memory_order_relaxed));
        fprintf(f, "posRightCm%s=%.2f\n", s, g_posRightCm[h].load(std::memory_order_relaxed));
        fprintf(f, "posUpCm%s=%.2f\n", s, g_posUpCm[h].load(std::memory_order_relaxed));
        fprintf(f, "rotPitchDeg%s=%.2f\n", s, g_rotPitchDeg[h].load(std::memory_order_relaxed));
        fprintf(f, "rotYawDeg%s=%.2f\n", s, g_rotYawDeg[h].load(std::memory_order_relaxed));
        fprintf(f, "rotRollDeg%s=%.2f\n", s, g_rotRollDeg[h].load(std::memory_order_relaxed));
    }
    fclose(f);
    BVR_LOG("[hands] offsets saved to hands.ini");
}

// "posFwdCmL"/"posFwdCmR" store one hand; the legacy suffix-less "posFwdCm"
// (pre-per-hand ini files) stores BOTH, so an old hands.ini keeps working.
bool store_hand_key(const char* key, const char* base, std::atomic<float> (&dst)[2], float v) {
    size_t n = strlen(base);
    if (strncmp(key, base, n) != 0) return false;
    if (key[n] == '\0') {
        dst[0].store(v, std::memory_order_relaxed);
        dst[1].store(v, std::memory_order_relaxed);
        return true;
    }
    if ((key[n] == 'L' || key[n] == 'R') && key[n + 1] == '\0') {
        dst[key[n] == 'R' ? 1 : 0].store(v, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void load_config() {
    wchar_t path[MAX_PATH];
    config_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file yet is normal
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char key[64] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2)
            continue;
        ++n;
        if (strcmp(key, "mode") == 0) {
            int m = static_cast<int>(v);
            g_mode.store(m < 0 ? 0 : m > 2 ? 2 : m, std::memory_order_relaxed);
        }
        else if (strcmp(key, "aimPose") == 0) g_useAimPose.store(v != 0.0f, std::memory_order_relaxed);
        else if (strcmp(key, "gripPlacement") == 0) g_gripPlace.store(v != 0.0f);
        else if (strcmp(key, "palmDepthCm") == 0) g_palmDepthCm.store(v);
        else if (strcmp(key, "barrelAim") == 0) g_barrelAim.store(v != 0.0f);
        else if (strcmp(key, "palmCast") == 0) g_palmCast.store(v != 0.0f);
        else if (strcmp(key, "palmPlane") == 0) g_palmPlane.store(v != 0.0f);
        else if (store_hand_key(key, "posFwdCm", g_posFwdCm, v)) {}
        else if (store_hand_key(key, "posRightCm", g_posRightCm, v)) {}
        else if (store_hand_key(key, "posUpCm", g_posUpCm, v)) {}
        else if (store_hand_key(key, "rotPitchDeg", g_rotPitchDeg, v)) {}
        else if (store_hand_key(key, "rotYawDeg", g_rotYawDeg, v)) {}
        else if (store_hand_key(key, "rotRollDeg", g_rotRollDeg, v)) {}
        else --n;
    }
    fclose(f);
    if (n) BVR_LOG("[hands] loaded %d value(s) from hands.ini", n);
}

} // namespace

// BioShock holds ONE thing at a time, so the viewmodel belongs to whichever
// hand the player last engaged. Two ways to engage, both latched here from
// the state the bridge itself composes (same source as aim.cpp's object map):
//   - the TRIGGERS (fire = switch-and-fire, XENON_RT/LT), and
//   - the BUMPERS (the grips compose to LB/RB, and a bumper press switches
//     the raised hand with NO trigger event - the M8 grip-switch bug was the
//     latch only learning from triggers, so a grip switch left the model on
//     the stale controller until the next trigger pull).
// Bumpers are checked first so a same-frame trigger wins (firing is the
// stronger evidence of which hand the player means). Shared with the aim
// laser so the beam leaves the hand that is actually holding the weapon.
int active_hand() {
    int mode = g_handMode.load(std::memory_order_relaxed);
    if (mode == 0 || mode == 1) return mode;

    bool lb = false, rb = false;
    bvr::input::last_composed_bumpers(&lb, &rb);
    if (rb && !lb) g_autoHand.store(1, std::memory_order_relaxed);
    else if (lb && !rb) g_autoHand.store(0, std::memory_order_relaxed);

    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    if (rt >= 64 && lt < 64) g_autoHand.store(1, std::memory_order_relaxed);
    else if (lt >= 64 && rt < 64) g_autoHand.store(0, std::memory_order_relaxed);
    return g_autoHand.load(std::memory_order_relaxed);
}

void* hands_actor() {
    return g_handsActor;
}

void* weapon_actor() {
    // Primary (session 21 part 2): read Hands.CurrentHoldable straight off
    // the rig - THE equipped weapon by definition, updated by the engine at
    // equip time. The old learned/cache preference pinned the resolver to
    // the previously FIRED weapon across wheel switches (an unequipped
    // weapon keeps its vtable and owner), which broke per-weapon profile
    // swapping in the first headset run.
    if (has_vtable(g_handsActor, patterns::kHandsVtableRva)) {
        void* hold = nullptr;
        if (read_ptr(static_cast<const uint8_t*>(g_handsActor) +
                         patterns::kHandsCurrentHoldableOffset,
                     &hold) &&
            weapon_valid(hold)) {
            g_weaponActor = hold;
            return hold;
        }
    }
    void* w = weapon_valid(g_weaponActor) ? g_weaponActor
                                          : bvr::b1r::aim::learned_weapon_object();
    return weapon_valid(w) ? w : nullptr;
}

void* resolve_weapon_actor(const FrameContext& ctx) {
    return find_weapon_actor(ctx, false);
}

bool weapon_scan_in_progress() { return g_weaponScan.sweeping; }

bool current_holdable(void** out) {
    // Raw rig read, CLASS-AGNOSTIC: the MachineGun and GrenadeLauncher carry
    // a different native vtable than kPlayerWeaponVtableRva, so the
    // vtable-gated weapon_actor() path rejected them and fell back to the
    // stale cached weapon - the session-21 part-3 defect (their profile key
    // never changed and edits landed in the previous weapon's profile). The
    // profile layer keys on the CLASS NAME, which validates through the
    // UClass vtable instead - any holdable class resolves. Returns false
    // when the rig itself is unknown/unreadable (callers then fall back to
    // the legacy paths); *out may be null (nothing equipped).
    if (!has_vtable(g_handsActor, patterns::kHandsVtableRva)) return false;
    void* hold = nullptr;
    if (!read_ptr(static_cast<const uint8_t*>(g_handsActor) +
                      patterns::kHandsCurrentHoldableOffset,
                  &hold))
        return false;
    *out = hold;
    return true;
}

// Live mesh-alignment trim, read by `vraim synccheck` so its model chain runs
// on the REAL tuned values (session 20).
float model_trim_pitch_deg(int hand) {
    return g_rotPitchDeg[hand & 1].load(std::memory_order_relaxed);
}
float model_trim_yaw_deg(int hand) {
    return g_rotYawDeg[hand & 1].load(std::memory_order_relaxed);
}
float model_trim_roll_deg(int hand) {
    return g_rotRollDeg[hand & 1].load(std::memory_order_relaxed);
}

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    load_config();
    int mode = g_mode.load(std::memory_order_relaxed);
    BVR_LOG("[hands] init: mode=%s (AHands vtable 0x%X, APlayerWeapon vtable 0x%X)",
            mode == 0 ? "GUN" : mode == 1 ? "HANDS" : "BONES", patterns::kHandsVtableRva,
            patterns::kPlayerWeaponVtableRva);
}

// ---- Viewmodel mirror probe (see core/gfx/vm_mirror.h) ----------------------
// The render side reflects every foreground draw about the head's mid-plane;
// this side hands the rig the controller poses reflected about the SAME plane,
// so the reflected render lands on the real hands. Both reflections are the
// same plane: through the HMD, normal = the HMD's right axis (the axis the
// stereo eye offsets are applied along).
//
// Reflecting a controller is P' = M(P - H) + H with M = I - 2nn^T, and for the
// orientation R' = M R D, where D flips the controller's LOCAL x. D is what
// turns a left controller into a right one: both hands' grip and aim frames
// have +x pointing to the user's right, so a reflected left frame has +x
// pointing left until D restores the convention. det(M R D) = +1, a proper
// rotation. The result is a virtual RIGHT controller for the gun hand (so the
// right-hand calibration applies unchanged) and a virtual LEFT one for the
// plasmid hand.
namespace {

// Effects-flip state (used by apply_gun_plane and the attach hook below).
bool g_attachHookTried = false, g_attachHookLive = false;
std::atomic<bool> g_effectsFlip{true};
std::atomic<uint32_t> g_flips{0};
void* g_flipWeapon = nullptr;
float g_pgQ[3] = {}, g_pgN[3] = {};
uint64_t g_pgStampMs = 0;

void quat_to_mat(const float q[4], float m[9]) { // columns = local axes
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[3] = 2 * (x * y - z * w);     m[6] = 2 * (x * z + y * w);
    m[1] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[7] = 2 * (y * z - x * w);
    m[2] = 2 * (x * z - y * w);     m[5] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

void mat_to_quat(const float m[9], float q[4]) {
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[5] - m[7]) / s;
        q[1] = (m[6] - m[2]) / s;
        q[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[5] - m[7]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[3] + m[1]) / s;
        q[2] = (m[6] + m[2]) / s;
    } else if (m[4] > m[8]) {
        const float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[6] - m[2]) / s;
        q[0] = (m[3] + m[1]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[7] + m[5]) / s;
    } else {
        const float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[1] - m[3]) / s;
        q[0] = (m[6] + m[2]) / s;
        q[1] = (m[7] + m[5]) / s;
        q[2] = 0.25f * s;
    }
}

// Reflect an XR-space controller pose about the head mid-plane (in place).
void mirror_pose_about_head(const bvr::vr::HeadPose& head, float pos[3], float quat[4]) {
    static const float kRight[3] = {1.0f, 0.0f, 0.0f};
    float n[3];
    quat_rotate(head.qx, head.qy, head.qz, head.qw, kRight, n);
    const float d = (pos[0] - head.px) * n[0] + (pos[1] - head.py) * n[1] +
                    (pos[2] - head.pz) * n[2];
    for (int i = 0; i < 3; ++i) pos[i] -= 2.0f * d * n[i];

    float r[9];
    quat_to_mat(quat, r);
    for (int c = 0; c < 3; ++c) {
        float* col = &r[c * 3];
        if (c == 0) { col[0] = -col[0]; col[1] = -col[1]; col[2] = -col[2]; } // D
        const float k = col[0] * n[0] + col[1] * n[1] + col[2] * n[2];       // M
        for (int i = 0; i < 3; ++i) col[i] -= 2.0f * k * n[i];
    }
    mat_to_quat(r, quat);
}

void configure_mirror_once() {
    static bool done = false;
    if (done) return;
    done = true;
    // THE fg marker, measured by the probe's own labelled stack samples (run
    // v3, 2026-09-29): fg and world skinned draws share every frame from the
    // D3D call up to the scene iterator 0x5CF9xx, and differ at the call site
    // inside it - fg draws return to 0x5CFECF, world draws to 0x5CFF36. That
    // is the "recorded scene command that iterates the mesh" ENGINE_NOTES
    // session 21 names. The session-21 bake RVAs (0x3DBF7C / 0x3EDCBF) are
    // skinning strategies shared with the world - measured ~5,000 world
    // draws/s carrying them - so they are no longer used.
    // cb tiers: 576 carries the clip rows at float 40 (after the 36..39
    // viewport block, patterns.h kFgCbTransformFirst); 832 and 1088 measured
    // at the same offset live.
    static const uint32_t kFgRet[] = {0x5CFECF};
    static const bvr::vm_mirror::TierSpec kTiers[] = {
        {576, static_cast<int>(patterns::kFgCbTransformFirst) + 4},
        {832, static_cast<int>(patterns::kFgCbTransformFirst) + 4}, // discovered live, run 1
        {1088, static_cast<int>(patterns::kFgCbTransformFirst) + 4}}; // measured, run 2
    bvr::vm_mirror::configure(kFgRet, 1, kTiers, 3);
}

// Arms the render-side reflection exactly once per call, on every exit path,
// so a gated frame (cinematic, cutscene view, no rig) is never reflected.
struct MirrorArm {
    bool arm = false;
    ~MirrorArm() { bvr::vm_mirror::set_armed(arm); }
};

// FRotator from a UE basis (forward + up), the exact inverse of ue_rot_basis
// (same zero-roll reference frame ue_angles_from_xr_quat measures roll in).
FRotator basis_to_rot(const float f[3], const float u[3]) {
    FRotator r{};
    const float len2d = sqrtf(f[0] * f[0] + f[1] * f[1]);
    r.yaw = static_cast<int32_t>(atan2f(f[1], f[0]) * kRotUnitsPerRadian);
    r.pitch = static_cast<int32_t>(atan2f(f[2], len2d) * kRotUnitsPerRadian);
    if (len2d > 0.001f) {
        const float rn[3] = {-f[1] / len2d, f[0] / len2d, 0.0f};
        const float un[3] = {-f[2] * rn[1], f[2] * rn[0], f[0] * rn[1] - f[1] * rn[0]};
        r.roll = static_cast<int32_t>(
            atan2f(u[0] * rn[0] + u[1] * rn[1],
                   u[0] * un[0] + u[1] * un[1] + u[2] * un[2]) *
            kRotUnitsPerRadian);
    }
    return r;
}

void reflect_vec(const float v[3], const float n[3], float out[3]) {
    const float k = 2.0f * (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]);
    for (int i = 0; i < 3; ++i) out[i] = v[i] - k * n[i];
}

// Head-plane mode (v4): per eye, the head centre sits at x = -eyeSign*IPD/2 in
// that eye's view, so the plane is x = d with d = +IPD/2 (left), -IPD/2 (right).
void publish_head_planes(const FrameContext& ctx) {
    const float half = camera::ipd_mm() / 2000.0f * ctx.worldScale;
    const float shift = bvr::vm_mirror::plane_shift_uu();
    const float n[3] = {1.0f, 0.0f, 0.0f};
    bvr::vm_mirror::set_plane(0, n, +half + shift, true);
    bvr::vm_mirror::set_plane(1, n, -half + shift, true);
}

// GUN-PLANE mode (v5) - the effects fix. `gp` arrives as the rig pose for the
// head-reflected (virtual) controller, which is what the render must SHOW
// after one reflection. Showing it via the head plane leaves the ENGINE's rig -
// and so its muzzle, and every flash, tracer and smoke puff the engine spawns
// from it - on the wrong side of the head. Instead:
//   F  = head-reflection of the virtual rig   (the gun as it must appear: at
//        the real left hand, an improper frame - a left-handed gun)
//   Pg = F's own vertical mid-plane (through F, normal = F's right axis)
//   engine rig R' = Pg-reflection of F        (a proper right-handed rig with
//        F's forward and up, at F's place)
// and the render reflects about Pg, which turns R' back into F exactly. The
// barrel lies in Pg, so the ENGINE muzzle is the VISIBLE muzzle: effects spawn
// where the gun is seen. The overlay shift slider moves Pg sideways for a
// weapon whose barrel sits off the attach bone's plane (R' then moves by 2x the
// shift, keeping F where it was).
// Returns false when the per-eye cameras are unavailable (caller falls back).
// Head plane of the last apply_gun_plane (point, normal), for the off hand.
float g_hdH[3] = {};
float g_hdN[3] = {};

// World-space render plane (gun-plane mode), re-expressed per eye.
float g_eyePlaneQ[3] = {};
float g_eyePlaneN[3] = {};
bool g_eyePlaneLive = false;
uint64_t g_eyePlaneStampMs = 0;

// Pg in one eye's view space (x right, y up, z forward): n.X = d.
void publish_eye_plane(int e, const float eyeLoc[3], const int32_t eyeRot[3]) {
    FRotator er{eyeRot[0], eyeRot[1], eyeRot[2]};
    float ef[3], erg[3], eu[3];
    ue_rot_basis(er, ef, erg, eu);
    const float* fr = g_eyePlaneN;
    const float* Q = g_eyePlaneQ;
    const float n[3] = {fr[0] * erg[0] + fr[1] * erg[1] + fr[2] * erg[2],
                        fr[0] * eu[0] + fr[1] * eu[1] + fr[2] * eu[2],
                        fr[0] * ef[0] + fr[1] * ef[1] + fr[2] * ef[2]};
    const float d = fr[0] * (Q[0] - eyeLoc[0]) + fr[1] * (Q[1] - eyeLoc[1]) +
                    fr[2] * (Q[2] - eyeLoc[2]);
    bvr::vm_mirror::set_plane(e, n, d, true);
}

bool apply_gun_plane(const GamePose& headW, GamePose& gp, float shift) {
    float eyeLoc[2][3];
    int32_t eyeRot[2][3];
    for (int e = 0; e < 2; ++e)
        if (!camera::driven_eye_cam(e, eyeLoc[e], eyeRot[e])) return false;

    float hf[3], hr[3], hu[3];
    ue_rot_basis(headW.rot, hf, hr, hu); // head right = the head plane normal
    const float H[3] = {headW.loc.x, headW.loc.y, headW.loc.z};

    float vf[3], vr[3], vu[3];
    ue_rot_basis(gp.rot, vf, vr, vu);
    const float Tv[3] = {gp.loc.x, gp.loc.y, gp.loc.z};

    // F: head-reflect the virtual rig.
    const float rel[3] = {Tv[0] - H[0], Tv[1] - H[1], Tv[2] - H[2]};
    float relF[3], ff[3], fr[3], fu[3];
    reflect_vec(rel, hr, relF);
    reflect_vec(vf, hr, ff);
    reflect_vec(vr, hr, fr);
    reflect_vec(vu, hr, fu);
    const float Tf[3] = {H[0] + relF[0], H[1] + relF[1], H[2] + relF[2]};

    // Pg through F (+ shift along F's right), normal = F's right. v6: the
    // caller's shift puts Pg through the weapon's MUZZLE (auto, from bone 44)
    // plus the per-weapon trim, so engine effects leave the visible barrel.
    const float Q[3] = {Tf[0] + shift * fr[0], Tf[1] + shift * fr[1], Tf[2] + shift * fr[2]};

    // Engine rig: Pg-reflection of F. Position Tf + 2*shift*fr; forward/up
    // unchanged by Pg (they lie in it); right becomes -fr (proper frame).
    gp.loc.x = Tf[0] + 2.0f * shift * fr[0];
    gp.loc.y = Tf[1] + 2.0f * shift * fr[1];
    gp.loc.z = Tf[2] + 2.0f * shift * fr[2];
    gp.rot = basis_to_rot(ff, fu);

    // The head plane too: the off hand goes through the same two reflections.
    for (int i = 0; i < 3; ++i) {
        g_hdH[i] = H[i];
        g_hdN[i] = hr[i];
    }
    // Effects flip: the same plane in WORLD space.
    for (int i = 0; i < 3; ++i) {
        g_pgQ[i] = Q[i];
        g_pgN[i] = fr[i];
    }
    g_pgStampMs = GetTickCount64();
    // The render plane, in WORLD space, for on_eye_camera.
    for (int i = 0; i < 3; ++i) {
        g_eyePlaneQ[i] = Q[i];
        g_eyePlaneN[i] = fr[i];
    }
    g_eyePlaneLive = true;
    g_eyePlaneStampMs = GetTickCount64();
    // Provisional per-eye publish from the stashed cameras. Those are the
    // PREVIOUS frame's (the left eye is stashed after this runs), so while
    // moving they lag by a frame and the two eyes disagree - the doubling.
    // on_eye_camera overwrites each eye with its exact current camera right
    // before that eye renders; this only covers a frame where it does not run.
    for (int e = 0; e < 2; ++e) publish_eye_plane(e, eyeLoc[e], eyeRot[e]);
    return true;
}

// Reflect a rig pose about the plane through P with unit normal n. The result
// is rebuilt as a PROPER frame from the reflected forward and up (right flips),
// so two of these compose to the rigid motion both planes describe.
void reflect_pose(GamePose& gp, const float P[3], const float n[3]) {
    float f[3], r[3], u[3];
    ue_rot_basis(gp.rot, f, r, u);
    const float rel[3] = {gp.loc.x - P[0], gp.loc.y - P[1], gp.loc.z - P[2]};
    float relR[3], fR[3], uR[3];
    reflect_vec(rel, n, relR);
    reflect_vec(f, n, fR);
    reflect_vec(u, n, uR);
    gp.loc = {P[0] + relR[0], P[1] + relR[1], P[2] + relR[2]};
    gp.rot = basis_to_rot(fR, uR);
}

// ---- Viewmodel recoil -----------------------------------------------------
// The shotgun and the Tommy gun fire dead flat in VR. Each player shot (the fire
// seam's counter, aim.cpp) kicks the RENDERED gun: muzzle up, a little sideways,
// back into the hand, then a spring back. Visual only - the fire ray is built
// from the controller, so accuracy is untouched - plus a haptic thump. Held
// two-handed, the kick is smaller and the off hand rides it.
struct RecoilSpec {
    const char* weapon;
    float pitchDeg, yawDeg, backCm; // per shot
    float returnMs;                 // spring-back time constant
    float hapticAmp;
    int hapticMs;
};
// Per shot. The chemical thrower fires a stream of small "shots" (one per
// projectile tick), so its numbers are a shimmer that settles, not a kick; the
// research camera only clicks.
const RecoilSpec kRecoil[] = {
    {"Shotgun", 11.0f, 1.5f, 5.0f, 170.0f, 1.0f, 90},
    {"MachineGun", 2.4f, 1.0f, 1.4f, 90.0f, 0.55f, 35},
    {"Pistol", 5.5f, 1.0f, 2.2f, 110.0f, 0.75f, 45},
    {"GrenadeLauncher", 9.0f, 0.8f, 4.5f, 230.0f, 1.0f, 110},
    {"Crossbow", 3.5f, 0.4f, 1.6f, 160.0f, 0.6f, 60},
    {"ChemicalThrower", 0.35f, 0.4f, 0.25f, 120.0f, 0.3f, 50},
    {"ResearchCamera", 0.0f, 0.0f, 0.0f, 100.0f, 0.4f, 25},
};

std::atomic<bool> g_recoilOn{true};

// Plasmid casts and wrench swings share the ability fire seam; which one it
// was is which hand owns the viewmodel. Haptics only.
uint32_t g_abShots = 0;
bool g_abSeeded = false;
void ability_haptics(int hand) {
    const uint32_t n = aim::player_ability_count();
    if (!g_abSeeded) {
        g_abShots = n;
        g_abSeeded = true;
    }
    if (n == g_abShots) return;
    g_abShots = n;
    if (!g_recoilOn.load(std::memory_order_relaxed)) return;
    if (hand == 1 && strcmp(aim::active_weapon_key(), "Wrench") == 0)
        bvr::vr::haptic_pulse(1, 0.85f, 70); // the wrench connects
    else if (hand == 0)
        bvr::vr::haptic_pulse(0, 0.6f, 90); // a plasmid leaves the palm
}
std::atomic<float> g_recoilScale{1.0f};
float g_rcTarget[3] = {}; // pitch deg, yaw deg, back cm
float g_rcCur[3] = {};
uint32_t g_rcShots = 0;
bool g_rcShotsSeeded = false;
uint64_t g_rcLastMs = 0;

// A rigid kick about the weapon hand: old and new bases, pivot, push-back.
struct Kick {
    float P[3];
    float f[3], r[3], u[3];    // before
    float f2[3], r2[3], u2[3]; // after
    float back[3];             // translation, UU
};

void kick_vec(const Kick& k, const float v[3], float out[3]) {
    const float cf = v[0] * k.f[0] + v[1] * k.f[1] + v[2] * k.f[2];
    const float cr = v[0] * k.r[0] + v[1] * k.r[1] + v[2] * k.r[2];
    const float cu = v[0] * k.u[0] + v[1] * k.u[1] + v[2] * k.u[2];
    for (int i = 0; i < 3; ++i) out[i] = cf * k.f2[i] + cr * k.r2[i] + cu * k.u2[i];
}

void apply_kick(const Kick& k, GamePose& gp) {
    const float rel[3] = {gp.loc.x - k.P[0], gp.loc.y - k.P[1], gp.loc.z - k.P[2]};
    float relK[3];
    kick_vec(k, rel, relK);
    gp.loc = {k.P[0] + relK[0] + k.back[0], k.P[1] + relK[1] + k.back[1],
              k.P[2] + relK[2] + k.back[2]};
    float f[3], r[3], u[3], fK[3], uK[3];
    ue_rot_basis(gp.rot, f, r, u);
    kick_vec(k, f, fK);
    kick_vec(k, u, uK);
    gp.rot = basis_to_rot(fK, uK);
}

// Advance the recoil spring and build this frame's kick for the weapon pose.
// False when there is nothing to apply.
bool recoil_kick(const FrameContext& ctx, const GamePose& gp, bool held, Kick& k) {
    const uint64_t now = GetTickCount64();
    float dt = g_rcLastMs ? (now - g_rcLastMs) / 1000.0f : 0.0f;
    g_rcLastMs = now;
    if (dt > 0.1f) dt = 0.1f;
    const uint32_t shots = aim::player_shot_count();
    if (!g_rcShotsSeeded) {
        g_rcShots = shots;
        g_rcShotsSeeded = true;
    }
    const RecoilSpec* spec = nullptr;
    const char* key = aim::active_weapon_key();
    for (const RecoilSpec& r : kRecoil)
        if (strcmp(key, r.weapon) == 0) spec = &r;
    const uint32_t newShots = shots - g_rcShots;
    g_rcShots = shots;
    if (spec && newShots && newShots < 8 && g_recoilOn.load(std::memory_order_relaxed)) {
        const float sc = g_recoilScale.load(std::memory_order_relaxed) * (held ? 0.55f : 1.0f);
        for (uint32_t n = 0; n < newShots; ++n) {
            const float side = (GetTickCount() ^ (n * 2654435761u)) & 1 ? 1.0f : -1.0f;
            g_rcTarget[0] += spec->pitchDeg * sc;
            g_rcTarget[1] += spec->yawDeg * sc * side;
            g_rcTarget[2] += spec->backCm * sc;
        }
        if (g_rcTarget[0] > 28.0f) g_rcTarget[0] = 28.0f; // a sustained burst climbs, then holds
        if (g_rcTarget[2] > 10.0f) g_rcTarget[2] = 10.0f;
        bvr::vr::haptic_pulse(1, spec->hapticAmp, spec->hapticMs);
        if (held) bvr::vr::haptic_pulse(0, spec->hapticAmp * 0.6f, spec->hapticMs);
    }
    // Spring: the target decays home, the pose chases it fast (a crisp rise).
    const float ret = spec ? spec->returnMs / 1000.0f : 0.12f;
    const float decay = expf(-dt / ret);
    const float chase = 1.0f - expf(-dt / 0.012f);
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        g_rcTarget[i] *= decay;
        g_rcCur[i] += (g_rcTarget[i] - g_rcCur[i]) * chase;
        if (fabsf(g_rcCur[i]) > 0.01f) any = true;
    }
    if (!any) return false;

    ue_rot_basis(gp.rot, k.f, k.r, k.u);
    k.P[0] = gp.loc.x;
    k.P[1] = gp.loc.y;
    k.P[2] = gp.loc.z;
    const float a = g_rcCur[0] / kRadToDeg, b = g_rcCur[1] / kRadToDeg;
    float f1[3], u1[3];
    for (int i = 0; i < 3; ++i) {
        f1[i] = k.f[i] * cosf(a) + k.u[i] * sinf(a); // muzzle up
        u1[i] = k.u[i] * cosf(a) - k.f[i] * sinf(a);
    }
    for (int i = 0; i < 3; ++i) {
        k.f2[i] = f1[i] * cosf(b) + k.r[i] * sinf(b); // and a little sideways
        k.r2[i] = k.r[i] * cosf(b) - f1[i] * sinf(b);
        k.u2[i] = u1[i];
    }
    const float backUu = g_rcCur[2] * ctx.worldScale / 100.0f;
    for (int i = 0; i < 3; ++i) k.back[i] = -k.f[i] * backUu;
    return true;
}

// ---- Arms: shoulder anchors (experimental, arms.ini) -------------------------
// Each visible hand's arm hangs from a shoulder below and beside the head. The
// shoulders follow a TORSO yaw that lags the head (a 25 deg deadzone plus a
// slow drift), so looking around does not swing both arms. Computed in the
// rig's own space - the virtual, head-mirrored one when mirroring - then put
// through the same reflections as the hands, so the render mirror shows each
// arm on the side of the hand it holds.
std::atomic<bool> g_armsOn{true};
std::atomic<float> g_shDownCm{20.0f}, g_shSideCm{18.0f}, g_shBackCm{6.0f};
std::atomic<float> g_elbowOut{0.6f};
std::atomic<bool> g_armsSave{false};
bool g_armsLoaded = false;
float g_torsoYaw = 0.0f;
bool g_torsoValid = false;
uint64_t g_torsoMs = 0;

void arms_ini_path(wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%s\\arms.ini", bvr::log::data_dir());
}
void arms_save() {
    wchar_t path[MAX_PATH];
    arms_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# BioShock VR - arms (experimental)\n");
    fprintf(f, "armsOn=%d\nshoulderDownCm=%.1f\nshoulderSideCm=%.1f\nshoulderBackCm=%.1f\n"
               "elbowOut=%.2f\narmLength=%.2f\nscaleSkin=%d\n",
            g_armsOn.load() ? 1 : 0, g_shDownCm.load(), g_shSideCm.load(), g_shBackCm.load(),
            g_elbowOut.load(), bones::arm_length(), bones::arm_scale_skin() ? 1 : 0);
    fclose(f);
}
void arms_load() {
    g_armsLoaded = true;
    wchar_t path[MAX_PATH];
    arms_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") == 0 && f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char key[48] = {};
            float v = 0.0f;
            if (sscanf_s(line, "%47[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2)
                continue;
            if (strcmp(key, "armsOn") == 0) g_armsOn.store(v != 0.0f);
            else if (strcmp(key, "shoulderDownCm") == 0) g_shDownCm.store(v);
            else if (strcmp(key, "shoulderSideCm") == 0) g_shSideCm.store(v);
            else if (strcmp(key, "shoulderBackCm") == 0) g_shBackCm.store(v);
            else if (strcmp(key, "elbowOut") == 0) g_elbowOut.store(v);
            else if (strcmp(key, "armLength") == 0 && v > 0.3f) bones::set_arm_length(v);
            else if (strcmp(key, "scaleSkin") == 0) bones::set_arm_scale_skin(v != 0.0f);
        }
        fclose(f);
    }
    bones::set_arms(g_armsOn.load());
}

void reflect_point(float p[3], const float P[3], const float n[3]) {
    const float k = 2.0f * ((p[0] - P[0]) * n[0] + (p[1] - P[1]) * n[1] + (p[2] - P[2]) * n[2]);
    for (int i = 0; i < 3; ++i) p[i] -= k * n[i];
}

// Publish both clusters' shoulder + elbow-pole targets for this frame.
void publish_arm_targets(const FrameContext& ctx, bool mirrorPose, bool gunPlane) {
    if (!g_armsLoaded) arms_load();
    if (g_armsSave.exchange(false)) arms_save();
    bones::set_arms(g_armsOn.load(std::memory_order_relaxed));
    if (!g_armsOn.load(std::memory_order_relaxed)) {
        bones::set_arm_target(0, false, nullptr, nullptr, nullptr);
        bones::set_arm_target(1, false, nullptr, nullptr, nullptr);
        return;
    }
    bvr::vr::HeadPose head{};
    if (!ctx.vrDriving || !bvr::vr::peek_head_pose(head)) {
        bones::set_arm_target(0, false, nullptr, nullptr, nullptr);
        bones::set_arm_target(1, false, nullptr, nullptr, nullptr);
        return;
    }
    const float hp[3] = {head.px, head.py, head.pz};
    const float hq[4] = {head.qx, head.qy, head.qz, head.qw};
    const GamePose hw = xr_pose_to_game(ctx, hp, hq);

    // Torso yaw: chase the head only past a deadzone, plus a slow drift.
    const float headYaw = static_cast<float>(hw.rot.yaw) / kRotUnitsPerRadian;
    const uint64_t now = GetTickCount64();
    float dt = g_torsoMs ? (now - g_torsoMs) / 1000.0f : 0.0f;
    g_torsoMs = now;
    if (dt > 0.1f) dt = 0.1f;
    if (!g_torsoValid) {
        g_torsoYaw = headYaw;
        g_torsoValid = true;
    }
    float diff = headYaw - g_torsoYaw;
    while (diff > kPi) diff -= 2.0f * kPi;
    while (diff < -kPi) diff += 2.0f * kPi;
    const float dz = 25.0f / kRadToDeg;
    if (diff > dz) g_torsoYaw += diff - dz;
    else if (diff < -dz) g_torsoYaw += diff + dz;
    g_torsoYaw += diff * (1.0f - expf(-dt / 1.5f)) * 0.5f;

    FRotator tr{0, static_cast<int32_t>(g_torsoYaw * kRotUnitsPerRadian), 0};
    float f[3], r[3], u[3];
    ue_rot_basis(tr, f, r, u);
    const float uu = ctx.worldScale / 100.0f;
    const float down = g_shDownCm.load() * uu, side = g_shSideCm.load() * uu,
                back = g_shBackCm.load() * uu, out = g_elbowOut.load();
    const bool lh = bvr::input::left_handed();
    for (int c = 0; c < 2; ++c) {
        // Which side this cluster's arm hangs from: in the mirrored rig each
        // cluster keeps its own side; unmirrored, it follows its controller.
        float sgn = c == 1 ? 1.0f : -1.0f;
        if (!mirrorPose && lh) sgn = -sgn;
        float S[3], P[3], O[3];
        for (int i = 0; i < 3; ++i) {
            S[i] = (&hw.loc.x)[i] - u[i] * down + r[i] * side * sgn - f[i] * back;
            P[i] = -u[i] + r[i] * out * sgn - f[i] * 0.3f;
            O[i] = r[i] * sgn; // this arm's outward side
        }
        if (gunPlane) {
            reflect_point(S, g_hdH, g_hdN);
            reflect_point(S, g_eyePlaneQ, g_eyePlaneN);
            float t[3];
            reflect_vec(P, g_hdN, t);
            reflect_vec(t, g_eyePlaneN, P);
            reflect_vec(O, g_hdN, t);
            reflect_vec(t, g_eyePlaneN, O);
        }
        bones::set_arm_target(c, true, S, P, O);
    }
}

// Pose source for a hand's MODEL: the grip pose in grip-placement mode,
// otherwise the historical choice (aim pose = the laser's ray).
bool model_uses_aim_pose() {
    return !g_gripPlace.load(std::memory_order_relaxed) &&
           g_useAimPose.load(std::memory_order_relaxed);
}

// Grip-placement: turn a GRIP pose (game space, fine-tune trims and offsets
// already applied in the grip's own frame) into the drive target that puts
// Jack's palm on it. False = no palm frame yet (the pose is left as is).
bool to_anchor(const FrameContext& ctx, int hand, bool driven, GamePose& gp) {
    if (!g_gripPlace.load(std::memory_order_relaxed)) return false;
    float gq[4], oq[4], ol[3];
    ue_rot_to_quat(gp.rot, gq);
    const float gl[3] = {gp.loc.x, gp.loc.y, gp.loc.z};
    const float depth = g_palmDepthCm.load(std::memory_order_relaxed) * ctx.worldScale / 100.0f;
    if (!bones::grip_to_anchor(hand, driven, gl, gq, depth, ol, oq)) return false;
    static const float kX[3] = {1.0f, 0.0f, 0.0f}, kZ[3] = {0.0f, 0.0f, 1.0f};
    float f[3], u[3];
    quat_rotate(oq[0], oq[1], oq[2], oq[3], kX, f);
    quat_rotate(oq[0], oq[1], oq[2], oq[3], kZ, u);
    gp.rot = basis_to_rot(f, u);
    gp.loc = {ol[0], ol[1], ol[2]};
    return true;
}

// ---- Magazine watch: is the gun in hand empty? ------------------------------
// The engine reloads on its own when a magazine runs dry, and that reload is
// indistinguishable from a fire-cycle animation (pump, wrench) by timing. So
// each gun's magazine counter is LEARNED: the int in the holdable (or its
// weapon object) that drops by exactly the shot count between shots, three
// times running, with a sane value. The smallest such counter is the magazine
// (a total-ammo count would be >= it). Read-only, per weapon, per session.
constexpr int kClipWords = 0x800 / 4;
struct ClipSource {
    void* obj = nullptr;
    int32_t prev[kClipWords];
    uint8_t score[kClipWords];
    bool havePrev = false;
};
struct ClipWatch {
    std::string key;
    ClipSource src[2];
    uint32_t shots = 0;
    int foundSrc = -1, foundWord = -1;
};
ClipWatch g_clip;
std::map<std::string, std::pair<int, int>> g_clipLearned; // key -> (source, word)

bool read_block(const void* src, void* out, size_t n) {
    __try {
        memcpy(out, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void clip_tick() {
    void* objs[2] = {nullptr, nullptr};
    hands::current_holdable(&objs[0]);
    objs[1] = aim::learned_weapon_object();
    if (objs[1] == objs[0]) objs[1] = nullptr;
    const std::string key = aim::active_weapon_key();
    if (key != g_clip.key || objs[0] != g_clip.src[0].obj) {
        g_clip.key = key;
        g_clip.shots = aim::player_shot_count();
        g_clip.foundSrc = g_clip.foundWord = -1;
        auto it = g_clipLearned.find(key);
        if (it != g_clipLearned.end()) {
            g_clip.foundSrc = it->second.first;
            g_clip.foundWord = it->second.second;
        }
        for (int k = 0; k < 2; ++k) {
            g_clip.src[k].obj = objs[k];
            g_clip.src[k].havePrev = false;
            memset(g_clip.src[k].score, 0, sizeof g_clip.src[k].score);
        }
    }
    for (int k = 0; k < 2; ++k)
        if (g_clip.src[k].obj != objs[k]) {
            g_clip.src[k].obj = objs[k];
            g_clip.src[k].havePrev = false;
            memset(g_clip.src[k].score, 0, sizeof g_clip.src[k].score);
        }
    const uint32_t shots = aim::player_shot_count();
    if (shots != g_clip.shots) {
        const int32_t n = static_cast<int32_t>(shots - g_clip.shots);
        g_clip.shots = shots;
        int bestSrc = -1, bestWord = -1;
        int32_t bestVal = 0x7fffffff;
        for (int k = 0; k < 2; ++k) {
            ClipSource& cs = g_clip.src[k];
            if (!cs.obj) continue;
            int32_t cur[kClipWords];
            if (!read_block(cs.obj, cur, sizeof cur)) {
                cs.havePrev = false;
                continue;
            }
            if (cs.havePrev)
                for (int i = 0; i < kClipWords; ++i) {
                    const int32_t d = cs.prev[i] - cur[i];
                    if (d == n && cur[i] >= 0 && cur[i] < 1000) {
                        if (cs.score[i] < 255) ++cs.score[i];
                    } else if (d != 0) {
                        cs.score[i] = 0;
                    }
                    if (cs.score[i] >= 3 && cur[i] < bestVal) {
                        bestVal = cur[i];
                        bestSrc = k;
                        bestWord = i;
                    }
                }
            memcpy(cs.prev, cur, sizeof cur);
            cs.havePrev = true;
        }
        if (bestSrc >= 0 && (bestSrc != g_clip.foundSrc || bestWord != g_clip.foundWord)) {
            g_clip.foundSrc = bestSrc;
            g_clip.foundWord = bestWord;
            g_clipLearned[key] = {bestSrc, bestWord};
            BVR_LOG("[hands] %s: magazine counter learned (%s +0x%X, now %d)", key.c_str(),
                    bestSrc == 0 ? "holdable" : "weapon object", bestWord * 4, bestVal);
        }
    }
    bool empty = false;
    if (g_clip.foundSrc >= 0 && g_clip.src[g_clip.foundSrc].obj) {
        int32_t v = -1;
        if (read_block(static_cast<uint8_t*>(g_clip.src[g_clip.foundSrc].obj) + g_clip.foundWord * 4, &v, 4))
            empty = v == 0;
    }
    bones::set_clip_empty(empty);
}

// ---- Per-weapon barrel angle (barrel.ini, keyed by weapon class name) ----
// "Bullets follow the barrel" takes each gun's idle forward as its barrel. Most
// models are authored straight down the view; a few are toed in toward the
// crosshair. The defaults are those models' angles from the shipped aim
// profile (the pistol's and crossbow's sideways trims - the other guns ship
// at ~0). Stored in the rig's own un-mirrored frame, so one value serves both
// hands; the F10 sliders show it as you see it.
struct BarrelAngle {
    float yaw = 0.0f, pitch = 0.0f;
};
std::mutex g_barrelMx;
std::map<std::string, BarrelAngle> g_barrelAngle; // user-set entries only
std::string g_barrelKey;                          // the weapon in hand
bool g_barrelLoaded = false;

BarrelAngle barrel_default(const std::string& key) {
    if (key == "Pistol") return {-4.20f, 0.0f};
    if (key == "Crossbow") return {-6.65f, 0.0f};
    return {};
}

void barrel_ini_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\barrel.ini", bvr::log::data_dir());
}

void load_barrel_angles() { // caller holds g_barrelMx
    g_barrelLoaded = true;
    wchar_t path[MAX_PATH];
    barrel_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char key[64] = {};
        BarrelAngle a;
        if (line[0] != '#' && sscanf_s(line, "%63[^=]=%f,%f", key, static_cast<unsigned>(sizeof key),
                                       &a.yaw, &a.pitch) == 3)
            g_barrelAngle[key] = a;
    }
    fclose(f);
}

void save_barrel_angles() { // caller holds g_barrelMx
    wchar_t path[MAX_PATH];
    barrel_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# BioShock VR - each weapon's barrel angle in its idle pose, for\n");
    fprintf(f, "# \"Bullets follow the gun barrel\". <WeaponClassName>=<yaw>,<pitch> (deg,\n");
    fprintf(f, "# right+/up+, right-handed rig frame). Delete a line for the default.\n");
    for (const auto& [k, v] : g_barrelAngle) fprintf(f, "%s=%.2f,%.2f\n", k.c_str(), v.yaw, v.pitch);
    fclose(f);
}

BarrelAngle barrel_angle_for(const std::string& key) { // caller holds g_barrelMx
    if (!g_barrelLoaded) load_barrel_angles();
    auto it = g_barrelAngle.find(key);
    return it != g_barrelAngle.end() ? it->second : barrel_default(key);
}

// Barrel aim: the gun's rendered barrel (hand-rig bones 43 -> 44), taken from
// the weapon hand's drive target BEFORE the recoil kick (recoil stays visual)
// and in the rig's own space, then mirrored about the head when the viewmodel
// mirror is on - what you SEE, in both mirror modes. Published for the next
// shot (aim.cpp) and, as an offset from the weapon grip, for the laser.
void publish_barrel(const FrameContext& ctx, const GamePose& gpV, bool mirrorPose,
                    const GamePose& headW) {
    float d0[3], m0[3];
    BarrelAngle ba;
    {
        std::lock_guard<std::mutex> lk(g_barrelMx);
        g_barrelKey = aim::active_weapon_key();
        ba = barrel_angle_for(g_barrelKey);
    }
    if (!bones::barrel_dir_target(ba.yaw, ba.pitch, d0) || !bones::muzzle_ref_offset(m0)) {
        aim::set_barrel(false, nullptr, nullptr, nullptr, nullptr);
        return;
    }
    float f[3], r[3], u[3];
    ue_rot_basis(gpV.rot, f, r, u);
    float o[3], d[3];
    for (int i = 0; i < 3; ++i) {
        o[i] = (&gpV.loc.x)[i] + f[i] * m0[0] + r[i] * m0[1] + u[i] * m0[2];
        d[i] = f[i] * d0[0] + r[i] * d0[1] + u[i] * d0[2];
    }
    if (mirrorPose) {
        float hf[3], hr[3], hu[3];
        ue_rot_basis(headW.rot, hf, hr, hu);
        const float H[3] = {headW.loc.x, headW.loc.y, headW.loc.z};
        reflect_point(o, H, hr);
        float t[3];
        reflect_vec(d, hr, t);
        memcpy(d, t, sizeof t);
    }
    const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 1e-4f) {
        aim::set_barrel(false, nullptr, nullptr, nullptr, nullptr);
        return;
    }
    for (float& c : d) c /= dl;
    // Laser: the same barrel as an offset from the weapon GRIP pose (XR), so the
    // render thread can follow the controller at full rate.
    float lo[3] = {0, 0, 0}, ldir[3] = {0, 0, -1};
    bool laserOk = false;
    bvr::vr::HeadPose hp{};
    if (bvr::vr::get_hand_pose(1, false, hp)) {
        float xo[3], xt[3];
        game_point_to_xr(ctx, FVector{o[0], o[1], o[2]}, xo);
        const float k = ctx.worldScale * 0.5f;
        game_point_to_xr(ctx, FVector{o[0] + d[0] * k, o[1] + d[1] * k, o[2] + d[2] * k}, xt);
        float xd[3] = {xt[0] - xo[0], xt[1] - xo[1], xt[2] - xo[2]};
        const float xl = sqrtf(xd[0] * xd[0] + xd[1] * xd[1] + xd[2] * xd[2]);
        if (xl > 1e-5f) {
            for (float& c : xd) c /= xl;
            const float rel[3] = {xo[0] - hp.px, xo[1] - hp.py, xo[2] - hp.pz};
            quat_rotate(-hp.qx, -hp.qy, -hp.qz, hp.qw, rel, lo);
            quat_rotate(-hp.qx, -hp.qy, -hp.qz, hp.qw, xd, ldir);
            laserOk = true;
        }
    }
    aim::set_barrel(true, o, d, laserOk ? lo : nullptr, laserOk ? ldir : nullptr);
}

// Plasmid casts: with the hand on your real palm, the cast has to leave JACK'S
// palm, not the controller's pointing-ray origin a few centimetres away. The
// palm point (rig space -> as SEEN when mirroring) and its offset from the
// plasmid hand's grip pose (XR, for the laser) go to aim.cpp; the direction
// stays the calibrated pointing ray.
void publish_palm(const FrameContext& ctx, const GamePose& gpV, bool mirrorPose,
                  const GamePose& headW) {
    float rel[3];
    const float depth = g_palmDepthCm.load(std::memory_order_relaxed) * ctx.worldScale / 100.0f;
    if (!bones::palm_in_target(0, true, depth, rel)) {
        aim::set_palm(false, nullptr, nullptr);
        return;
    }
    float f[3], r[3], u[3], p[3];
    ue_rot_basis(gpV.rot, f, r, u);
    for (int i = 0; i < 3; ++i) p[i] = (&gpV.loc.x)[i] + f[i] * rel[0] + r[i] * rel[1] + u[i] * rel[2];
    if (mirrorPose) {
        float hf[3], hr[3], hu[3];
        ue_rot_basis(headW.rot, hf, hr, hu);
        const float H[3] = {headW.loc.x, headW.loc.y, headW.loc.z};
        reflect_point(p, H, hr);
    }
    float lo[3] = {0, 0, 0};
    bool laserOk = false;
    bvr::vr::HeadPose hp{};
    if (bvr::vr::get_hand_pose(0, false, hp)) {
        float xo[3];
        game_point_to_xr(ctx, FVector{p[0], p[1], p[2]}, xo);
        const float d[3] = {xo[0] - hp.px, xo[1] - hp.py, xo[2] - hp.pz};
        quat_rotate(-hp.qx, -hp.qy, -hp.qz, hp.qw, d, lo);
        laserOk = true;
    }
    aim::set_palm(true, p, laserOk ? lo : nullptr);
}

// Always-visible off hand: the OTHER role's controller through the same chain
// as the driven hand - XR pose (two-handed pose when gripped), the head-mirror
// when mirroring, that hand's model trims and offsets, and in gun-plane mode
// the same head-then-gun-plane reflections the driven rig got, because the
// render reflects the whole rig about ONE plane.
bool off_hand_pose(const FrameContext& ctx, int offHand, bool mirrorPose, bool gunPlane,
                   const Kick* kick, GamePose& out) {
    bvr::vr::HeadPose hp{};
    if (!ctx.vrDriving ||
        !bvr::vr::get_hand_pose(offHand, model_uses_aim_pose(), hp))
        return false;
    float pos[3] = {hp.px, hp.py, hp.pz};
    float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
    if (mirrorPose) {
        bvr::vr::HeadPose head{};
        if (!bvr::vr::peek_head_pose(head)) return false;
        mirror_pose_about_head(head, pos, quat);
    }
    GamePose gp = model_pose_from_xr(ctx, pos, quat,
                                     g_rotPitchDeg[offHand].load(std::memory_order_relaxed),
                                     g_rotYawDeg[offHand].load(std::memory_order_relaxed),
                                     g_rotRollDeg[offHand].load(std::memory_order_relaxed));
    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    const float uuPerCm = ctx.worldScale / 100.0f;
    const float of = g_posFwdCm[offHand].load(std::memory_order_relaxed) * uuPerCm;
    const float orr = g_posRightCm[offHand].load(std::memory_order_relaxed) * uuPerCm;
    const float ou = g_posUpCm[offHand].load(std::memory_order_relaxed) * uuPerCm;
    gp.loc = {gp.loc.x + fwd[0] * of + right[0] * orr + up[0] * ou,
              gp.loc.y + fwd[1] * of + right[1] * orr + up[1] * ou,
              gp.loc.z + fwd[2] * of + right[2] * orr + up[2] * ou};
    to_anchor(ctx, offHand, false, gp); // grip placement: palm on your palm
    if (kick) apply_kick(*kick, gp); // held on the gun: ride its recoil
    if (gunPlane) {
        reflect_pose(gp, g_hdH, g_hdN);
        reflect_pose(gp, g_eyePlaneQ, g_eyePlaneN);
    }
    out = gp;
    return true;
}

// ---- v6 per-weapon muzzle trim (mirror.ini, keyed by weapon class name) ----
// The automatic part - bone 44's sideways offset from the attach bone - covers
// each gun's barrel position; this trim absorbs any gun whose effects spawn a
// little off bone 44. The overlay slider edits the ACTIVE weapon's trim.
std::map<std::string, float> g_mirrorTrim;
std::map<std::string, float> g_flashY; // v7: measured flash offset per weapon (rig-local right, UU)
std::string g_trimKey;
bool g_trimLoaded = false, g_trimDirty = false;
uint64_t g_trimDirtyMs = 0;

void mirror_ini_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\mirror.ini", bvr::log::data_dir());
}

void load_mirror_trims() {
    g_trimLoaded = true;
    wchar_t path[MAX_PATH];
    mirror_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64] = {};
        float v = 0.0f;
        if (line[0] != '#' &&
            sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) == 2) {
            const char* at = strstr(key, "@muzzle2");
            // v7 "@flash" / v8 "@muzzle" entries were learned from single sightings
            // (a parked light, an equip frame) - discard them; v9 learns by median.
            if (strstr(key, "@flash") || (strstr(key, "@muzzle") && !at)) continue;
            if (at && fabsf(v) > 15.0f) continue;
            g_mirrorTrim[key] = v;
            if (at) g_flashY[std::string(key, at - key)] = v;
        }
    }
    fclose(f);
}

void save_mirror_trims() {
    wchar_t path[MAX_PATH];
    mirror_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# BioShock VR mirror probe - per-weapon muzzle trim (UU), on top of the\n");
    fprintf(f, "# automatic bone-44 offset. <WeaponClassName>=<uu>\n");
    for (const auto& [k, v] : g_mirrorTrim) fprintf(f, "%s=%.2f\n", k.c_str(), v);
    fclose(f);
    BVR_LOG("[mirror] per-weapon muzzle trims saved (%u)",
            static_cast<unsigned>(g_mirrorTrim.size()));
}

// Called once per driven frame: swaps the slider to the active weapon's trim
// on a weapon change, captures slider edits into the map, saves after a pause.
float sync_weapon_trim(int hand) {
    if (!g_trimLoaded) load_mirror_trims();
    std::string key = hand == 1 ? std::string(aim::active_weapon_key()) : "PlasmidHand";
    if (key.empty()) key = "UnknownWeapon";
    const uint64_t now = GetTickCount64();
    if (key != g_trimKey) {
        g_trimKey = key;
        auto it = g_mirrorTrim.find(key);
        bvr::vm_mirror::set_plane_shift_uu(it != g_mirrorTrim.end() ? it->second : 0.0f);
    } else {
        const float cur = bvr::vm_mirror::plane_shift_uu();
        auto it = g_mirrorTrim.find(key);
        if (it == g_mirrorTrim.end() ? cur != 0.0f : it->second != cur) {
            g_mirrorTrim[key] = cur;
            g_trimDirty = true;
            g_trimDirtyMs = now;
        }
    }
    if (g_trimDirty && now - g_trimDirtyMs > 1500) {
        g_trimDirty = false;
        save_mirror_trims();
    }
    return bvr::vm_mirror::plane_shift_uu();
}

// ---- v7 muzzle-flash locator ------------------------------------------------
// The weapon skeletons carry NO muzzle bone (ENGINE_NOTES session 20: the
// shotgun is SG_Body/SG_Pump/SG_Shell), so the flash must be a separate object
// the game attaches when firing. Unreal actors keep an Attached array; its
// offset is found by searching the AHands actor for an array holding the
// weapon. Every frame the gun's and the hands' Attached arrays are watched;
// anything new is logged with its class and its position in the ENGINE rig's
// frame (fwd/right/up from the attach anchor). A flash-named child's
// sideways offset IS the plane shift that makes the engine flash coincide
// with the mirrored barrel (the v5 derivation: s = rig-local right offset).
struct TArr {
    void* data;
    int32_t num, max;
};
int g_attachedOff = -1;
bool g_attachedSearched = false;
constexpr int kMaxKids = 24;
struct Kid {
    void* obj;
    int frames;
    bool logged;
    int prio;             // muzzle priority (0 = not a muzzle effect)
    char owner[48];       // owning weapon class ("" = attached to the hands)
    char cls[48];         // class name, for the effects probe
};
Kid g_kids[kMaxKids] = {};

bool read_tarr(const void* p, TArr& out) {
    uint8_t raw[12];
    if (!read12(p, raw)) return false;
    memcpy(&out, raw, sizeof out);
    return true;
}

void find_attached_offset(void* hands, void* weapon) {
    g_attachedSearched = true;
    int found = 0;
    for (int off = 0x40; off < 0x900; off += 4) {
        TArr a{};
        if (!read_tarr(static_cast<uint8_t*>(hands) + off, a)) continue;
        if (!a.data || a.num < 1 || a.num > 64 || a.max < a.num || a.max > 1024) continue;
        for (int i = 0; i < a.num; ++i) {
            void* e = nullptr;
            if (!read_ptr(static_cast<void**>(a.data) + i, &e)) break;
            if (e == weapon) {
                BVR_LOG("[mirror] Attached-array candidate at actor+0x%X (%d entries) holds the "
                        "weapon",
                        off, a.num);
                if (g_attachedOff < 0) g_attachedOff = off;
                ++found;
                break;
            }
        }
    }
    if (!found)
        BVR_LOG("[mirror] no array on the hands actor holds the weapon - flash locator off");
}

// Priority of an attached effect as the weapon's MUZZLE point: 2 = a
// *MuzzleFX (ShotgunMuzzleFX, Pistol_MuzzleFX), 1 = another muzzle effect
// (LiquidNitrogenMuzzle, Shotgun_MuzzleSmoke), 0 = not one. Lights are never
// muzzle points: DynamicLightMuzzleFlash is parked hundreds of UU away between
// shots (v7's run learned -504 UU from it and threw the rig across the room).
int muzzle_priority(const wchar_t* cls) {
    if (!cls) return 0;
    wchar_t low[64];
    int i = 0;
    for (; cls[i] && i < 63; ++i) low[i] = static_cast<wchar_t>(towlower(cls[i]));
    low[i] = 0;
    if (wcsstr(low, L"light")) return 0;
    if (!wcsstr(low, L"muzzle")) return 0;
    return wcsstr(low, L"muzzlefx") ? 2 : 1;
}
constexpr float kMaxMuzzleLateralUu = 15.0f; // any real barrel is within this
constexpr float kMaxChildReachUu = 150.0f;   // parked/hidden objects are far beyond
std::map<std::string, int> g_flashPrio;

// v9 learning: every frame a muzzle effect is attached, its sideways offset is
// sampled into a per-weapon window; the offset is learned only from the MEDIAN
// once enough samples agree. v8 learned from one sighting and caught equip
// frames (the Shotgun read -36.9 mid-switch, the Pistol -14.1).
struct MuzzleSamples {
    float v[31];
    int n = 0, next = 0, prio = 0;
    uint64_t lastSaveMs = 0;
};
std::map<std::string, MuzzleSamples> g_muzzleSamples;

void sample_muzzle(const char* owner, int prio, float mr) {
    MuzzleSamples& m = g_muzzleSamples[owner];
    if (prio < m.prio) return;          // a better-ranked effect is feeding this weapon
    if (prio > m.prio) { m = {}; m.prio = prio; }
    m.v[m.next] = mr;
    m.next = (m.next + 1) % 31;
    if (m.n < 31) ++m.n;
    if (m.n < 12) return;
    float sorted[31];
    memcpy(sorted, m.v, sizeof(float) * m.n);
    std::sort(sorted, sorted + m.n);
    const float med = sorted[m.n / 2];
    int agree = 0;
    for (int i = 0; i < m.n; ++i)
        if (fabsf(m.v[i] - med) <= 1.0f) ++agree;
    if (agree * 3 < m.n * 2) return;    // < 2/3 within 1 UU: still noisy, wait
    auto it = g_flashY.find(owner);
    const uint64_t now = GetTickCount64();
    if (it != g_flashY.end() && fabsf(it->second - med) < 0.3f) return;
    g_flashY[owner] = med;
    g_mirrorTrim[std::string(owner) + "@muzzle2"] = med;
    if (now - m.lastSaveMs > 2000) {
        m.lastSaveMs = now;
        save_mirror_trims();
        BVR_LOG("[mirror] %s muzzle offset learned: %.2f UU (median of %d, %d agree)", owner,
                med, m.n, agree);
    }
}

// Watch one actor's Attached array: log new children once, sample muzzle
// effects every frame.
void watch_kids(void* parent, const char* who, const GamePose& rig, const char* weaponKey) {
    if (!parent || g_attachedOff < 0) return;
    TArr a{};
    if (!read_tarr(static_cast<uint8_t*>(parent) + g_attachedOff, a)) return;
    if (!a.data || a.num < 0 || a.num > 64) return;
    float f[3], r[3], u[3];
    ue_rot_basis(rig.rot, f, r, u);
    const bool parentIsWeapon = parent == weapon_actor();
    for (int i = 0; i < a.num; ++i) {
        void* e = nullptr;
        if (!read_ptr(static_cast<void**>(a.data) + i, &e) || !e) continue;
        Kid* k = nullptr;
        for (auto& kk : g_kids)
            if (kk.obj == e) k = &kk;
        if (!k) {
            for (auto& kk : g_kids)
                if (!kk.obj) { k = &kk; break; }
            if (!k) continue;
            *k = {};
            k->obj = e;
            const wchar_t* cls = patterns::object_class_name(e);
            if (cls) _snprintf_s(k->cls, sizeof k->cls, _TRUNCATE, "%S", cls);
            k->prio = muzzle_priority(cls);
            if (parentIsWeapon) {
                const wchar_t* wc = patterns::object_class_name(parent);
                if (wc) _snprintf_s(k->owner, sizeof k->owner, _TRUNCATE, "%S", wc);
            }
        }
        ++k->frames;
        if (k->frames < 3) continue; // a fresh spawn is placed a frame or two late
        float loc[3];
        if (!read12(static_cast<uint8_t*>(e) + patterns::kActorLocOffset, loc)) continue;
        const float rel[3] = {loc[0] - rig.loc.x, loc[1] - rig.loc.y, loc[2] - rig.loc.z};
        const float mf = rel[0] * f[0] + rel[1] * f[1] + rel[2] * f[2];
        const float mr = rel[0] * r[0] + rel[1] * r[1] + rel[2] * r[2];
        const float mu = rel[0] * u[0] + rel[1] * u[1] + rel[2] * u[2];
        const float reach = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        const bool usable = reach <= kMaxChildReachUu && fabsf(mr) <= kMaxMuzzleLateralUu;
        if (!k->logged) {
            k->logged = true;
            BVR_LOG("[mirror] new child of %s (%s): class %s at rig-local fwd %.1f right %.1f "
                    "up %.1f UU%s",
                    who, k->owner[0] ? k->owner : weaponKey, k->cls[0] ? k->cls : "?", mf, mr,
                    mu, k->prio && usable && k->owner[0] ? " <- muzzle effect (sampling)" : "");
        }
        // With the effects flip live, attached effects sit where WE put them -
        // measuring them would feed the plane back into itself. They need no
        // learned offset then anyway (any plane lands them on the mirrored gun).
        if (k->prio > 0 && usable && k->owner[0] && !(g_effectsFlip.load() && g_attachHookLive))
            sample_muzzle(k->owner, k->prio, mr);
    }
}

// Effects probe (v9): the gun's own Location plus up to three of its attached
// effects, preferring steam, the muzzle FX and smoke - the watchpoints find
// whichever code repositions attached effects each frame.
std::atomic<bool> g_fxProbeRequest{false};
char g_fxProbeStatus[120] = "";

void start_fx_probe(void* weapon) {
    uintptr_t addr[4] = {};
    const char* labels[4] = {};
    static char lbl[4][40];
    addr[0] = reinterpret_cast<uintptr_t>(weapon) + patterns::kActorLocOffset;
    _snprintf_s(lbl[0], sizeof lbl[0], _TRUNCATE, "gun Location");
    labels[0] = lbl[0];
    int n = 1;
    static const char* kWant[] = {"steam", "muzzlefx", "smoke", "muzzle", "ion", ""};
    for (const char* want : kWant) {
        for (auto& kk : g_kids) {
            if (n >= 4) break;
            if (!kk.obj || !kk.owner[0] || !kk.cls[0]) continue;
            char low[48];
            int j = 0;
            for (; kk.cls[j] && j < 47; ++j) low[j] = static_cast<char>(tolower(kk.cls[j]));
            low[j] = 0;
            if (strstr(low, "light") || strstr(low, "upgrade") || strstr(low, "ammo")) continue;
            if (want[0] && !strstr(low, want)) continue;
            const uintptr_t ad = reinterpret_cast<uintptr_t>(kk.obj) + patterns::kActorLocOffset;
            bool dup = false;
            for (int q = 0; q < n; ++q) dup = dup || addr[q] == ad;
            if (dup) continue;
            addr[n] = ad;
            _snprintf_s(lbl[n], sizeof lbl[n], _TRUNCATE, "%s Location", kk.cls);
            labels[n] = lbl[n];
            ++n;
        }
    }
    if (n < 2) {
        strcpy_s(g_fxProbeStatus, "no gun effects attached yet - fire the gun once, then retry");
        return;
    }
    for (int q = n; q < 4; ++q) { // pad unused slots with the gun (harmless duplicates)
        addr[q] = addr[0];
        labels[q] = lbl[0];
    }
    bonewatch::request_custom(addr, labels);
    _snprintf_s(g_fxProbeStatus, sizeof g_fxProbeStatus, _TRUNCATE,
                "watching %d effect position(s) - keep firing for 2 s", n - 1);
}

void age_kids() {
    // Recycle only when the table is full, so a permanent attachment logs
    // once, while pooled flash objects re-used shot to shot still got their
    // first sighting logged.
    for (auto& kk : g_kids)
        if (!kk.obj) return;
    for (auto& kk : g_kids) kk = {};
}

// ---- Route B: re-apply the drive right after the engine writes the pose ----
// Measured (bonewatch, 2026-09-29): the plasmid effects read the wrist through
// the bone getter AFTER the engine's pose writer and BEFORE the CalcView drive,
// so they saw the authored hand. Hooking the writer and re-running the drive
// on its return puts OUR hand in the array for every reader that follows -
// effects, attachments, the renderer. drive() itself sees a fresh engine pose
// there (anchor != last written) and adopts it as the reference first, so
// engine animation (reload, equip) keeps flowing through exactly as before;
// the CalcView drive later that frame then finds its own write and refines it.
// Uses the last CalcView's inputs (one frame old) - the hand moves < 1 cm.
using PoseWriteFn = bool(__fastcall*)(void* self, void* edx);
PoseWriteFn g_poseWriteOrig = nullptr;
bool g_poseHookTried = false, g_poseHookLive = false;
std::atomic<bool> g_effectsFollow{true};
std::atomic<uint32_t> g_postDrives{0};
DWORD g_gameTid = 0;
FrameContext g_postCtx{};
void* g_postTarget = nullptr;
GamePose g_postGp{};
int g_postHand = -1;
uint64_t g_postStampMs = 0;
bool g_inPost = false;

bool __fastcall pose_write_detour(void* self, void* edx) {
    const bool r = g_poseWriteOrig(self, edx);
    if (!g_inPost && g_effectsFollow.load(std::memory_order_relaxed) && self &&
        self == bones::skeleton_instance() && GetCurrentThreadId() == g_gameTid &&
        g_postTarget && g_postHand >= 0 && GetTickCount64() - g_postStampMs < 150) {
        g_inPost = true;
        if (bones::drive(g_postCtx, g_postTarget, g_postGp, g_postHand))
            g_postDrives.fetch_add(1, std::memory_order_relaxed);
        g_inPost = false;
    }
    return r;
}

void install_pose_hook() {
    g_poseHookTried = true;
    uint8_t* target = const_cast<uint8_t*>(g_imageBase) + patterns::kPoseWriteRva;
    if (!bvr::pattern_scan::is_memory_valid(target, sizeof patterns::kPoseWritePrologue) ||
        memcmp(target, patterns::kPoseWritePrologue, sizeof patterns::kPoseWritePrologue) != 0) {
        BVR_LOG("[routeB] pose-writer prologue mismatch at %p - different game build? "
                "REFUSING hook (effects stay on the game's hand)",
                target);
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&pose_write_detour),
                      reinterpret_cast<void**>(&g_poseWriteOrig)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        BVR_LOG("[routeB] could not hook the pose writer at %p", target);
        return;
    }
    g_poseHookLive = true;
    BVR_LOG("[routeB] pose-writer hook ENABLED (rva 0x%X) - hand effects follow the driven "
            "hand",
            patterns::kPoseWriteRva);
}

// ---- Effects flip: mirror the gun's attached effects with the gun ----------
// The render mirror flips the gun in the PICTURE; the engine's gun stays right-
// handed and spawns its effects from right-handed positions, so only one point
// per gun could ever line up. Reflecting each attached effect's world
// transform about the SAME plane Pg, right after the engine positions it,
// puts every effect exactly on the visible (mirrored) gun - for any plane
// shift, so attached effects need no trim at all. Loose effects (the Tommy
// gun's flash is not attached) still use the per-gun trim.
using AttachUpdateFn = uint32_t(__fastcall*)(void* self, void* edx, void* parent, void* child,
                                             void* a3, void* a4);
AttachUpdateFn g_attachOrig = nullptr;

// Only EFFECTS get flipped. Some attachments are gun PARTS drawn in the
// viewmodel (foreground) scene - VisibleAmmoModel (the crossbow bolt, the
// cartridges), ShotgunShell during a reload - and the render mirror already
// reflects those; flipping their position too reflected them twice (first
// in-headset run: cartridges and the bolt floated off the gun). Classified by
// class name, cached per object; unknown names are left alone (safe side).
bool effect_class(const wchar_t* cls) {
    if (!cls) return false;
    wchar_t low[64];
    int i = 0;
    for (; cls[i] && i < 63; ++i) low[i] = static_cast<wchar_t>(towlower(cls[i]));
    low[i] = 0;
    static const wchar_t* kParts[] = {L"ammo", L"shell", L"bolt", L"upgrade", L"model",
                                      L"clip", L"magazine", L"canister"};
    for (const wchar_t* w : kParts)
        if (wcsstr(low, w)) return false;
    static const wchar_t* kFx[] = {L"fx", L"steam", L"smoke", L"muzzle", L"spark", L"ion",
                                   L"flash", L"light", L"trail", L"glow", L"heat", L"cold",
                                   L"emitter", L"vapor", L"mist", L"fire", L"flame", L"drip"};
    for (const wchar_t* w : kFx)
        if (wcsstr(low, w)) return true;
    return false;
}

std::map<void*, bool> g_effectCache; // child object -> is an effect

bool is_effect(void* child) {
    auto it = g_effectCache.find(child);
    if (it != g_effectCache.end()) return it->second;
    if (g_effectCache.size() > 512) g_effectCache.clear(); // pooled objects churn slowly
    const bool fx = effect_class(patterns::object_class_name(child));
    g_effectCache[child] = fx;
    return fx;
}

void reflect_point(float p[3]) {
    const float d = (p[0] - g_pgQ[0]) * g_pgN[0] + (p[1] - g_pgQ[1]) * g_pgN[1] +
                    (p[2] - g_pgQ[2]) * g_pgN[2];
    for (int i = 0; i < 3; ++i) p[i] -= 2.0f * d * g_pgN[i];
}

uint32_t __fastcall attach_update_detour(void* self, void* edx, void* parent, void* child,
                                         void* a3, void* a4) {
    const uint32_t r = g_attachOrig(self, edx, parent, child, a3, a4);
    if (!child || !parent || parent != g_flipWeapon ||
        !g_effectsFlip.load(std::memory_order_relaxed) ||
        GetCurrentThreadId() != g_gameTid || GetTickCount64() - g_pgStampMs > 150)
        return r;
    if (!is_effect(child)) return r; // gun parts: the render mirror already flips them
    uint8_t* c = static_cast<uint8_t*>(child);
    float loc[3];
    int32_t rot[3];
    if (!read12(c + patterns::kActorLocOffset, loc) || !read12(c + patterns::kActorRotOffset, rot))
        return r;
    const float dq[3] = {loc[0] - g_pgQ[0], loc[1] - g_pgQ[1], loc[2] - g_pgQ[2]};
    if (dq[0] * dq[0] + dq[1] * dq[1] + dq[2] * dq[2] > 150.0f * 150.0f)
        return r; // parked/hidden attachments (upgrade models) stay where they are
    reflect_point(loc);
    FRotator fr{rot[0], rot[1], rot[2]};
    float f[3], rr[3], u[3], f2[3], u2[3];
    ue_rot_basis(fr, f, rr, u);
    reflect_vec(f, g_pgN, f2);
    reflect_vec(u, g_pgN, u2);
    const FRotator out = basis_to_rot(f2, u2); // right implied: proper frame
    const int32_t rot2[3] = {out.pitch, out.yaw, out.roll};
    if (write12(c + patterns::kActorLocOffset, loc) && write12(c + patterns::kActorRotOffset, rot2))
        g_flips.fetch_add(1, std::memory_order_relaxed);
    return r;
}

void install_attach_hook() {
    g_attachHookTried = true;
    uint8_t* target = const_cast<uint8_t*>(g_imageBase) + patterns::kAttachUpdateRva;
    if (!bvr::pattern_scan::is_memory_valid(target, sizeof patterns::kAttachUpdatePrologue) ||
        memcmp(target, patterns::kAttachUpdatePrologue, sizeof patterns::kAttachUpdatePrologue) !=
            0) {
        BVR_LOG("[fxflip] attach-update prologue mismatch at %p - REFUSING hook", target);
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&attach_update_detour),
                      reinterpret_cast<void**>(&g_attachOrig)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        BVR_LOG("[fxflip] could not hook the attach update at %p", target);
        return;
    }
    g_attachHookLive = true;
    BVR_LOG("[fxflip] attach-update hook ENABLED (rva 0x%X) - gun effects follow the mirror",
            patterns::kAttachUpdateRva);
}

void route_b_ui() {
    bool fx = g_effectsFlip.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Flip the gun's effects with the gun (smoke, steam, flash)", &fx))
        g_effectsFlip.store(fx, std::memory_order_relaxed);
    bool on = g_effectsFollow.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Plasmid effects follow your real hand", &on))
        g_effectsFollow.store(on, std::memory_order_relaxed);
    if (!bvr::overlay::dev_tools()) return;

    ImGui::SeparatorText("Hands + effects diagnostics");
    static uint32_t s_l = 0, s_r = 0, s_last = 0, s_rate = 0;
    static uint64_t s_ms = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_ms >= 1000) {
        const uint32_t f = g_flips.load(std::memory_order_relaxed);
        s_r = f - s_l;
        s_l = f;
        const uint32_t c = g_postDrives.load(std::memory_order_relaxed);
        s_rate = c - s_last;
        s_last = c;
        s_ms = now;
    }
    ImGui::Text("attach hook: %s | effects flipped/s %u",
                g_attachHookLive ? "live" : (g_attachHookTried ? "REFUSED (see log)" : "not yet"),
                s_r);
    ImGui::Text("pose-writer hook: %s | re-applies/s %u",
                g_poseHookLive ? "live" : (g_poseHookTried ? "REFUSED (see log)" : "not yet"),
                s_rate);
    if (ImGui::Button("Watch gun effects (2 s - keep firing)")) g_fxProbeRequest.store(true);
    if (g_fxProbeStatus[0]) ImGui::TextWrapped("%s", g_fxProbeStatus);
    bonewatch::draw_debug_ui();
}

bool mirror_wanted() {
    return bvr::input::left_handed() && bvr::input::mirror_viewmodel();
}

} // namespace

void on_calcview(const FrameContext& ctx) {
    MirrorArm mirrorArm;
    g_gameTid = GetCurrentThreadId();
    if (!g_poseHookTried && g_effectsFollow.load(std::memory_order_relaxed)) install_pose_hook();
    if (!g_attachHookTried && mirror_wanted() && g_effectsFlip.load(std::memory_order_relaxed))
        install_attach_hook();
    {
        int nb = 0;
        void* arr = bones::bone_array(&nb);
        bonewatch::tick(arr, nb);
    }
    if (mirror_wanted()) {
        configure_mirror_once();
        bvr::vm_mirror::set_game_ui(&route_b_ui);
        bvr::vm_mirror::set_eye_half_uu(camera::ipd_mm() / 2000.0f * ctx.worldScale);
        bvr::vm_mirror::set_world_scale(ctx.worldScale);
    }
    // Overlay request, applied from THIS thread (same rule as aim.cpp: the
    // render thread must never touch engine state directly).
    int pending = g_pendingEnable.exchange(-1, std::memory_order_relaxed);
    if (pending == 1) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_handsScanFails = 0; // re-enable: scan promptly
        BVR_LOG("[hands] ON (overlay) - viewmodel follows the controller");
    } else if (pending == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[hands] OFF (overlay) - engine placement restored");
    }

    // World change: the old actors died with the old world, and recycled heap
    // addresses must never be written to. The scale bookkeeping is dropped, not
    // restored - restore would write into a stranger.
    if (ctx.pc != g_lastPc) {
        if (g_lastPc && (g_handsActor || g_weaponActor))
            BVR_LOG("[hands] world changed - actor caches cleared");
        g_lastPc = ctx.pc;
        g_handsActor = nullptr;
        g_weaponActor = nullptr;
        g_lastHandsScanMs = 0;
        g_lastWeaponScanMs = 0;
        g_handsScanFails = 0; // new world: scan promptly again
        bones::on_world_change();
    }

    // One-shot probe: describe every instance of both classes, choose none.
    if (g_probeLeft > 0) {
        --g_probeLeft;
        find_hands_actor(ctx, true);
        find_weapon_actor(ctx, true);
    }

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // Cutscene guard, same predicate the aim ray uses: during normal play the
    // view actor is the player's own pawn.
    bool gameplayView = false;
    if (ctx.viewActor) {
        void* vtbl = nullptr;
        if (read_ptr(ctx.viewActor, &vtbl))
            gameplayView = (to_rva(vtbl) == patterns::kShockPlayerVtableRva);
        if (!gameplayView && ctx.viewActor == ctx.pc) gameplayView = true;
    }
    // Session 29: the cinematic gate, made EXPLICIT. Until now the hands
    // stopped during a cutscene only as a side effect of ctx.vrDriving going
    // false with the head drive - an accident, not a contract, and one that
    // authored+look breaks by design (it drives the head again, which would
    // hand the controllable rig straight back over the authored animation).
    if (gameplayView && bvr::hud::cinematic_hold() &&
        bvr::vr::cine_drive() != bvr::vr::CineDrive::Off) {
        gameplayView = false;
        // Release HERE, not only on the cinematic entry edge. Measured in
        // headset (session 29): switching drive mode to `off` mid-cutscene
        // resumes the drive, which collapses the inactive hand - and switching
        // back gates the only code that can restore it, because
        // restore_hidden() lives inside drive(). The hand then stays collapsed
        // for the rest of the scene (log: hiddenHand=0 cacheAge=32578ms at the
        // exit edge, 32.5 s being exactly the moment the gate re-closed).
        // Releasing where the suppression happens closes that by construction,
        // and release() is idempotent - it self-limits to one real pass.
        bones::release("hands gated for cinematic");
    }

    // Two-handed grip first: while held it changes the poses read below.
    twohand::tick(active_hand() == 1, gameplayView && ctx.vrDriving);
    if (!gameplayView) return;

    bool gunMode = g_mode.load(std::memory_order_relaxed) == 0;
    if (gunMode) {
        // Live-proven dead end kept only as a future detach experiment: the
        // renderer draws an ATTACHED weapon from its attachment matrix, so
        // writing the weapon actor's own transform changes nothing (and the
        // 2026-07-25 evening session suggests it can desync the attach state).
        // Refuse rather than write.
        static bool warned = false;
        if (!warned) {
            warned = true;
            BVR_LOG("[hands] gun mode is inert on this engine (attached weapons render "
                    "from the attach matrix) - use mode hands");
        }
        return;
    }
    void* target = find_hands_actor(ctx, false);
    if (!target) return;

    // Where the model goes. The hand is resolved once and used for the pose,
    // the aim trim, and both per-hand model offsets below.
    const int hand = active_hand();
    GamePose gp{};
    bool mirrorPose = false; // the rig pose below is the head-reflected one
    GamePose headW{};        // the HMD in game space, for the gun-plane mode
    uint64_t now = GetTickCount64();
    if (now < g_test.deadline) {
        // Camera-relative lane: proves the write lands, no pose math involved.
        gp.rot.yaw = ctx.camYaw + static_cast<int32_t>(g_test.yawDeg * kRotUnitsPerDegree);
        gp.rot.pitch = ctx.camPitch + static_cast<int32_t>(g_test.pitchDeg * kRotUnitsPerDegree);
        gp.rot.roll = 0;
        float dir[3];
        ue_rot_to_dir(gp.rot, dir);
        gp.loc = {ctx.camX + dir[0] * g_test.distUu, ctx.camY + dir[1] * g_test.distUu,
                  ctx.camZ + dir[2] * g_test.distUu};
    } else {
        float pos[3], quat[4];
        FrameContext mapCtx = ctx;
        if (now < g_sim.deadline) {
            // Synthetic XR pose through the REAL mapping path: a fixed spot a
            // hand would occupy, oriented by the sim angles.
            pos[0] = 0.15f;  // meters right of the recenter origin
            pos[1] = -0.20f; // below it
            pos[2] = -0.35f; // in front (XR forward is -Z)
            xr_local_trim_quat(g_sim.pitchDeg / kRadToDeg, g_sim.yawDeg / kRadToDeg,
                               g_sim.rollDeg / kRadToDeg, quat);
            // POSITION zero only. The recenter YAW is deliberately left alone
            // (session 17): forcing it to 0 here made the parked synthetic hand
            // BODY-locked while a real controller is RECENTER-locked, so with
            // the M7.5 yaw transfer armed the parked gun swung by the full head
            // angle - indistinguishable by eye from the sessions-12-16
            // head-coupling defect, but a pure artifact of this line. It was a
            // no-op before the transfer existed: every flat baseline arms
            // `simhead 0 0 0` first, which sets the recenter yaw to exactly 0.
            mapCtx.recenterPx = mapCtx.recenterPy = mapCtx.recenterPz = 0.0f;
        } else {
            bvr::vr::HeadPose hp{};
            bool aimPose = model_uses_aim_pose();
            if (!ctx.vrDriving || !bvr::vr::get_hand_pose(hand, aimPose, hp)) return;
            pos[0] = hp.px;
            pos[1] = hp.py;
            pos[2] = hp.pz;
            quat[0] = hp.qx;
            quat[1] = hp.qy;
            quat[2] = hp.qz;
            quat[3] = hp.qw;
            // Mirror probe: the rig gets the reflected pose (a virtual right
            // controller for the gun); the aim ray, laser and swing keep the
            // real one - they never pass through here.
            if (mirror_wanted() && g_mode.load(std::memory_order_relaxed) == 2) {
                bvr::vr::HeadPose head{};
                if (bvr::vr::peek_head_pose(head)) {
                    mirror_pose_about_head(head, pos, quat);
                    const float hpPos[3] = {head.px, head.py, head.pz};
                    const float hpQuat[4] = {head.qx, head.qy, head.qz, head.qw};
                    headW = xr_pose_to_game(mapCtx, hpPos, hpQuat);
                    mirrorPose = true;
                    mirrorArm.arm = true;
                }
            }
            if (bvr::b1r::bones::telemetry_on()) {
                static uint64_t lastTlm = 0;
                if (now - lastTlm >= 200) {
                    lastTlm = now;
                    BVR_LOG("[tlm] ctrl%d xr p=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f) "
                            "pose=%s",
                            hand, hp.px, hp.py, hp.pz, hp.qx, hp.qy, hp.qz, hp.qw,
                            aimPose ? "aim" : "grip");
                }
            }
        }

        // Mesh-alignment trim (per hand), composed in the controller's local
        // frame so it holds at EVERY controller orientation. The chain is a
        // pure function in frame_context.h, shared with `vraim synccheck`.
        gp = model_pose_from_xr(mapCtx, pos, quat,
                                g_rotPitchDeg[hand].load(std::memory_order_relaxed),
                                g_rotYawDeg[hand].load(std::memory_order_relaxed),
                                g_rotRollDeg[hand].load(std::memory_order_relaxed));
        // The aim calibration trim is deliberately NOT applied to the model
        // (session 18 part 3): re-trimming the ray must not move the tuned
        // model. The legacy `aligntrim` euler coupling was DELETED in session
        // 20 - euler adds after conversion were the wrong algebra everywhere
        // but the tuning pose, and the unification left nothing for it to do.
    }

    // Position offset rides the final (trimmed) frame: "2 cm forward" means
    // along the barrel as finally oriented.
    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    float uuPerCm = ctx.worldScale / 100.0f;
    float of = g_posFwdCm[hand].load(std::memory_order_relaxed) * uuPerCm;
    float orr = g_posRightCm[hand].load(std::memory_order_relaxed) * uuPerCm;
    float ou = g_posUpCm[hand].load(std::memory_order_relaxed) * uuPerCm;
    float loc[3] = {gp.loc.x + fwd[0] * of + right[0] * orr + up[0] * ou,
                    gp.loc.y + fwd[1] * of + right[1] * orr + up[1] * ou,
                    gp.loc.z + fwd[2] * of + right[2] * orr + up[2] * ou};

    if (g_mode.load(std::memory_order_relaxed) == 2) {
        bool gunPlaneLive = false;
        // BONES (M7-v2): the actor stays engine-placed (eye anchor, correct
        // culling, correct engine-side FX anchoring) and the hand CLUSTER
        // moves to the controller instead.
        gp.loc = {loc[0], loc[1], loc[2]};
        if (hand == 1) {
            bones::set_active_weapon(aim::active_weapon_key());
            clip_tick();
        } else {
            bones::set_clip_empty(false);
        }
        bones::set_anim_log(bvr::overlay::dev_tools());
        to_anchor(ctx, hand, true, gp); // grip placement: palm on your palm
        const GamePose gpPreKick = gp;  // the barrel the bullets follow (no recoil)
        ability_haptics(hand);
        Kick kick{};
        const bool kicked = hand == 1 && recoil_kick(ctx, gp, twohand::gripped(), kick);
        if (kicked) apply_kick(kick, gp);
        if (mirrorPose) {
            const float trim = sync_weapon_trim(hand);
            float muzzle[3] = {0.0f, 0.0f, 0.0f};
            // Priority: the flash offset MEASURED for this weapon (v7) - it is
            // where the engine really spawns the flash; else bone 44 (v6).
            auto learned = hand == 1 ? g_flashY.find(g_trimKey) : g_flashY.end();
            const bool haveFlash = learned != g_flashY.end();
            bool havePalm = false;
            if (hand == 0 && g_palmPlane.load(std::memory_order_relaxed)) {
                // The plasmid hand's effects hang off the ENGINE hand, which
                // is placed at the reflection of the hand you see about this
                // plane: through the anchor bone it put them a hand's width to
                // the side. Through the PALM (where the casts leave) the
                // engine palm and the visible palm are the same point - the
                // gun's muzzle rule, applied to the hand.
                float rel[3];
                const float depth = g_palmDepthCm.load(std::memory_order_relaxed) *
                                    ctx.worldScale / 100.0f;
                if (bones::palm_in_target(0, true, depth, rel)) {
                    muzzle[1] = rel[1];
                    havePalm = true;
                }
            }
            const bool haveMuzzle =
                havePalm || haveFlash || (hand == 1 && bones::muzzle_ref_offset(muzzle));
            if (haveFlash) muzzle[1] = learned->second;
            float shift = (haveMuzzle ? muzzle[1] : 0.0f) + trim;
            if (shift > 20.0f) shift = 20.0f; // a sane plane; never throw the rig away
            if (shift < -20.0f) shift = -20.0f;
            const bool gunPlane =
                bvr::vm_mirror::plane_mode() == bvr::vm_mirror::PlaneMode::Gun &&
                apply_gun_plane(headW, gp, shift);
            gunPlaneLive = gunPlane;
            g_flipWeapon = gunPlane && hand == 1 ? weapon_actor() : nullptr;
            if (!g_flipWeapon) g_pgStampMs = 0;
            if (!gunPlane) {
                g_eyePlaneLive = false;
                publish_head_planes(ctx); // v4 behaviour
            }
            static uint64_t s_noteMs = 0;
            if (now - s_noteMs > 250) {
                s_noteMs = now;
                char note[160];
                _snprintf_s(note, sizeof note, _TRUNCATE,
                            "Tuning: %s | muzzle offset (%s) %.2f UU + trim %.2f%s",
                            g_trimKey.c_str(),
                            havePalm    ? "palm"
                            : haveFlash ? "measured flash"
                            : haveMuzzle ? "bone 44 guess"
                                         : "none",
                            haveMuzzle ? muzzle[1] : 0.0f, trim,
                            gunPlane ? "" : " | gun plane unavailable - head plane used");
                bvr::vm_mirror::set_ui_note(note);
            }
        }
        // The weapon-scale lane rides the same per-frame slot (session 61);
        // it no-ops at wscale 1.0 and drops itself on weapon switches.
        bones::wskel_drive();
        publish_arm_targets(ctx, mirrorPose, gunPlaneLive);
        // The other hand: tracked at its own controller (or collapsed as before).
        {
            GamePose offGp{};
            const bool held = hand == 1 && twohand::gripped();
            // Shape flags first: the grip placement solves against the pose
            // the off hand will actually be drawn with.
            bones::set_off_follow(held);
            bones::set_off_preview(hand == 1 && twohand::preview_grip());
            const bool track =
                twohand::off_hand_enabled() &&
                off_hand_pose(ctx, 1 - hand, mirrorPose, gunPlaneLive,
                              held && kicked ? &kick : nullptr, offGp);
            bones::set_off_target(track, track ? &offGp : nullptr);
        }
        bonewatch::mark_drive_begin();
        const bool drove = bones::drive(ctx, target, gp, hand);
        bonewatch::mark_drive_end();
        if (!drove) {
            mirrorArm.arm = false; // rig not driven: never reflect an unplaced rig
            g_postTarget = nullptr;
            return;
        }
        if (hand == 1 && g_gripPlace.load(std::memory_order_relaxed) &&
            g_barrelAim.load(std::memory_order_relaxed))
            publish_barrel(ctx, gpPreKick, mirrorPose, headW);
        else
            aim::set_barrel(false, nullptr, nullptr, nullptr, nullptr);
        if (hand == 0 && g_gripPlace.load(std::memory_order_relaxed) &&
            g_palmCast.load(std::memory_order_relaxed))
            publish_palm(ctx, gpPreKick, mirrorPose, headW);
        else
            aim::set_palm(false, nullptr, nullptr);
        // Route B: the inputs the pose-writer hook re-applies next engine write.
        g_postCtx = ctx;
        g_postTarget = target;
        g_postGp = gp;
        g_postHand = hand;
        g_postStampMs = now;
        if (mirrorPose && hand == 1) {
            void* weapon = weapon_actor();
            if (weapon && !g_attachedSearched) find_attached_offset(target, weapon);
            watch_kids(weapon, "weapon", gp, g_trimKey.c_str());
            watch_kids(target, "hands", gp, g_trimKey.c_str());
            age_kids();
            if (weapon && g_fxProbeRequest.exchange(false)) start_fx_probe(weapon);
        }
    } else {
        uint8_t* p = static_cast<uint8_t*>(target);
        bool wrote = write12(p + patterns::kActorLocOffset, loc);
        if (g_writeRot.load(std::memory_order_relaxed)) {
            int32_t rot[3] = {gp.rot.pitch, gp.rot.yaw, gp.rot.roll};
            wrote = write12(p + patterns::kActorViewDirOffset, rot) || wrote;
        }
        if (!wrote) {
            g_handsActor = nullptr; // the write faulted - stop trusting this pointer
            return;
        }
    }
    g_writes.fetch_add(1, std::memory_order_relaxed);
    g_lastX.store(loc[0], std::memory_order_relaxed);
    g_lastY.store(loc[1], std::memory_order_relaxed);
    g_lastZ.store(loc[2], std::memory_order_relaxed);
    g_lastPitch.store(gp.rot.pitch, std::memory_order_relaxed);
    g_lastYaw.store(gp.rot.yaw, std::memory_order_relaxed);
    g_lastRoll.store(gp.rot.roll, std::memory_order_relaxed);
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        log_status();
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "on") == 0) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_handsScanFails = 0; // re-enable: scan promptly
        BVR_LOG("[hands] ON - viewmodel follows the controller");
        log_status();
    } else if (strcmp(verb, "off") == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[hands] OFF - engine placement restored");
    } else if (strcmp(verb, "mode") == 0) {
        int mode = strncmp(rest, "gun", 3) == 0     ? 0
                   : strncmp(rest, "hands", 5) == 0 ? 1
                                                    : 2;
        g_mode.store(mode, std::memory_order_relaxed);
        BVR_LOG("[hands] mode = %s",
                mode == 0   ? "GUN (inert - renderer ignores an attached weapon's actor fields)"
                : mode == 1 ? "HANDS (actor pinning - retired, kept for A/B)"
                            : "BONES (M7-v2: hand cluster follows the controller)");
    } else if (strcmp(verb, "pose") == 0) {
        bool aim = strncmp(rest, "aim", 3) == 0;
        g_useAimPose.store(aim, std::memory_order_relaxed);
        BVR_LOG("[hands] pose source = %s", aim ? "AIM (matches laser + bullets)"
                                                : "GRIP (physical hand axis)");
    } else if (strcmp(verb, "scale") == 0) {
        // "scale [l|r|both] <f>" - no side = both hands. Session 61: the
        // lever the s16 dead ends never tested (bones.h set_scale).
        int side = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            side = rest[0] == 'r' ? 1 : 0;
            nums = rest + 2;
        } else if (strncmp(rest, "both", 4) == 0) {
            nums = rest + 4;
        }
        float f = 0.0f;
        if (sscanf_s(nums, "%f", &f) == 1 && f > 0.0f) {
            bones::set_scale(side, f);
            BVR_LOG("[hands] scale %s = %.3f (L=%.3f R=%.3f; 1.0 = authored)",
                    side < 0 ? "both" : side == 1 ? "right" : "left", f, bones::scale(0),
                    bones::scale(1));
        } else {
            BVR_LOG("[hands] usage: vrhands scale [l|r|both] <f> (current L=%.3f R=%.3f; "
                    "probe mode via vrbones scalemode)",
                    bones::scale(0), bones::scale(1));
        }
    } else if (strcmp(verb, "wscale") == 0) {
        float f = 0.0f;
        if (sscanf_s(rest, "%f", &f) == 1 && f > 0.0f) {
            bones::set_weapon_scale(f);
            BVR_LOG("[hands] weapon scale = %.3f (1.0 = authored, lane drops itself)",
                    bones::weapon_scale());
        } else {
            BVR_LOG("[hands] usage: vrhands wscale <f> (current %.3f; uniform about the "
                    "grip, per-frame drive of the holdable's own skeleton)",
                    bones::weapon_scale());
        }
    } else if (strcmp(verb, "probe") == 0) {
        int n = 1;
        if (sscanf_s(rest, "%d", &n) != 1 || n <= 0) n = 1;
        if (n > 30) n = 30;
        g_probeLeft = n;
        BVR_LOG("[hands] probe armed for %d frame(s) - listing AHands + player weapons", n);
    } else if (strcmp(verb, "hand") == 0) {
        int mode = rest[0] == 'l' ? 0 : rest[0] == 'r' ? 1 : 2;
        g_handMode.store(mode, std::memory_order_relaxed);
        BVR_LOG("[hands] hand = %s", mode == 0 ? "LEFT" : mode == 1 ? "RIGHT" : "auto");
    } else if (strcmp(verb, "pos") == 0) {
        // "pos [l|r] <fwd> <right> <up>" - no side = both hands (the legacy
        // form, kept so the acceptance harness and old scripts still work).
        int side = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            side = rest[0] == 'r' ? 1 : 0;
            nums = rest + 1;
            while (*nums == ' ' || *nums == '\t') ++nums;
        }
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(nums, "%f %f %f", &f, &r, &u) == 3) {
            for (int h = 0; h < 2; ++h) {
                if (side >= 0 && h != side) continue;
                g_posFwdCm[h].store(f, std::memory_order_relaxed);
                g_posRightCm[h].store(r, std::memory_order_relaxed);
                g_posUpCm[h].store(u, std::memory_order_relaxed);
            }
            BVR_LOG("[hands] pos offset (%s) fwd%+.1f right%+.1f up%+.1f cm",
                    side < 0 ? "both" : side == 1 ? "right" : "left", f, r, u);
        } else {
            BVR_LOG("[hands] usage: vrhands pos [l|r] <fwdCm> <rightCm> <upCm>");
        }
    } else if (strcmp(verb, "rot") == 0) {
        int side = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            side = rest[0] == 'r' ? 1 : 0;
            nums = rest + 1;
            while (*nums == ' ' || *nums == '\t') ++nums;
        }
        float p = 0.0f, y = 0.0f, r = 0.0f;
        if (sscanf_s(nums, "%f %f %f", &p, &y, &r) == 3) {
            for (int h = 0; h < 2; ++h) {
                if (side >= 0 && h != side) continue;
                g_rotPitchDeg[h].store(p, std::memory_order_relaxed);
                g_rotYawDeg[h].store(y, std::memory_order_relaxed);
                g_rotRollDeg[h].store(r, std::memory_order_relaxed);
            }
            BVR_LOG("[hands] rot trim (%s) pitch%+.1f yaw%+.1f roll%+.1f deg",
                    side < 0 ? "both" : side == 1 ? "right" : "left", p, y, r);
        } else {
            BVR_LOG("[hands] usage: vrhands rot [l|r] <pitchDeg> <yawDeg> <rollDeg>");
        }
    } else if (strcmp(verb, "writerot") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_writeRot.store(on, std::memory_order_relaxed);
        BVR_LOG("[hands] rotation write %s", on ? "ON" : "off (position only)");
    } else if (strcmp(verb, "fname") == 0) {
        // Session 20: the name-system gate. `fname <idx>` resolves any name
        // index; `fname weapon` reads the cached weapon actor's attach-bone
        // FName (+0xF0 {index, number}) - the stage-4 acceptance.
        if (strncmp(rest, "weapon", 6) == 0) {
            void* w = weapon_valid(g_weaponActor) ? g_weaponActor
                                                  : bvr::b1r::aim::learned_weapon_object();
            if (!weapon_valid(w)) {
                BVR_LOG("[hands] fname: no live weapon actor (fire once so the aim seam "
                        "learns it)");
                return;
            }
            const int32_t* nm = reinterpret_cast<const int32_t*>(
                static_cast<uint8_t*>(w) + patterns::kActorAttachBoneNameOffset);
            const wchar_t* t = patterns::fname_text(nm[0]);
            BVR_LOG("[hands] weapon attach-bone FName idx=%d num=%d -> '%S' "
                    "(GNames count %d)",
                    nm[0], nm[1], t ? t : L"<unresolved>", patterns::fname_count());
        } else {
            int idx = 0;
            if (sscanf_s(rest, "%d", &idx) == 1) {
                const wchar_t* t = patterns::fname_text(idx);
                BVR_LOG("[hands] fname %d -> '%S' (GNames count %d)", idx,
                        t ? t : L"<unresolved>", patterns::fname_count());
            } else {
                BVR_LOG("[hands] usage: vrhands fname <index>|weapon");
            }
        }
    } else if (strcmp(verb, "swaykill") == 0) {
        if (strncmp(rest, "status", 6) == 0)
            BVR_LOG("[hands] swaykill %s", bones::sway_kill() ? "ON" : "off");
        else
            bones::set_sway_kill(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "hideinactive") == 0) {
        bones::set_hide_inactive(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "save") == 0) {
        save_config();
    } else if (strcmp(verb, "reload") == 0) {
        load_config();
        log_status();
    } else if (strcmp(verb, "test") == 0) {
        float yaw = 0.0f, pitch = 0.0f, dist = 60.0f;
        int hold = 0;
        int n = sscanf_s(rest, "%f %f %f %d", &yaw, &pitch, &dist, &hold);
        if (n < 2) {
            BVR_LOG("[hands] usage: vrhands test <yawDeg> <pitchDeg> [distUU] [holdMs]");
            return;
        }
        if (n < 3 || dist <= 0.0f) dist = 60.0f;
        if (hold <= 0) hold = 30000;
        if (hold > 120000) hold = 120000;
        g_test.yawDeg = yaw;
        g_test.pitchDeg = pitch;
        g_test.distUu = dist;
        g_test.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
        BVR_LOG("[hands] test placement: yaw %+.1f pitch %+.1f dist %.0f UU for %d ms", yaw,
                pitch, dist, hold);
    } else if (strcmp(verb, "simpose") == 0) {
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        int hold = 0;
        int n = sscanf_s(rest, "%f %f %f %d", &yaw, &pitch, &roll, &hold);
        if (n < 3) {
            BVR_LOG("[hands] usage: vrhands simpose <yawDeg> <pitchDeg> <rollDeg> [holdMs]");
            return;
        }
        if (hold <= 0) hold = 30000;
        if (hold > 120000) hold = 120000;
        g_sim.yawDeg = yaw;
        g_sim.pitchDeg = pitch;
        g_sim.rollDeg = roll;
        g_sim.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
        BVR_LOG("[hands] sim pose: yaw %+.1f pitch %+.1f roll %+.1f for %d ms (real mapping "
                "path, synthetic controller)",
                yaw, pitch, roll, hold);
    } else if (strcmp(verb, "testclear") == 0) {
        g_test.deadline = 0;
        g_sim.deadline = 0;
        BVR_LOG("[hands] test + sim placements cleared");
    } else if (strcmp(verb, "status") == 0) {
        log_status();
    } else {
        BVR_LOG("[hands] unknown command '%s' (on|off|mode gun|hands|pose aim|grip|scale|"
                "probe|hand|pos|rot|writerot|hideinactive|save|reload|test|simpose|"
                "testclear|status)",
                verb);
    }
}

bool active() {
    return g_enabled.load(std::memory_order_relaxed) &&
           (g_weaponActor != nullptr || g_handsActor != nullptr);
}

void save_offsets() {
    save_config();
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Hands + weapon (M7)")) return;

    bool gpl = g_gripPlace.load();
    if (ImGui::Checkbox("Hands match your real hands (grip pose)", &gpl)) {
        g_gripPlace.store(gpl);
        save_config();
    }
    if (gpl) {
        float pd = g_palmDepthCm.load();
        if (ImGui::SliderFloat("palm depth (cm into the fist)", &pd, -2.0f, 5.0f)) {
            g_palmDepthCm.store(pd);
            save_config();
        }
        bool ba = g_barrelAim.load();
        if (ImGui::Checkbox("Bullets follow the gun barrel", &ba)) {
            g_barrelAim.store(ba);
            save_config();
        }
        if (ba) {
            // As SEEN: in the mirrored view the rig's right is your left.
            const bool seenFlip =
                bvr::input::left_handed() && bvr::input::mirror_viewmodel();
            std::lock_guard<std::mutex> lk(g_barrelMx);
            if (!g_barrelKey.empty()) {
                BarrelAngle a = barrel_angle_for(g_barrelKey);
                float yawSeen = seenFlip ? -a.yaw : a.yaw;
                ImGui::TextDisabled("Barrel angle for %s (moves the laser and the shots):",
                                    g_barrelKey.c_str());
                bool changed = ImGui::SliderFloat("barrel left/right (deg)", &yawSeen, -12.0f, 12.0f, "%.2f");
                bool done = ImGui::IsItemDeactivatedAfterEdit();
                changed |= ImGui::SliderFloat("barrel down/up (deg)", &a.pitch, -8.0f, 8.0f, "%.2f");
                done |= ImGui::IsItemDeactivatedAfterEdit();
                if (changed) {
                    a.yaw = seenFlip ? -yawSeen : yawSeen;
                    g_barrelAngle[g_barrelKey] = a;
                }
                if (ImGui::Button("Default for this weapon")) {
                    g_barrelAngle.erase(g_barrelKey);
                    done = true;
                }
                if (done) save_barrel_angles();
            }
        }
        bool pc = g_palmCast.load();
        if (ImGui::Checkbox("Plasmid casts leave Jack's palm (off: the controller tip)", &pc)) {
            g_palmCast.store(pc);
            save_config();
        }
        bool pp = g_palmPlane.load();
        if (ImGui::Checkbox("Plasmid effects mirrored about the palm (off: the wrist)", &pp)) {
            g_palmPlane.store(pp);
            save_config();
        }
        ImGui::TextDisabled("The offset/trim sliders below now fine-tune from your palm.");
    }

    bool rc = g_recoilOn.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Recoil + haptics (every weapon, wrench hits, plasmid casts)", &rc))
        g_recoilOn.store(rc, std::memory_order_relaxed);
    float rs = g_recoilScale.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("recoil strength", &rs, 0.0f, 2.5f))
        g_recoilScale.store(rs, std::memory_order_relaxed);

    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Viewmodel follows the controller", &on))
        g_pendingEnable.store(on ? 1 : 0, std::memory_order_relaxed);

    bool aimPose = g_useAimPose.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Align to the AIM ray (matches laser; off = grip pose)", &aimPose))
        g_useAimPose.store(aimPose, std::memory_order_relaxed);

    int hand = g_handMode.load(std::memory_order_relaxed);
    if (ImGui::RadioButton("left", &hand, 0)) g_handMode.store(0, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("right", &hand, 1)) g_handMode.store(1, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::RadioButton("auto", &hand, 2)) g_handMode.store(2, std::memory_order_relaxed);

    // The six sliders below edit ONE hand's offsets - the selector picks
    // which (in-headset tuning wants one set of sliders, not twelve).
    // Separate from the drive-hand radio above: that picks which controller
    // OWNS the viewmodel, this picks which hand's numbers the sliders show.
    static int tuneHand = 1; // start on the weapon hand
    ImGui::Text("Tuning hand:");
    ImGui::SameLine();
    ImGui::RadioButton("L (plasmid)", &tuneHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("R (weapon)", &tuneHand, 1);

    float f = g_posFwdCm[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset forward (cm)", &f, -120.0f, 120.0f))
        g_posFwdCm[tuneHand].store(f, std::memory_order_relaxed);
    float r = g_posRightCm[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset right (cm)", &r, -120.0f, 120.0f))
        g_posRightCm[tuneHand].store(r, std::memory_order_relaxed);
    float u = g_posUpCm[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("offset up (cm)", &u, -120.0f, 120.0f))
        g_posUpCm[tuneHand].store(u, std::memory_order_relaxed);

    float rp = g_rotPitchDeg[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim pitch (deg)", &rp, -90.0f, 90.0f))
        g_rotPitchDeg[tuneHand].store(rp, std::memory_order_relaxed);
    float ry = g_rotYawDeg[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim yaw (deg)", &ry, -90.0f, 90.0f))
        g_rotYawDeg[tuneHand].store(ry, std::memory_order_relaxed);
    float rr = g_rotRollDeg[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("trim roll (deg)", &rr, -180.0f, 180.0f))
        g_rotRollDeg[tuneHand].store(rr, std::memory_order_relaxed);

    // Session 61: hand + weapon scale (deliberately independent of world
    // scale - the rig can be the wrong size while the world is right).
    // The hand slider edits the tuning hand's cluster; the weapon slider is
    // uniform about the grip and only binds skeletal holdables (the wrench
    // is a rigid mesh and stays authored).
    float hs = bones::scale(tuneHand);
    if (ImGui::SliderFloat("model scale (x, independent of worldscale)", &hs, 0.2f, 4.0f))
        bones::set_scale(tuneHand, hs);
    if (ImGui::Button("scale both hands to this")) bones::set_scale(-1, hs);
    float ws = bones::weapon_scale();
    if (ImGui::SliderFloat("WEAPON scale (uniform, about the grip)", &ws, 0.3f, 2.5f))
        bones::set_weapon_scale(ws);

    if (ImGui::Button("Save offsets")) save_config();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) load_config();

    bones::draw_debug_ui();

    ImGui::Text("weapon %p | hands %p | writes %u", g_weaponActor, g_handsActor,
                g_writes.load(std::memory_order_relaxed));
    ImGui::Text("last loc (%.0f %.0f %.0f) rot (%d %d %d)",
                g_lastX.load(std::memory_order_relaxed),
                g_lastY.load(std::memory_order_relaxed),
                g_lastZ.load(std::memory_order_relaxed),
                g_lastPitch.load(std::memory_order_relaxed),
                g_lastYaw.load(std::memory_order_relaxed),
                g_lastRoll.load(std::memory_order_relaxed));
}

bool grip_placement() { return g_gripPlace.load(std::memory_order_relaxed); }

void draw_arms_ui() {
    if (!ImGui::CollapsingHeader("Arms (experimental)")) return;
    bool on = g_armsOn.load();
    if (ImGui::Checkbox("Full arms (IK from the shoulders)", &on)) {
        g_armsOn.store(on);
        g_armsSave.store(true);
    }
    ImGui::TextDisabled("Shoulder position, from the centre of your head:");
    float v = g_shDownCm.load();
    if (ImGui::SliderFloat("shoulders down (cm)", &v, 5.0f, 40.0f)) {
        g_shDownCm.store(v);
        g_armsSave.store(true);
    }
    v = g_shSideCm.load();
    if (ImGui::SliderFloat("shoulders apart, each side (cm)", &v, 8.0f, 30.0f)) {
        g_shSideCm.store(v);
        g_armsSave.store(true);
    }
    v = g_shBackCm.load();
    if (ImGui::SliderFloat("shoulders back (cm)", &v, -10.0f, 25.0f)) {
        g_shBackCm.store(v);
        g_armsSave.store(true);
    }
    v = g_elbowOut.load();
    if (ImGui::SliderFloat("elbows out", &v, 0.0f, 1.5f)) {
        g_elbowOut.store(v);
        g_armsSave.store(true);
    }
    v = bones::arm_length();
    if (ImGui::SliderFloat("arm length (x Jack's)", &v, 0.6f, 1.8f)) {
        bones::set_arm_length(v);
        g_armsSave.store(true);
    }
    unsigned solves = 0, stretched = 0;
    bones::arm_stats(&solves, &stretched);
    static unsigned s_ls = 0, s_lt = 0, s_rs = 0, s_rt = 0;
    static uint64_t s_ms = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_ms >= 1000) {
        s_rs = solves - s_ls;
        s_rt = stretched - s_lt;
        s_ls = solves;
        s_lt = stretched;
        s_ms = now;
    }
    ImGui::Text("arms posed/s %u | at full reach/s %u", s_rs, s_rt);
    ImGui::TextDisabled("Lots of 'full reach'? Raise the arm length or lower the shoulders.");
    if (bvr::overlay::dev_tools()) {
        ImGui::Text("forearm roll L %.0f | R %.0f deg (elbow lifts past 80)",
                    bones::arm_twist_deg(0), bones::arm_twist_deg(1));
        bool sk = bones::arm_scale_skin();
        if (ImGui::Checkbox("Scale the arm skin like the hands", &sk)) {
            bones::set_arm_scale_skin(sk);
            g_armsSave.store(true);
        }
    }
}

void on_eye_camera(int eye, const float loc[3], int32_t pitch, int32_t yaw, int32_t roll) {
    if (eye < 0 || eye > 1 || !g_eyePlaneLive) return;
    if (!bvr::vm_mirror::armed() ||
        bvr::vm_mirror::plane_mode() != bvr::vm_mirror::PlaneMode::Gun)
        return;
    // Same frame only: the plane is written by on_calcview a moment earlier.
    if (GetTickCount64() - g_eyePlaneStampMs > 100) return;
    const int32_t rot[3] = {pitch, yaw, roll};
    publish_eye_plane(eye, loc, rot);
}

} // namespace bvr::b1r::hands
