// M7-v2 bone drive. See bones.h for the contract and ENGINE_NOTES "Skeleton /
// bone internals" for every offset's derivation.
//
// Write protocol, decided by the live probes (2026-07-26):
//  - The bone array is re-evaluated lazily behind a dirty flag. Our write runs
//    in the CalcView detour (after the engine tick placed everything), then
//    CLEARS the dirty flag so a render-side evaluate-if-dirty cannot rebuild
//    the pose over our values in the same frame.
//  - Before writing we compare the anchor bone against what WE last wrote: if
//    it changed, the engine re-evaluated since last frame and the array holds
//    a fresh animated pose - recapture it as the reference. If it did not
//    change, the engine skipped evaluation and the reference stays. Either
//    way the drive composes from an ENGINE pose, never from its own output
//    (no feedback accumulation).
//  - Disabling sets the dirty flag so the engine's next evaluation restores
//    its own pose - no restore bookkeeping of ours can go stale.

#include "game/bioshock1r/bones.h"

#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h" // backbuffer_dims: the lens laws are aspect-parameterised
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h" // cine_drive/name for the vrbones status residue line
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace bvr::b1r::bones {
namespace {

const uint8_t* g_imageBase = nullptr;

// Havok hkQsTransform: the pos/scale w lanes carry engine-owned values (one
// live bone held 35.02 in pos.w) - the drive writes pos.xyz and quat only.
struct Qts {
    float p[4];
    float q[4];
    float s[4];
};
static_assert(sizeof(Qts) == 48, "hkQsTransform is 48 bytes");

constexpr int kMaxBones = 128;

// Cached skeleton, revalidated every use.
void* g_skelInst = nullptr;
Qts* g_bones = nullptr;
int g_boneCount = 0;

// Reference pose (engine-evaluated) + the anchor value we last wrote.
Qts g_ref[kMaxBones];
bool g_refValid = false;
Qts g_lastWrittenAnchor[2]; // per hand
bool g_hasWritten[2] = {false, false};

// Everything the last drive() wrote, for reapply() (the stereo second pass).
struct CachedBone {
    int idx;
    float p[3];
    float q[4];
    float s[3];
    bool writeScale;
};
CachedBone g_cache[kMaxBones];
int g_cacheCount = 0;
struct CachedSleeve {
    int idx;
    float p[3];
    float s[3];
    float q[4];     // arms: the IK rotation
    bool writeQ;    // false for the collapse (position + zero scale only)
};
CachedSleeve g_cacheSleeve[16]; // both hands' sleeves (off-hand tracking)
int g_cacheSleeveCount = 0;

// Session 20 idle-sway kill (default ON; `vrhands swaykill on|off`): freeze
// the drive's reference pose against the idle animation's breathing. A fresh
// engine pose is adopted only when either wrist anchor moved past the
// thresholds - real animations (equip/reload/melee) pass through and
// re-freeze when they settle; the measured idle wobble (+-1.2 deg barrel
// direction, sub-UU positions) stays out. Acts only on the DRIVEN rig; the
// weapon's own skeleton (pump, cylinder) animates untouched.
std::atomic<bool> g_swayKill{true};
// 2x the MEASURED idle envelope vs a frozen snapshot (session 20, 1 Hz sway
// telemetry: dpos peaks 3.01 UU, dang peaks 4.6 deg) - real animations move
// the anchors tens of UU / tens of degrees, so the gap is wide on both sides.
constexpr float kSwayPosThreshUu = 6.0f;
constexpr float kSwayAngThreshDeg = 12.0f;
// After a real animation, keep tracking this long past the LAST threshold
// crossing so the freeze lands on the SETTLED pose, not the last big frame.
constexpr uint64_t kSwaySettleMs = 600;
// A shot is an animation by definition: the weapon's fire animation tracks
// for this long after each one whatever its size (see note_weapon_shot).
constexpr uint64_t kShotTrackMs = 450;
std::atomic<uint64_t> g_lastShotMs{0};
uint64_t g_lastBigDeltaMs = 0;
// Telemetry (1 Hz while frozen): the probe deltas the threshold judges, so
// the thresholds are set from measured idle amplitude, not guesses.
uint64_t g_lastSwayTlmMs = 0;

// Session 19: the whole INACTIVE hand collapses too - the drive poses only
// the active hand's cluster, so the other one stays engine-animated at the
// eye anchor and reads as a ghost hand. Cache mirrors g_cacheSleeve so the
// stereo second pass replays the same writes. writeScale is false for the
// weapon-attach bone: it hides by translation, never by scale (see drive()).
std::atomic<bool> g_hideInactive{true};
struct CachedHidden {
    int idx;
    float p[3];
    float s[3];
    bool writeScale;
};
CachedHidden g_cacheHidden[32];
int g_cacheHiddenCount = 0;
int g_hiddenHand = -1; // whose cluster is collapsed right now (game thread)
// Session 29: hoisted out of drive() (it was a function-static there) so
// release() can restore a collapsed sleeve. A latch only drive() could see was
// a latch only drive() could ever clear - which is precisely the shape of bug
// release() exists to fix.
bool g_wasCollapsed = false;
int g_collapsedHand = -1; // whose sleeve g_wasCollapsed refers to

// Off-hand tracking (always-visible off hand, BioVR port): where the cluster
// the drive does NOT own this frame goes. Stored rather than passed so the
// Route B re-drive (pose-writer hook) replays the same off-hand target.
bool g_offTrack = false;
struct PalmLocal {
    bool valid = false;
    float p[3];    // palm centroid, wrist-local (reference units)
    float q[4];    // palm frame (fwd = tube, right = grip +X, up), wrist-local
    float face[3]; // toward the palm surface, wrist-local unit vector
};
PalmLocal g_palm[2];
bool g_palmLogged[2] = {false, false};
float g_lastQa[4] = {0, 0, 0, 1};
float g_lastActorLoc[3] = {};
bool g_lastActorValid = false;
GamePose g_offGp{};
// Held on the gun (two-handed grip): the off hand also follows what the
// ENGINE's own off hand is doing relative to the gun - pumping the shotgun,
// cranking the chemical thrower - as a delta from that hand's settled pose.
bool g_offFollow = false;
bool g_offPreview = false; // grip shape without the follow (recording, grab zone)
Qts g_followBaseRef43; // the DRAWN gun's attach pose (reference) at rest
// v2 (the first cut followed the sway-killed REFERENCE, which only tracks an
// animation in bursts and freezes it mid-stroke): follow the engine's LIVE
// pose, read every time the engine re-evaluates, against a REST pose that is
// only accepted after the hand has held still for a moment.
Qts g_live[2];       // [0] engine left wrist, [1] weapon attach, latest engine write
Qts g_liveR;         // engine right wrist, same write (EVE probe)
bool g_liveValid = false;
Qts g_followBase[2]; // the rest pose the delta is measured from
bool g_followBaseValid = false;
// v3: the whole engine left hand (live) and its shape at rest. The held hand
// is drawn from the REST shape - a closed grip on the gun - and only moved by
// the engine's wrist motion, so an animation that relaxes the engine's
// fingers (or a sway-kill reference caught mid-stroke) cannot open the palm.
// The gun's barrel in the attach bone's (43) own frame: the game authors every
// weapon's idle pose pointing straight down the view (component +X), so at rest
// conj(q43) * X is the barrel - unlike 43->44, which runs from the grip (below
// the barrel) up to the tip and tilts every shot upward.
float g_barrelLocal[3] = {1, 0, 0};
float g_barrelRightLocal[3] = {0, 1, 0}; // the gun's right and up at rest, same frame:
float g_barrelUpLocal[3] = {0, 0, 1};    // a per-weapon barrel trim turns about these
bool g_barrelLocalValid = false;
Qts g_liveL[kMaxBones];
Qts g_gripShape[kMaxBones];
bool g_gripShapeValid = false;
// Grip placement, weapon hand: the palm-to-gun relation the controller is
// matched against, taken from the gun AT REST ([0] weapon wrist, [1] attach).
// Solving against the live reference instead pinned the palm and cancelled
// every authored gun motion that moves hand and gun together (the shotgun
// rocking through its pump, the grenade launcher's twist, the crossbow
// prime) - while the support-hand follow still applied that motion, so the
// held hand slid off the gun. With the rest relation the gun is rigid to the
// controller exactly as in aim-pose mode and animations play on top of it;
// at rest the reference IS the rest pose, so the palm still sits exactly on
// your palm. Dropped on a weapon or hand change (an equip keeps the palm
// pinned, then the new rest blends in); refreshed every frame at rest.
Qts g_placeRest[2];
bool g_placeRestValid = false;
uint64_t g_placeRestMs = 0;
char g_placeWeapon[64] = {};
bool g_refTracking = false; // the sway-killed reference is following an animation
uint64_t g_refTrackingSince = 0;
// Tracking an animation right now? A weapon whose idle alone exceeds the sway
// thresholds keeps the reference tracking forever; after 4 s (longer than any
// reload - the crossbow's runs 2.5 s) that is its
// idle, not an animation, and must not block rest acceptance.
bool ref_animating() {
    return g_refTracking && GetTickCount64() - g_refTrackingSince < 4000;
}
Qts g_restCand[2];   // candidate rest pose, and since when it has held
uint64_t g_restCandMs = 0;
constexpr float kRestHoldUu = 1.0f;     // idle sway is sub-UU (session 20)
constexpr uint64_t kRestHoldMs = 300;   // shorter than any pump pause
// The off hand's NEUTRAL pose (relaxed, open): the engine's own plasmid-hand
// idle, captured whenever the plasmid hand is raised and settled, saved to
// offhand_neutral.ini so it is there from the next launch. Used for the free
// off hand in weapon mode instead of the engine's grip-the-fore-end shape;
// the gripped hand keeps the grip shape.
Qts g_neutral[kMaxBones];
bool g_neutralValid = false;
bool g_neutralLoaded = false;
bool g_neutralSaved = false;
uint64_t g_neutralSince = 0;
bool g_neutralTaken = false; // this raise already captured

void neutral_path(wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%s\\offhand_neutral.ini", bvr::log::data_dir());
}

void load_neutral() {
    g_neutralLoaded = true;
    wchar_t path[MAX_PATH];
    neutral_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        int idx = -1;
        Qts b{};
        if (sscanf_s(line, "%d %f %f %f %f %f %f %f %f %f %f", &idx, &b.p[0], &b.p[1], &b.p[2],
                     &b.q[0], &b.q[1], &b.q[2], &b.q[3], &b.s[0], &b.s[1], &b.s[2]) == 11 &&
            idx >= 0 && idx < kMaxBones) {
            g_neutral[idx] = b;
            ++n;
        }
    }
    fclose(f);
    g_neutralValid = n > 0;
}

void save_neutral(int first, int last) {
    wchar_t path[MAX_PATH];
    neutral_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# off-hand neutral pose (engine plasmid-hand idle): bone p q s\n");
    for (int i = first; i <= last; ++i) {
        const Qts& b = g_neutral[i];
        fprintf(f, "%d %.4f %.4f %.4f %.6f %.6f %.6f %.6f %.4f %.4f %.4f\n", i, b.p[0], b.p[1],
                b.p[2], b.q[0], b.q[1], b.q[2], b.q[3], b.s[0], b.s[1], b.s[2]);
    }
    fclose(f);
    BVR_LOG("[bones] off-hand neutral pose captured and saved");
}

void* g_cacheSkelInst = nullptr;
uint64_t g_cacheMs = 0;

// Both clusters are baked (patterns.h) after live measurement; the lcluster
// command stays as a runtime override for future rig experiments.
std::atomic<int> g_lFirst{patterns::kBoneLClusterFirst}, g_lLast{patterns::kBoneLClusterLast},
    g_lAnchor{patterns::kBoneLWrist};
std::atomic<int> g_rAnchorOverride{-1};

// Session 61: per-cluster viewmodel scale (1.0 = authored; the BS2-shaped
// lever the s16 dead ends never actually tested). The cluster is still moved
// rigidly, but the anchor-relative translations shrink by s for EVERY cluster
// bone - the weapon-attach (43) and muzzle (44) bones MOVE with the scaled
// hand - while the hkQsTransform .s channel is written only for the bones the
// probe mode selects, and NEVER for 43/44: the s16 test wrote 43's .s at its
// authored value, and BS2's later live bisection localised the identical
// weapon blowup to the attach pivot's own scale CHANNEL being consumed
// (inverse-decomposed) by the attachment math. Leaving the channel engine-
// owned entirely is the untested cell this probe exists to measure.
// Scale rides the per-frame drive - one-shot pokes were BS2-proven not to
// render reliably - and the reference recapture PINS the scale rows of bones
// we scale-wrote (see the adopt block) so g_ref can never re-adopt our own
// write and compound refS * s^n.
// Defaults are the user's session-61 in-headset calibration (2026-08-14,
// "everything looks perfect") - the same standing rule the aim trims follow:
// the accepted preset becomes the shipped default. 1.0 = authored size.
std::atomic<float> g_scale[2] = {0.793f, 0.793f}; // [0] left, [1] right
// Probe modes (vrbones scalemode <n>): which cluster bones get the .s write.
// 0 = all except attach/muzzle (intended ship mode), 1 = fingers only (wrist
// keeps authored .s), 2 = wrist only, 3 = translation-only (no .s anywhere -
// pure skeleton compression).
std::atomic<int> g_scaleMode{0};
// Engine scale-restamp telemetry: BS2's engine never restamps the scale
// channel (pin-to-ref correct), Infinite's does (adopt correct). Counted at
// reference adoption: a scale-written bone whose bank value no longer matches
// what we last wrote was restamped by the engine.
std::atomic<uint32_t> g_scaleRestamps{0};
float g_lastWrittenS[kMaxBones][3];
bool g_scaleWrote[kMaxBones] = {};

// Which bones get the .s channel write in a given probe mode. Right hand:
// attach (43) and muzzle (44) are excluded in EVERY mode - their scale
// channel stays engine-owned (see the g_scale block comment).
bool scale_selects(int mode, int hand, int idx, int first) {
    if (mode == 3) return false;
    if (hand == 1 &&
        (idx == patterns::kBoneWeaponAttach || idx == patterns::kBoneRClusterLast))
        return false;
    if (mode == 1) return idx != first; // fingers only (wrist = cluster first)
    if (mode == 2) return idx == first; // wrist only
    return true;
}

// Session 61: uniform weapon scale - drives the HOLDABLE's own
// SkeletonInstance (BS2 session-41's shipped design, adapted; the weapon has
// carried its own skeleton here since s20, it is how the muzzle bone was
// found). Every bone: translation *= ws (uniform about the component origin,
// which IS the grip - R_Grip sits at (0,0,0)), quat adopted from the engine
// per frame (drum-spin/reload animations keep playing while scaled), scale
// channel = captured reference * ws, pinned exactly like the cluster scale.
// At ws == 1.0 the lane drops the skeleton entirely and restores the
// captured pose - zero interference at the default.
std::atomic<float> g_wScale{0.760f}; // s61 in-headset calibration (1.0 = authored)
void* g_wHoldable = nullptr; // the actor the lane is bound to
void* g_wSkelInst = nullptr;
Qts* g_wBones = nullptr;
int g_wBoneCount = 0;
Qts g_wRef[kMaxBones]; // captured pose (restored on drop)
Qts g_wAnim[kMaxBones]; // adopted engine p/q; scale rows pinned to g_wRef
Qts g_wWritten[kMaxBones]; // what we last wrote (adoption + reapply source)
bool g_wWrittenValid = false;
uint32_t g_wAdopts = 0;
uint32_t g_wDrives = 0;

std::atomic<bool> g_collapse{true}; // hide the driven arm's sleeve
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_reapplies{0};
std::atomic<int> g_lastHand{-1};
char g_status[160] = "idle";

// Render-lock (session 13): solve the anchor against the renderer's OWN
// foreground transform (captured per frame from the vm draws' cb0) so the
// rig lands on the world-correct pixel. See patterns.h "Foreground scene".
// DEFAULT OFF since session 21 part 2 - the user's in-headset verdict:
// "lock off is exactly what I was looking for, the aim is in tune with the
// model" - the lock's own correction WAS the +-90-flipping laser-vs-gun
// drift reported in session 20 (its model was calibrated against the old
// fg composition and miscorrects laterally at large hand yaws).
// `vrbones lock abs` remains the live A/B back to the old behavior.
std::atomic<int> g_renderLock{0};         // 0 off, 1 abs (true position), 2 diff
                                          // (head-split cancel only)
// Correction gains. Session 13 measured "gain 0.5 lands, 1.0 doubles" and
// blamed a rigid-path rebake; session 14 decomposed that 1.79x overshoot as
// (model depth-scale error 1.63, from the section-contaminated eye) x (true
// rebake ~1.1). With the model's depth scale now physically calibrated, the
// remaining rebake factor is ~1.1 on BOTH axes, so both gains default to
// ~1/1.1. Separate knobs kept - the axes are measured by different
// instruments (simhead sweeps vs size/parallax A/Bs).
std::atomic<float> g_lockGain{0.9f};      // lateral
std::atomic<float> g_lockDepthGain{0.9f}; // along the fg forward
// Fg-eye pull-back behind the camera at the MATCHED lens. The DRIVEN rigid
// path renders a much smaller pull than the vanilla path's fov-coupled ~65
// (session 16 flat calibration at option 117: +11.5 UU physical, agreed by
// offset-parallax and size A/Bs within ~1 UU, and close to the stock-lens
// 13 - the driven path's pull is NOT fov-coupled). The default is the knob
// value that lands 11.5 through the 0.9 depth gain (11.5/0.9 = 12.8); if
// lockdgain changes, the effective pull moves with it.
std::atomic<float> g_lockPull{12.8f};
std::atomic<float> g_lockDeltaMag{0.0f};  // telemetry: |delta| UU last frame
std::atomic<uint32_t> g_lockSolves{0};
std::atomic<uint32_t> g_lockSkips{0};

// In-headset telemetry (vrbones log on): ~5 Hz shared sample window. The
// headset cannot be watched from outside, so the log carries every frame of
// the chain - raw XR poses (camera.cpp / hands.cpp lines), the camera, the
// actor, the world target, and what the bone array held before the write.
std::atomic<bool> g_telemetry{false};
uint64_t g_lastTlmMs = 0;
bool g_tlmWindowOpen = false;

// ---- guarded memory (no C++ objects inside SEH frames) ----------------------

bool read_n(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_n(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

// actor -> SkeletonInstance, fully revalidated (vtable, plausible count,
// readable array). The instance is recreated on mesh relinks, so never trust
// yesterday's pointer.
//
// Session 20: the resolution itself is BY VALUE and slot-free (`resolve_skel`)
// so any actor's skeleton can be inspected - the weapon carries its own, which
// is what the muzzle probe needs. `locate()` keeps the module's single cached
// slot for the drive, which only ever poses the AHands rig.
struct Skel {
    void* inst = nullptr;
    Qts* bones = nullptr;
    int count = 0;
};

bool resolve_skel(void* actor, Skel& out) {
    out = {};
    if (!actor) return false;
    void* si = nullptr;
    if (!read_n(static_cast<uint8_t*>(actor) + patterns::kActorSkelInstOffset, &si, sizeof si) ||
        !si)
        return false;
    void* vtbl = nullptr;
    if (!read_n(si, &vtbl, sizeof vtbl) || to_rva(vtbl) != patterns::kSkeletonInstanceVtableRva)
        return false;
    struct {
        Qts* bones;
        int count;
    } a{};
    if (!read_n(static_cast<uint8_t*>(si) + patterns::kSkelInstBonesOffset, &a, sizeof a) ||
        !a.bones || a.count < 1 || a.count > kMaxBones)
        return false;
    // The count bound alone is not enough (session 27): the drive memcpys up to
    // count * sizeof(Qts) bytes THROUGH a.bones, so a plausible count paired
    // with an array pointer that does not actually have that many readable
    // bytes behind it is a multi-kilobyte write into whatever follows. The SEH
    // guard on write_n turns that into a survivable fault, not a correct one -
    // so require the whole span to be committed before trusting the pair.
    if (!bvr::pattern_scan::is_memory_valid(a.bones, static_cast<size_t>(a.count) * sizeof(Qts)))
        return false;
    out.inst = si;
    out.bones = a.bones;
    out.count = a.count;
    return true;
}

// Bone index -> name, via the SharedSkeletonData FName->index map walked in
// reverse (patterns.h "Session 20: bone NAMES"). Returns how many names were
// filled; entries stay null when a bone has no map entry.
int resolve_bone_names(const Skel& sk, const wchar_t** names, int cap) {
    for (int i = 0; i < cap; ++i) names[i] = nullptr;
    if (!sk.inst) return 0;
    void* shared = nullptr;
    if (!read_n(static_cast<uint8_t*>(sk.inst) + patterns::kSkelInstSharedOffset, &shared,
                sizeof shared) ||
        !shared)
        return 0;
    const uint8_t* map = static_cast<const uint8_t*>(shared) + patterns::kSharedBoneNameMapOffset;
    struct {
        const uint8_t* pairs;
    } p{};
    const int32_t* buckets = nullptr;
    int32_t bucketCount = 0;
    if (!read_n(map + patterns::kNameMapPairsOffset, &p, sizeof p) ||
        !read_n(map + patterns::kNameMapBucketsOffset, &buckets, sizeof buckets) ||
        !read_n(map + patterns::kNameMapBucketCountOffset, &bucketCount, sizeof bucketCount))
        return 0;
    if (!p.pairs || !buckets || bucketCount <= 0 || bucketCount > 65536) return 0;

    int filled = 0;
    for (int b = 0; b < bucketCount; ++b) {
        int32_t idx = -1;
        if (!read_n(buckets + b, &idx, sizeof idx)) break;
        // Chain walk, bounded by the table size so a corrupt link cannot spin.
        for (int guard = 0; idx >= 0 && guard <= cap * 4; ++guard) {
            struct {
                int32_t next, nameIdx, nameNum, value;
            } pair{};
            if (!read_n(p.pairs + static_cast<size_t>(idx) * patterns::kNameMapPairStride, &pair,
                        sizeof pair))
                break;
            if (pair.value >= 0 && pair.value < cap && !names[pair.value]) {
                const wchar_t* t = patterns::fname_text(pair.nameIdx);
                if (t) {
                    names[pair.value] = t;
                    ++filled;
                }
            }
            idx = pair.next;
        }
    }
    return filled;
}

bool locate(void* handsActor) {
    Skel sk{};
    if (!resolve_skel(handsActor, sk) || sk.count < 8) {
        g_skelInst = nullptr;
        return false;
    }
    if (sk.inst != g_skelInst || sk.bones != g_bones || sk.count != g_boneCount) {
        BVR_LOG("[bones] skeleton: inst=%p bones=%p count=%d%s", sk.inst,
                static_cast<void*>(sk.bones), sk.count,
                sk.count == patterns::kHandsRigBoneCount ? "" : " (UNEXPECTED count)");
        g_refValid = false; // new array = new reference
        g_hasWritten[0] = g_hasWritten[1] = false;
    }
    g_skelInst = sk.inst;
    g_bones = sk.bones;
    g_boneCount = sk.count;
    return true;
}

void set_dirty(uint8_t v) {
    if (!g_skelInst) return;
    write_n(static_cast<uint8_t*>(g_skelInst) + patterns::kSkelInstDirtyOffset, &v, 1);
}

// ---- quat helpers over the Qts layout ---------------------------------------

void qts_rotate(const float q[4], const float v[3], float out[3]) {
    quat_rotate(q[0], q[1], q[2], q[3], v, out);
}

// ---- render lock -------------------------------------------------------------

// Build the foreground-view model matrix (rows x, y, w of [R | -R*E], scaled
// by the fixed projection) for a given component-frame rotation quat and eye
// position. The eye is a PARAMETER because it rides the camera (session 13
// part 3): eye = camera position in component space + the measured pull-back
// expressed in the fg view's own frame.
void build_fg_model(const float qd[4], const float E[3], float invTanH, float invTanV,
                    float M[12]) {
    float fgF[3], fgR[3], fgU[3];
    static const float kX[3] = {1.0f, 0.0f, 0.0f}, kY[3] = {0.0f, 1.0f, 0.0f},
                       kZ[3] = {0.0f, 0.0f, 1.0f};
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kX, fgF);
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kY, fgR);
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kZ, fgU);
    M[0] = invTanH * fgR[0];
    M[1] = invTanH * fgR[1];
    M[2] = invTanH * fgR[2];
    M[3] = -(M[0] * E[0] + M[1] * E[1] + M[2] * E[2]);
    M[4] = invTanV * fgU[0];
    M[5] = invTanV * fgU[1];
    M[6] = invTanV * fgU[2];
    M[7] = -(M[4] * E[0] + M[5] * E[1] + M[6] * E[2]);
    M[8] = fgF[0];
    M[9] = fgF[1];
    M[10] = fgF[2];
    M[11] = -(M[8] * E[0] + M[9] * E[1] + M[10] * E[2]);
}

// Natural foreground depth of a component point through the model (row w).
float fg_natural_w(const float M[12], const float ptc[3]) {
    return M[8] * ptc[0] + M[9] * ptc[1] + M[10] * ptc[2] + M[11];
}

// Solve M (rows x, y, w) for the component point that renders at (ndcX, ndcY)
// with foreground depth w. The caller picks w: the world-equivalent k*d for
// the depth-corrected solves, or the natural depth for the diff-mode anchor.
bool solve_fg(const float M[12], float ndcX, float ndcY, float w, float outP[3]) {
    if (w < 1.0f) return false;
    float A[3][3] = {{M[0], M[1], M[2]}, {M[4], M[5], M[6]}, {M[8], M[9], M[10]}};
    float b[3] = {ndcX * w - M[3], ndcY * w - M[7], w - M[11]};
    float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (det > -1e-4f && det < 1e-4f) return false;
    float inv = 1.0f / det;
    outP[0] = inv * (b[0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                     A[0][1] * (b[1] * A[2][2] - A[1][2] * b[2]) +
                     A[0][2] * (b[1] * A[2][1] - A[1][1] * b[2]));
    outP[1] = inv * (A[0][0] * (b[1] * A[2][2] - A[1][2] * b[2]) -
                     b[0] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                     A[0][2] * (A[1][0] * b[2] - b[1] * A[2][0]));
    outP[2] = inv * (A[0][0] * (A[1][1] * b[2] - b[1] * A[2][1]) -
                     A[0][1] * (A[1][0] * b[2] - b[1] * A[2][0]) +
                     b[0] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]));
    return true;
}

// NDC of the world target through a pinhole at the camera position with the
// given rotation and the world option FOV. outDf = the target's forward
// distance from the camera - the "true distance" the depth constraint uses.
// Session 28: h/w of the live backbuffer, defaulting to 16:9. The world lens is
// tanV = tanH*(h/w) (dump-measured, ENGINE_NOTES "Session 28"), so every model
// of it here has to read the real aspect. It was hardcoded 9/16, which is right
// only at 16:9 - and the resolutions people actually run in VR are square-ish.
float live_inv_aspect() {
    unsigned w = 0, h = 0;
    if (bvr::hud::backbuffer_dims(&w, &h) && w && h)
        return static_cast<float>(h) / static_cast<float>(w);
    return 9.0f / 16.0f;
}

bool world_ndc(const FrameContext& ctx, const GamePose& gp, const FRotator& rot, float tanH,
               float* outX, float* outY, float* outDf) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(rot, fwd, right, up);
    float d[3] = {gp.loc.x - ctx.camX, gp.loc.y - ctx.camY, gp.loc.z - ctx.camZ};
    float df = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
    if (df < 4.0f) return false; // target at/behind the eye: no stable pixel
    float dr = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float du = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    float tanV = tanH * live_inv_aspect(); // world lens: vertical follows the window
    *outX = dr / (df * tanH);
    *outY = du / (df * tanV);
    *outDf = df;
    return true;
}

// Compute the component-space nudge that counters the foreground pipeline's
// camera-coupled displacement of the rig. The pipeline (fixed 60-deg 4:3
// projection, eye pulled ~32 UU behind the RENDER CAMERA - it rides the
// camera, translation included; session 13 part 3 - plus hand sway) is
// self-consistent at view center but slides the rig laterally under head-
// split AND pins its stereo depth at ~(d+32)/k. The transform is MODELED
// analytically (patterns.h kFgEyeComp block) - live captures embed the
// engine's per-frame re-derivations of section transforms from the very
// bones this module writes (feedback), so they cannot be used.
// Depth constraint (session 14): the rig must RENDER at fg depth k*df,
// k = tan(worldFov/2)/tan(fgFov/2) - apparent size, stereo disparity, and
// translation parallax are all the same (1/w)*k geometry, so that one
// constraint makes all three world-correct at once. It renders today at
// df + kFgEyeFwdBehindCam (parallax-calibrated), so the solve pushes the
// anchor deeper by the DIFFERENCE, expressed against the model's own
// natural depth (the model's absolute depth scale is section-relative and
// cannot be trusted; its lateral geometry is g5-validated and kept).
// Modes: abs = solve the anchor onto its TRUE world pixel at w* (fixes the
// raised/too-close authored composition as well; inherits the sway wobble).
// diff = subtract the zero-split solve taken at ptc's NATURAL depth, so the
// delta carries the full depth correction plus only the head-split lateral
// cancel (authored lateral composition preserved).
// The correction returns SPLIT into a lateral part and a depth part (along
// the fg forward) because the rigid-section rebake gain was measured on the
// lateral axis only - the depth axis gets its own gain (vrbones lockdgain).
// qaInv/actorRot/actorLoc come from the drive (already computed there).
bool render_lock_delta(const FrameContext& ctx, const GamePose& gp, const float qaInv[4],
                       const FRotator& actorRot, const float actorLoc[3], const float ptc[3],
                       float outLat[3], float outDepth[3]) {
    int mode = g_renderLock.load(std::memory_order_relaxed);
    int32_t* opt = patterns::hfov_option_ptr();
    float hfov = opt && *opt > 0 ? static_cast<float>(*opt) : 0.0f;
    if (hfov <= 0.0f) return false;
    float tanH = tanf(hfov * 0.5f / kRadToDeg);

    FRotator camRot{ctx.camPitch, ctx.camYaw, ctx.camRoll};
    float ndcX, ndcY, df;
    if (!world_ndc(ctx, gp, camRot, tanH, &ndcX, &ndcY, &df)) return false;

    // Fg lens: with the session-15 lens match armed (vrfgfov, default on)
    // the rig renders through the WORLD lens - invTan scales come from the
    // live world tanH and the lens ratio k collapses to 1, so the depth
    // constraint w* = k*df becomes simply the true distance. Without the
    // match, the legacy 60-deg constants apply (kept for A/B).
    // Session 28: the vertical model reads the LIVE aspect. This comment's claim
    // that "the rig renders through the WORLD lens, so k collapses to 1" was
    // TRUE only at 16:9 - the shipped match constant (0.75) left the fg lens
    // 1.7778/aspect narrower than the world everywhere else, so at a square
    // backbuffer k was really 1.7778 while this code assumed 1. That mis-scaled
    // the depth constraint AND the head-split lateral cancel, which is what
    // "the hand and gun move when the headset moves" looks like. With the
    // aspect-correct fg write in camera.cpp the lenses genuinely match and k
    // genuinely collapses to 1 - the assumption is now earned, not asserted.
    bool matched = camera::fg_fov_match_active();
    float invTanHFg = matched ? 1.0f / tanH : patterns::kFgInvTanH;
    float invTanVFg =
        matched ? 1.0f / (tanH * live_inv_aspect()) : patterns::kFgInvTanV;
    float k = tanH * invTanHFg;
    float wStar = k * df;
    if (wStar < 4.0f) return false; // hand at/behind the face: no stable solve

    float qcam[4], qdRaw[4], qd[4];
    ue_rot_to_quat(camRot, qcam);
    quat_mul(qaInv, qcam, qdRaw);
    // Composition bias (patterns.h): the fg view sits a hair up-right of the
    // camera delta; constant, measured as the mean of the dump set.
    static float s_qBias[4] = {2.0f, 0.0f, 0.0f, 0.0f}; // sentinel: build once
    if (s_qBias[0] > 1.5f) {
        FRotator biasRot{
            static_cast<int32_t>(patterns::kFgViewPitchBiasDeg * kRotUnitsPerDegree),
            static_cast<int32_t>(patterns::kFgViewYawBiasDeg * kRotUnitsPerDegree), 0};
        ue_rot_to_quat(biasRot, s_qBias);
    }
    quat_mul(qdRaw, s_qBias, qd);

    // Effective fg eye: camera position in component space + the TRUE
    // pull-back (forward from the parallax calibration - kFgEyeFwdBehindCam;
    // laterals from the dump mean, statics the abs solve absorbs) rotated
    // into the fg view's frame. The eye RIDES THE CAMERA - dump-proven
    // (offset 0 30 0 moved the recovered eye 29.7 UU).
    float dCam[3] = {ctx.camX - actorLoc[0], ctx.camY - actorLoc[1], ctx.camZ - actorLoc[2]};
    float camComp[3], ePulled[3], eyeEff[3];
    qts_rotate(qaInv, dCam, camComp);
    float pull = matched ? g_lockPull.load(std::memory_order_relaxed)
                         : patterns::kFgEyeFwdBehindCam;
    const float eTrue[3] = {-pull, patterns::kFgEyeComp[1], patterns::kFgEyeComp[2]};
    // Pull frame (session-16 simhead discriminator): at the matched lens the
    // renderer's eye offset does NOT swing with the camera-vs-actor head
    // split - the gun over-shifted the world by exactly pull*sin(split)*gain
    // on both axes when eTrue rode qd - so it rotates by the constant view
    // bias only (identical at zero split, where the A/Bs calibrated it). The
    // unmatched session-14 path keeps the qd rotation it was verified with.
    if (matched)
        quat_rotate(s_qBias[0], s_qBias[1], s_qBias[2], s_qBias[3], eTrue, ePulled);
    else
        quat_rotate(qd[0], qd[1], qd[2], qd[3], eTrue, ePulled);
    eyeEff[0] = camComp[0] + ePulled[0];
    eyeEff[1] = camComp[1] + ePulled[1];
    eyeEff[2] = camComp[2] + ePulled[2];

    float M1[12];
    build_fg_model(qd, eyeEff, invTanHFg, invTanVFg, M1);
    float wNat = fg_natural_w(M1, ptc);
    float p1[3];
    if (!solve_fg(M1, ndcX, ndcY, wStar, p1)) return false;

    float delta[3];
    if (mode == 2) {
        // Differential: subtract the zero-split solution AT NATURAL DEPTH so
        // the lateral part vanishes when the camera sits on the actor while
        // the depth correction survives the subtraction.
        float ndcX0, ndcY0, df0;
        if (!world_ndc(ctx, gp, actorRot, tanH, &ndcX0, &ndcY0, &df0)) return false;
        float e0[3];
        quat_rotate(s_qBias[0], s_qBias[1], s_qBias[2], s_qBias[3], eTrue, e0);
        float M0[12], p0[3];
        build_fg_model(s_qBias, e0, invTanHFg, invTanVFg, M0); // bias kept at zero split
        if (!solve_fg(M0, ndcX0, ndcY0, fg_natural_w(M0, ptc), p0)) return false;
        delta[0] = p1[0] - p0[0];
        delta[1] = p1[1] - p0[1];
        delta[2] = p1[2] - p0[2];
    } else {
        delta[0] = p1[0] - ptc[0];
        delta[1] = p1[1] - ptc[1];
        delta[2] = p1[2] - ptc[2];
    }

    // Split along the fg forward (M row w is the unit forward vector).
    float dDepth = delta[0] * M1[8] + delta[1] * M1[9] + delta[2] * M1[10];
    outDepth[0] = dDepth * M1[8];
    outDepth[1] = dDepth * M1[9];
    outDepth[2] = dDepth * M1[10];
    outLat[0] = delta[0] - outDepth[0];
    outLat[1] = delta[1] - outDepth[1];
    outLat[2] = delta[2] - outDepth[2];
    float latMag = sqrtf(outLat[0] * outLat[0] + outLat[1] * outLat[1] + outLat[2] * outLat[2]);

    if (g_tlmWindowOpen) {
        BVR_LOG("[tlm] lock mode=%d tgt=(%.3f %.3f) df=%.1f k=%.2f wNat=%.1f w*=%.1f "
                "lat=%.2f depth=%+.2f",
                mode, ndcX, ndcY, df, k, wNat, wStar, latMag, dDepth);
    }
    // Per-axis refusal: the depth correction is LARGE by design (~25-70 UU),
    // the lateral one is not - a big lateral delta still means a model bug.
    if (latMag > 30.0f || dDepth < -120.0f || dDepth > 120.0f) {
        static uint64_t lastDump = 0;
        uint64_t now = GetTickCount64();
        if (now - lastDump > 2000) {
            lastDump = now;
            BVR_LOG("[bones] lock: refusing outsized delta (lat %.1f depth %+.1f)", latMag,
                    dDepth);
        }
        return false;
    }
    g_lockDeltaMag.store(
        sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]),
        std::memory_order_relaxed);
    return true;
}

// ---- the drive ---------------------------------------------------------------

void cluster_of(int hand, int* first, int* last, int* anchor) {
    if (hand == 1) {
        *first = patterns::kBoneRClusterFirst;
        *last = patterns::kBoneRClusterLast;
        int ov = g_rAnchorOverride.load(std::memory_order_relaxed);
        *anchor = ov >= 0 ? ov : patterns::kBoneWeaponAttach;
    } else {
        *first = g_lFirst.load(std::memory_order_relaxed);
        *last = g_lLast.load(std::memory_order_relaxed);
        *anchor = g_lAnchor.load(std::memory_order_relaxed);
    }
}

// Restore a hidden hand's cluster + sleeve from the reference pose. g_ref is
// a safe source: an engine re-evaluation rewrites the whole array (scales
// included) and triggers the reference refresh in drive(), so the reference
// can never hold our zeroed scales.
void restore_hidden(int hand) {
    if (hand < 0 || !g_bones || !g_refValid) return;
    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    for (int i = first; i <= last && i < g_boneCount; ++i) {
        if (i < 0) continue;
        write_n(g_bones[i].p, g_ref[i].p, 12);
        write_n(g_bones[i].q, g_ref[i].q, 16);
        write_n(g_bones[i].s, g_ref[i].s, 12);
        g_scaleWrote[i] = false; // the bank holds the authored scale again
    }
    const int* sleeve = hand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
    const size_t sleeveCount = hand == 1 ? _countof(patterns::kBoneRSleeve)
                                         : _countof(patterns::kBoneLSleeve);
    for (size_t k = 0; k < sleeveCount; ++k) {
        int idx = sleeve[k];
        if (idx >= g_boneCount) continue;
        write_n(g_bones[idx].p, g_ref[idx].p, 12);
        write_n(g_bones[idx].s, g_ref[idx].s, 12);
    }
}

// ---- weapon-skeleton scale lane (session 61) --------------------------------

void wskel_set_dirty(uint8_t v) {
    if (!g_wSkelInst) return;
    write_n(static_cast<uint8_t*>(g_wSkelInst) + patterns::kSkelInstDirtyOffset, &v, 1);
}

// The bound skeleton, revalidated by value - never write through yesterday's
// pointers (the same session-29 lesson release() carries for the rig).
bool wskel_intact() {
    if (!g_wHoldable || !g_wSkelInst || !g_wBones) return false;
    Skel sk{};
    if (!resolve_skel(g_wHoldable, sk)) return false;
    return sk.inst == g_wSkelInst && sk.bones == g_wBones && sk.count == g_wBoneCount;
}

// Hand the weapon skeleton back: restore the captured pose over every bone
// (the engine does not restamp the scale channel, so merely not driving
// would leave the gun scaled for good - the sleeve-collapse lesson), then
// set its dirty flag so the engine rebuilds from its own animation.
void wskel_drop(const char* why) {
    if (wskel_intact()) {
        for (int i = 0; i < g_wBoneCount; ++i) {
            write_n(g_wBones[i].p, g_wRef[i].p, 12);
            write_n(g_wBones[i].q, g_wRef[i].q, 16);
            write_n(g_wBones[i].s, g_wRef[i].s, 12);
        }
        wskel_set_dirty(1);
        BVR_LOG("[bones] wskel: released to the engine (%s) - authored pose restored, "
                "%u drives %u adopts",
                why ? why : "?", g_wDrives, g_wAdopts);
    }
    g_wHoldable = nullptr;
    g_wSkelInst = nullptr;
    g_wBones = nullptr;
    g_wBoneCount = 0;
    g_wWrittenValid = false;
    g_wAdopts = 0;
    g_wDrives = 0;
}

// Bind (or re-bind) the lane to the current holdable. The holdable read is
// deliberately CLASS-AGNOSTIC (hands::current_holdable) - the MachineGun and
// GrenadeLauncher carry a different vtable and a gated read pins a stale
// weapon (the session-21 part-3 defect; BS2 relearned it independently).
bool wskel_resolve() {
    // A holdable without a resolvable skeleton (the WRENCH: +0x3FC is null -
    // rigid melee mesh, flat-proven 2026-08-14) would otherwise fail here
    // every frame: negative-cache it per holdable with a 1 s retry, so a
    // TRANSIENT mid-equip failure on a skeletal weapon still self-heals,
    // and log the verdict once per holdable instead of forever.
    static void* s_failedHold = nullptr;
    static uint64_t s_nextRetryMs = 0;
    void* hold = nullptr;
    if (!hands::current_holdable(&hold) || !hold) {
        if (g_wHoldable) wskel_drop("holdable gone");
        return false;
    }
    if (hold == g_wHoldable && wskel_intact()) return true;
    if (g_wHoldable) wskel_drop("holdable changed");
    if (hold == s_failedHold && GetTickCount64() < s_nextRetryMs) return false;
    Skel sk{};
    if (!resolve_skel(hold, sk)) {
        if (hold != s_failedHold)
            BVR_LOG("[bones] wskel: holdable %p has no resolvable SkeletonInstance "
                    "(+0x3FC) - no bones to scale (rigid mesh?), lane stays unbound",
                    hold);
        s_failedHold = hold;
        s_nextRetryMs = GetTickCount64() + 1000;
        return false;
    }
    s_failedHold = nullptr;
    Qts bank[kMaxBones];
    if (!read_n(sk.bones, bank, sizeof(Qts) * static_cast<size_t>(sk.count))) return false;
    g_wHoldable = hold;
    g_wSkelInst = sk.inst;
    g_wBones = sk.bones;
    g_wBoneCount = sk.count;
    memcpy(g_wRef, bank, sizeof(Qts) * static_cast<size_t>(sk.count));
    memcpy(g_wAnim, bank, sizeof(Qts) * static_cast<size_t>(sk.count));
    g_wWrittenValid = false;
    g_wAdopts = 0;
    g_wDrives = 0;
    BVR_LOG("[bones] wskel: bound holdable=%p inst=%p count=%d (reference captured)", hold,
            sk.inst, sk.count);
    return true;
}

// Per-frame compose: adopt the engine's p/q where it wrote since our last
// write (animations keep playing), then write p*ws / q verbatim / s=ref*ws.
// The scale rows are NEVER adopted - the engine does not restamp scale, so
// adopting would feed our own write back as refS * ws^n (the same structural
// rule as the cluster's pinned reference).
bool wskel_compose(float ws) {
    for (int i = 0; i < g_wBoneCount; ++i) {
        Qts cur{};
        if (!read_n(&g_wBones[i], &cur, sizeof cur)) {
            g_wSkelInst = nullptr; // faulted: rebind next frame
            return false;
        }
        bool engineWrote = !g_wWrittenValid || memcmp(cur.p, g_wWritten[i].p, 12) != 0 ||
                           memcmp(cur.q, g_wWritten[i].q, 16) != 0;
        if (engineWrote) {
            memcpy(g_wAnim[i].p, cur.p, 12);
            memcpy(g_wAnim[i].q, cur.q, 16);
            ++g_wAdopts;
        }
        float p[3] = {g_wAnim[i].p[0] * ws, g_wAnim[i].p[1] * ws, g_wAnim[i].p[2] * ws};
        float sv[3] = {g_wRef[i].s[0] * ws, g_wRef[i].s[1] * ws, g_wRef[i].s[2] * ws};
        if (!write_n(g_wBones[i].p, p, 12) || !write_n(g_wBones[i].q, g_wAnim[i].q, 16) ||
            !write_n(g_wBones[i].s, sv, 12)) {
            g_wSkelInst = nullptr;
            return false;
        }
        memcpy(g_wWritten[i].p, p, 12);
        memcpy(g_wWritten[i].q, g_wAnim[i].q, 16);
        memcpy(g_wWritten[i].s, sv, 12);
    }
    g_wWrittenValid = true;
    ++g_wDrives;
    wskel_set_dirty(0); // render-side evaluate-if-dirty must not rebuild over us
    return true;
}

// ---- Translate lane: any actor's mesh offset from its origin ---------------
// One slot (the EVE syringe). Same protocol as the weapon-scale lane: adopt
// what the engine wrote since our last write (so its own animation keeps
// playing), write that plus the offset, clear the evaluate-if-dirty flag.
struct ShiftLane {
    void* actor = nullptr;
    void* inst = nullptr;
    Qts* bones = nullptr;
    int count = 0;
    Qts anim[kMaxBones];
    Qts written[kMaxBones];
    bool writtenValid = false;
};
ShiftLane g_sh;

bool shift_intact() {
    if (!g_sh.actor || !g_sh.inst) return false;
    Skel sk{};
    return resolve_skel(g_sh.actor, sk) && sk.inst == g_sh.inst && sk.bones == g_sh.bones &&
           sk.count == g_sh.count;
}

} // namespace

bool skel_shift(void* actor, const float compDelta[3]) {
    if (!actor) return false;
    if (actor != g_sh.actor || !shift_intact()) {
        if (g_sh.actor && actor != g_sh.actor) skel_shift_release();
        Skel sk{};
        if (!resolve_skel(actor, sk)) {
            g_sh = ShiftLane{};
            return false;
        }
        g_sh.actor = actor;
        g_sh.inst = sk.inst;
        g_sh.bones = sk.bones;
        g_sh.count = sk.count;
        g_sh.writtenValid = false;
    }
    for (int i = 0; i < g_sh.count; ++i) {
        Qts cur{};
        if (!read_n(&g_sh.bones[i], &cur, sizeof cur)) {
            g_sh = ShiftLane{};
            return false;
        }
        const bool engineWrote = !g_sh.writtenValid || memcmp(cur.p, g_sh.written[i].p, 12) != 0 ||
                                 memcmp(cur.q, g_sh.written[i].q, 16) != 0;
        if (engineWrote) g_sh.anim[i] = cur;
        const float pp[3] = {g_sh.anim[i].p[0] + compDelta[0], g_sh.anim[i].p[1] + compDelta[1],
                             g_sh.anim[i].p[2] + compDelta[2]};
        if (!write_n(g_sh.bones[i].p, pp, 12)) {
            g_sh = ShiftLane{};
            return false;
        }
        memcpy(g_sh.written[i].p, pp, 12);
        memcpy(g_sh.written[i].q, g_sh.anim[i].q, 16);
    }
    g_sh.writtenValid = true;
    const uint8_t clean = 0;
    write_n(static_cast<uint8_t*>(g_sh.inst) + patterns::kSkelInstDirtyOffset, &clean, 1);
    return true;
}

void skel_shift_release() {
    if (shift_intact() && g_sh.writtenValid) {
        for (int i = 0; i < g_sh.count; ++i) write_n(g_sh.bones[i].p, g_sh.anim[i].p, 12);
        const uint8_t dirty = 1; // let the engine rebuild from its own pose
        write_n(static_cast<uint8_t*>(g_sh.inst) + patterns::kSkelInstDirtyOffset, &dirty, 1);
    }
    g_sh = ShiftLane{};
}

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    BVR_LOG("[bones] init (SkeletonInstance vtable 0x%X, right cluster %d-%d anchor %d)",
            patterns::kSkeletonInstanceVtableRva, patterns::kBoneRClusterFirst,
            patterns::kBoneRClusterLast, patterns::kBoneWeaponAttach);
}

void on_world_change() {
    g_offTrack = false;
    g_palm[0].valid = g_palm[1].valid = false;
    g_lastActorValid = false;
    g_barrelLocalValid = false;
    g_placeRestValid = false;
    if (g_skelInst) BVR_LOG("[bones] world changed - skeleton cache cleared");
    g_skelInst = nullptr;
    g_bones = nullptr;
    g_boneCount = 0;
    g_refValid = false;
    g_hasWritten[0] = g_hasWritten[1] = false;
    g_cacheSkelInst = nullptr;
    g_cacheMs = 0;
    g_hiddenHand = -1; // the collapsed bones died with the old world
    g_cacheHiddenCount = 0;
    // Session 29: the sleeve latch dies with the world too. It was hoisted out
    // of drive() so release() could reach it, which also means it now has to be
    // cleared here - a stale `true` would make release() write a dead world's
    // sleeve bones back from a dead world's reference.
    g_wasCollapsed = false;
    g_collapsedHand = -1;
    memset(g_scaleWrote, 0, sizeof g_scaleWrote); // scale writes died with it
    // The weapon skeleton died with the world too - drop WITHOUT writing
    // (wskel_intact re-resolves through the dead actor and fails, so
    // wskel_drop degrades to a pointer clear, which is exactly right here).
    wskel_drop("world change");
    g_sh = ShiftLane{}; // its skeleton died with the world: forget, never write
}

void release(const char* why) {
    g_offTrack = false;
    // NEVER write without a live skeleton. on_world_change() nulls these the
    // moment a level swap is seen, so this is the interlock that makes the
    // call safe from any site: without it, a release on the world-change frame
    // writes ~1.8 KB through the PREVIOUS level's bone array. write_n is
    // SEH-guarded, so that does not fault - it silently corrupts whatever the
    // engine has since allocated over those pages (session 29: a save load
    // hung the game exactly this way).
    if (!g_skelInst || !g_bones || g_boneCount <= 0) {
        g_hiddenHand = -1;
        g_wasCollapsed = false;
        g_collapsedHand = -1;
        g_cacheSkelInst = nullptr;
        g_cacheMs = 0;
        g_cacheCount = g_cacheSleeveCount = g_cacheHiddenCount = 0;
        g_refValid = false;
        g_hasWritten[0] = g_hasWritten[1] = false;
        memset(g_scaleWrote, 0, sizeof g_scaleWrote); // the bank died with the world
        wskel_drop("rig released (no rig skeleton)"); // intact-gated, safe here
        return;
    }

    // Nothing driven means nothing to hand back. Cheap and idempotent, so the
    // edge detector that calls this can fire freely. The weapon lane is
    // dropped FIRST - it can be live while the rig cache is already clean.
    wskel_drop("rig released");
    if (g_hiddenHand < 0 && !g_wasCollapsed && !g_cacheSkelInst && !g_refValid) return;

    // ORDER MATTERS: both restores read g_ref, so g_refValid must still be
    // true here. Clearing it first would silently turn restore_hidden() into a
    // no-op and leave the hand collapsed - the exact failure this fixes.
    int hidden = g_hiddenHand;
    if (hidden >= 0) restore_hidden(hidden);
    // Scale writes persist in the bank across engine evaluations (the engine
    // does not restamp the scale channel - the same fact the sleeve restore
    // rests on), so a scaled cluster must be handed back explicitly too.
    if (g_refValid) {
        for (int i = 0; i < g_boneCount; ++i) {
            if (!g_scaleWrote[i]) continue;
            write_n(g_bones[i].s, g_ref[i].s, 12);
            g_scaleWrote[i] = false;
        }
    } else {
        memset(g_scaleWrote, 0, sizeof g_scaleWrote);
    }
    if (g_wasCollapsed && g_collapsedHand >= 0 && g_bones && g_refValid) {
        const int* sleeve =
            g_collapsedHand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
        const size_t sleeveCount = g_collapsedHand == 1 ? _countof(patterns::kBoneRSleeve)
                                                        : _countof(patterns::kBoneLSleeve);
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, g_ref[idx].p, 12);
            write_n(g_bones[idx].s, g_ref[idx].s, 12);
        }
    }
    g_hiddenHand = -1;
    g_wasCollapsed = false;
    g_collapsedHand = -1;

    // Stop reapply() dead. Its only brakes are the instance check and a 100 ms
    // cache age, so without this it keeps repainting - and re-clearing the
    // dirty flag - for ~6 frames into the cutscene.
    g_cacheSkelInst = nullptr;
    g_cacheMs = 0;
    g_cacheCount = 0;
    g_cacheSleeveCount = 0;
    g_cacheHiddenCount = 0;

    // Hand the skeleton back actively rather than waiting for the engine to
    // notice, then drop the frozen reference so the next drive re-adopts a
    // FRESH engine pose (same reasoning as set_sway_kill(false)) - otherwise
    // the first post-cutscene frame rebuilds the rig, and the muzzle axis the
    // laser and aim dot ride, from a pre-cutscene pose.
    if (g_skelInst) set_dirty(1);
    g_refValid = false;
    g_hasWritten[0] = g_hasWritten[1] = false;
    g_lastBigDeltaMs = 0;

    BVR_LOG("[bones] released to the engine (%s): hidden hand %d restored, reapply cache "
            "cleared, dirty flag handed back", why ? why : "?", hidden);
}

void debug_state(int* hiddenHand, unsigned long long* cacheAgeMs, bool* refValid) {
    if (hiddenHand) *hiddenHand = g_hiddenHand;
    if (cacheAgeMs)
        *cacheAgeMs = g_cacheMs ? static_cast<unsigned long long>(GetTickCount64() - g_cacheMs)
                                : 0ULL;
    if (refValid) *refValid = g_refValid;
}

void set_sway_kill(bool on) {
    bool was = g_swayKill.exchange(on, std::memory_order_relaxed);
    if (was && !on) g_refValid = false; // release the frozen pose immediately
    BVR_LOG("[bones] idle-sway kill %s (%s)", on ? "ON" : "off",
            on ? "reference frozen against idle breathing; real animations pass the "
                 "threshold and re-freeze when settled"
               : "reference tracks every engine evaluation - sway visible again");
}

bool sway_kill() {
    return g_swayKill.load(std::memory_order_relaxed);
}

void set_scale(int hand, float s) {
    if (s < 0.05f) s = 0.05f;
    if (s > 20.0f) s = 20.0f;
    if (hand != 1) g_scale[0].store(s, std::memory_order_relaxed);
    if (hand != 0) g_scale[1].store(s, std::memory_order_relaxed);
    // No bank write here: the per-frame drive applies it (and hands the
    // authored scale back on the 1.0 off edge) - commands may run when no
    // skeleton is live.
}

float scale(int hand) {
    return g_scale[hand == 1 ? 1 : 0].load(std::memory_order_relaxed);
}

void set_weapon_scale(float ws) {
    if (ws < 0.05f) ws = 0.05f;
    if (ws > 20.0f) ws = 20.0f;
    g_wScale.store(ws, std::memory_order_relaxed);
}

float weapon_scale() {
    return g_wScale.load(std::memory_order_relaxed);
}

void wskel_drive() {
    float ws = g_wScale.load(std::memory_order_relaxed);
    if (ws == 1.0f) {
        // The default is a total drop - no adoption, no writes, no cached
        // pointers. The lane only ever exists while the knob is off 1.0.
        if (g_wHoldable) wskel_drop("scale back to 1.0");
        return;
    }
    if (!wskel_resolve()) return;
    wskel_compose(ws);
}

void wskel_release(const char* why) {
    if (g_wHoldable) wskel_drop(why);
}

void set_hide_inactive(bool on) {
    bool was = g_hideInactive.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("[bones] hideinactive %s (inactive hand %s)", on ? "ON" : "off",
                on ? "collapses while the other drives"
                   : "restores on the next driven frame");
}

bool hide_inactive() { return g_hideInactive.load(std::memory_order_relaxed); }

bool telemetry_on() {
    return g_telemetry.load(std::memory_order_relaxed);
}

float lock_delta_mag() {
    return g_lockDeltaMag.load(std::memory_order_relaxed);
}

void* skeleton_instance() { return g_skelInst; }

void* bone_array(int* count) {
    if (count) *count = g_bones ? g_boneCount : 0;
    return g_bones;
}

bool muzzle_ref_offset(float out[3]) {
    // Same derivation as barrel_ref_axis below: drive() places the cluster as
    // a rigid rotation of the reference about the anchor, so the rendered
    // world offset of bone 44 is q_target (x) ((p44 - p43) * scale) and the
    // bracket IS the offset in the target's local frame.
    if (!g_refValid || g_boneCount <= patterns::kBoneRClusterLast) return false;
    const float* pa = g_ref[patterns::kBoneWeaponAttach].p;
    const float* pm = g_ref[patterns::kBoneRClusterLast].p;
    const float s = g_scale[1].load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) out[i] = (pm[i] - pa[i]) * s;
    return true;
}

bool barrel_ref_axis(float d0[3]) {
    // The rendered barrel axis in the DRIVE TARGET's local frame (UE
    // fwd/right/up). Derivation: drive() writes every cluster quat as
    // qtc (x) q_ref with qtc = inv(q_actor) (x) q_target and positions as a
    // rigid rotation of the reference about the anchor, so the rendered
    // world direction of (bone44ref - bone43ref) is q_target (x) d0 - the
    // actor frame cancels. d0 tracks the reference pose, which IS the
    // per-weapon animation (bone 44 = "muzzle-ish tip", patterns.h), so the
    // muzzle ray is per-weapon automatic and follows any authored sway the
    // engine still plays.
    if (!g_refValid || g_boneCount <= patterns::kBoneRClusterLast) return false;
    const float* pa = g_ref[patterns::kBoneWeaponAttach].p;
    const float* pm = g_ref[patterns::kBoneRClusterLast].p; // bone 44
    float d[3] = {pm[0] - pa[0], pm[1] - pa[1], pm[2] - pa[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0f) return false; // degenerate reference (collapsed rig)
    d0[0] = d[0] / len;
    d0[1] = d[1] / len;
    d0[2] = d[2] / len;
    return true;
}

// ---- Arms (two-bone IK) -------------------------------------------------------
// The AHands mesh carries BOTH complete arms: per side a clavicle, upper arm,
// elbow and two forearm-twist helpers (patterns kBone*Sleeve, in that order).
// The drive used to collapse them because they moved rigidly with the gun.
// With arms on, each visible hand's sleeve is posed instead: the shoulder at a
// point the game side derives from the head and a lagging torso yaw (already
// mirrored into engine space when the viewmodel mirror is on), the wrist where
// the hand cluster was just written, the elbow from the fixed bone lengths of
// the engine's own reference pose, bent toward a pole (down and out). Bone
// orientations are the reference orientations carried by the rotation that
// maps each segment's reference frame (direction + bend-plane normal) onto the
// solved one - so no bone-axis convention has to be known.
std::atomic<bool> g_armsOn{false};
std::atomic<bool> g_armScaleS{true}; // scale the arm skin like the hands (.s)
std::atomic<float> g_armLength{1.0f}; // bone-length multiplier (reach vs Jack's arm)
bool g_armTargetValid[2] = {false, false};
float g_armShoulderW[2][3] = {};
float g_armPoleW[2][3] = {};
float g_armOutW[2][3] = {}; // the arm's outward side: the elbow never points past the body line
std::atomic<uint32_t> g_armSolves{0}, g_armStretched{0};
constexpr float kPi = 3.14159265f;
float g_armTwist[2] = {0.0f, 0.0f};  // forearm roll, followed continuously (rad)
float g_armPrevPole[2][3] = {};       // last frame's elbow direction (unit)
uint64_t g_armTwistMs[2] = {0, 0};
std::atomic<float> g_armTwistDeg[2] = {0.0f, 0.0f}; // readout

void v_sub(const float a[3], const float b[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = a[i] - b[i];
}
float v_len(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
void v_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
float v_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool v_norm(float a[3]) {
    const float l = v_len(a);
    if (l < 1e-5f) return false;
    for (int i = 0; i < 3; ++i) a[i] /= l;
    return true;
}

// Rotation (quaternion, xyzw) from a 3x3 whose COLUMNS are the target axes
// expressed in the source axes' coordinates.
void mat_to_quat(const float m[3][3], float q[4]) {
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25f * s;
    }
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i) q[i] /= n;
}

// The rotation that takes frame (x0, n0) onto frame (x1, n1): x = segment
// direction, n = bend-plane normal (both unit, orthogonalised here).
bool frame_rotation(const float x0in[3], const float n0in[3], const float x1in[3],
                    const float n1in[3], float q[4]) {
    float x0[3] = {x0in[0], x0in[1], x0in[2]}, x1[3] = {x1in[0], x1in[1], x1in[2]};
    if (!v_norm(x0) || !v_norm(x1)) return false;
    float n0[3], n1[3], t[3];
    const float d0 = v_dot(n0in, x0), d1 = v_dot(n1in, x1);
    for (int i = 0; i < 3; ++i) {
        n0[i] = n0in[i] - d0 * x0[i];
        n1[i] = n1in[i] - d1 * x1[i];
    }
    if (!v_norm(n0) || !v_norm(n1)) return false;
    float y0[3], y1[3];
    v_cross(n0, x0, y0);
    v_cross(n1, x1, y1);
    // R = B1 * B0^T, B = [x y n] as columns.
    const float* a0[3] = {x0, y0, n0};
    const float* a1[3] = {x1, y1, n1};
    float m[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += a1[k][r] * a0[k][c];
            m[r][c] = sum;
        }
    (void)t;
    mat_to_quat(m, q);
    return true;
}

// Swing-twist: the part of q that turns about unit axis `ax`.
void twist_about(const float q[4], const float ax[3], float out[4]) {
    const float d = q[0] * ax[0] + q[1] * ax[1] + q[2] * ax[2];
    float t[4] = {ax[0] * d, ax[1] * d, ax[2] * d, q[3]};
    const float n = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2] + t[3] * t[3]);
    if (n < 1e-6f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; ++i) out[i] = t[i] / n;
}
void quat_scale_angle(const float q[4], float f, float out[4]) {
    float w = q[3] < -1.0f ? -1.0f : q[3] > 1.0f ? 1.0f : q[3];
    const float ang = 2.0f * acosf(w);
    const float sn = sqrtf(1.0f - w * w);
    if (sn < 1e-5f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    const float h = 0.5f * ang * f;
    for (int i = 0; i < 3; ++i) out[i] = q[i] / sn * sinf(h);
    out[3] = cosf(h);
}

void write_arm_bone(int idx, const float p[3], const float q[4], float sScale) {
    if (idx < 0 || idx >= g_boneCount) return;
    write_n(g_bones[idx].p, p, 12);
    write_n(g_bones[idx].q, q, 16);
    float sv[3] = {g_ref[idx].s[0] * sScale, g_ref[idx].s[1] * sScale, g_ref[idx].s[2] * sScale};
    write_n(g_bones[idx].s, sv, 12);
    if (g_cacheSleeveCount < static_cast<int>(_countof(g_cacheSleeve))) {
        CachedSleeve& cs = g_cacheSleeve[g_cacheSleeveCount++];
        cs.idx = idx;
        memcpy(cs.p, p, 12);
        memcpy(cs.s, sv, 12);
        memcpy(cs.q, q, 16);
        cs.writeQ = true;
    }
}

// Pose `hand`'s sleeve so it runs from its shoulder to the wrist just written
// at Wt. False = no target / degenerate (caller collapses the sleeve instead).
bool arm_ik(int hand, const float Wt[3], const float qaInv[4], const float actorLoc[3]) {
    if (!g_armsOn.load(std::memory_order_relaxed) || !g_armTargetValid[hand] || !g_refValid)
        return false;
    const int* sl = hand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
    const int cla = sl[0], upa = sl[1], elb = sl[2], tw1 = sl[3], tw2 = sl[4];
    const int wri = hand == 1 ? patterns::kBoneRClusterFirst : patterns::kBoneLWrist;
    for (int b : {cla, upa, elb, tw1, tw2, wri})
        if (b < 0 || b >= g_boneCount) return false;

    // Targets into component space.
    float S[3], P[3];
    {
        const float dW[3] = {g_armShoulderW[hand][0] - actorLoc[0],
                             g_armShoulderW[hand][1] - actorLoc[1],
                             g_armShoulderW[hand][2] - actorLoc[2]};
        qts_rotate(qaInv, dW, S);
        qts_rotate(qaInv, g_armPoleW[hand], P);
    }
    float O[3];
    qts_rotate(qaInv, g_armOutW[hand], O);
    const float sc = g_scale[hand].load(std::memory_order_relaxed);
    const float* U0 = g_ref[upa].p;
    const float* E0 = g_ref[elb].p;
    const float* W0 = g_ref[wri].p;
    float ue0[3], ew0[3], uw0[3];
    v_sub(E0, U0, ue0);
    v_sub(W0, E0, ew0);
    v_sub(W0, U0, uw0);
    const float len = g_armLength.load(std::memory_order_relaxed);
    const float a = v_len(ue0) * sc * len, b = v_len(ew0) * sc * len;
    if (a < 1.0f || b < 1.0f) return false;

    // Reach: out of range, the SHOULDER gives (slides toward / away from the
    // hand) rather than the hand leaving the controller.
    float sw[3];
    v_sub(Wt, S, sw);
    float d = v_len(sw);
    if (d < 1e-3f) return false;
    float n[3] = {sw[0] / d, sw[1] / d, sw[2] / d};
    // A hand pulled right in (a stock at the shoulder) must not fold the arm
    // flat: below 40% of full reach the shoulder gives instead.
    const float maxR = (a + b) * 0.995f;
    const float minR = fmaxf(fabsf(a - b) * 1.05f + 0.5f, (a + b) * 0.40f);
    if (d > maxR || d < minR) {
        const float dd = d > maxR ? maxR : minR;
        for (int i = 0; i < 3; ++i) S[i] = Wt[i] - n[i] * dd;
        d = dd;
        g_armStretched.fetch_add(1, std::memory_order_relaxed);
    }
    // Elbow: law of cosines, bent toward the pole.
    const float cosA = (a * a + d * d - b * b) / (2.0f * a * d);
    const float sinA = sqrtf(fmaxf(0.0f, 1.0f - cosA * cosA));
    const uint64_t nowMs = GetTickCount64();
    const bool fresh = nowMs - g_armTwistMs[hand] < 250;
    // The pole's part across the shoulder->wrist line. With the hand straight
    // along the pole (arm hanging toward it) that part shrinks to nothing and
    // its direction turns to noise - the elbow flipped sides, the forearm roll
    // jumped half a turn and the arm contorted. Last frame's elbow direction
    // is mixed in at a fixed weight: negligible while the pole is clear (the
    // result settles on the pole within a few frames), decisive only as it
    // degenerates.
    float pp0[3];
    const float pd = v_dot(P, n);
    for (int i = 0; i < 3; ++i) pp0[i] = P[i] - pd * n[i];
    if (fresh) {
        const float* pv = g_armPrevPole[hand];
        const float vd = v_dot(pv, n);
        const float k = 0.3f * v_len(P);
        for (int i = 0; i < 3; ++i) pp0[i] += k * (pv[i] - vd * n[i]);
    }
    if (!v_norm(pp0)) return false;
    // The arm's outward side, in the plane the elbow swings in.
    float op[3];
    bool haveOut = false;
    {
        const float od = v_dot(O, n);
        for (int i = 0; i < 3; ++i) op[i] = O[i] - od * n[i];
        haveOut = v_norm(op);
    }

    float nRef[3];
    v_cross(ue0, ew0, nRef);
    if (v_len(nRef) < 1e-4f) {
        // The reference arm is straight: take its bend plane from the pole.
        float ref_dir[3] = {uw0[0], uw0[1], uw0[2]};
        v_norm(ref_dir);
        float down[3] = {0.0f, 0.0f, -1.0f};
        v_cross(ref_dir, down, nRef);
    }
    float wq[4];
    if (!read_n(g_bones[wri].q, wq, 16)) return false; // the wrist as the drive wrote it

    // One elbow solve for a given swivel of the pole about the shoulder->wrist
    // line; returns the wrist's remaining roll about the forearm (radians,
    // in (-pi, pi]) and everything needed to write the arm.
    float E[3], ew[3], qUp[4], qFore[4], ax[3];
    auto solve = [&](float swivel) -> float {
        float pp[3];
        {
            const float h = 0.5f * swivel;
            const float qs[4] = {n[0] * sinf(h), n[1] * sinf(h), n[2] * sinf(h), cosf(h)};
            qts_rotate(qs, pp0, pp);
        }
        // Never let the elbow point inside the arm's own side. The inward part
        // is replaced by a small outward lean rather than removed: removing it
        // left almost nothing when the pole pointed straight inward (a hand
        // turned or crossed hard), and that remainder's direction was noise -
        // the elbow spun and the arm twisted through itself.
        if (haveOut) {
            const float o = v_dot(pp, op);
            constexpr float kMinOut = 0.25f;
            if (o < kMinOut)
                for (int i = 0; i < 3; ++i) pp[i] += (kMinOut - o) * op[i];
            v_norm(pp);
        }
        for (int i = 0; i < 3; ++i) E[i] = S[i] + n[i] * a * cosA + pp[i] * a * sinA;
        float se[3], nNew[3];
        v_sub(E, S, se);
        v_sub(Wt, E, ew);
        // The solved bend plane's normal from the POLE, not from the two
        // segments: cross(se, ew) vanishes as the arm straightens and its sign
        // then flips on noise. pp x n never degenerates.
        v_cross(pp, n, nNew);
        if (!frame_rotation(ue0, nRef, se, nNew, qUp)) return NAN;
        if (!frame_rotation(ew0, nRef, ew, nNew, qFore)) return NAN;
        memcpy(ax, ew, sizeof ax);
        if (!v_norm(ax)) return NAN;
        float implied[4], wqInv[4], delta[4], tw[4];
        quat_mul(qFore, g_ref[wri].q, implied);
        quat_conj(implied, wqInv);
        quat_mul(wq, wqInv, delta);
        twist_about(delta, ax, tw);
        const float sn = tw[0] * ax[0] + tw[1] * ax[1] + tw[2] * ax[2];
        float ang = 2.0f * atan2f(sn, tw[3]);
        if (ang > kPi) ang -= 2.0f * kPi;
        if (ang <= -kPi) ang += 2.0f * kPi;
        return ang;
    };
    auto unwrap = [](float raw, float near_) {
        float dlt = raw - near_;
        while (dlt > kPi) dlt -= 2.0f * kPi;
        while (dlt <= -kPi) dlt += 2.0f * kPi;
        return near_ + dlt;
    };

    // Forearm roll, followed continuously: the shortest-way-round reading
    // flips sign at 180 deg, which spun the helpers a full turn in one frame.
    // But continuity must never STICK: a real wrist cannot roll 250 deg, so
    // whenever the direct reading is well inside the normal range it is the
    // truth - a tracked value a full turn away (one bad frame) snaps back
    // instead of holding the arm wrung until the next weapon animation.
    float twist = solve(0.0f);
    if (twist != twist) return false;
    constexpr float kTrustDirect = 110.0f / 57.29578f;
    if (fresh && fabsf(twist) > kTrustDirect) twist = unwrap(twist, g_armTwist[hand]);
    // Past a comfortable roll, the elbow lifts out to carry the excess - the
    // way a real arm turns the whole forearm from the shoulder once the wrist
    // runs out of range - instead of the forearm wringing itself.
    constexpr float kComfort = 80.0f / 57.29578f, kMaxSwivel = 70.0f / 57.29578f;
    // The tracked value is the roll BEFORE the elbow lift (what the wrist
    // itself is doing), so tracking and the lift never feed each other.
    constexpr float kTrack = 250.0f / 57.29578f;
    if (twist > kTrack) twist = kTrack;
    if (twist < -kTrack) twist = -kTrack;
    g_armTwist[hand] = twist;
    g_armTwistMs[hand] = nowMs;
    memcpy(g_armPrevPole[hand], pp0, sizeof pp0); // the base pole (pre-swivel): no feedback
    float swivel = 0.0f;
    if (fabsf(twist) > kComfort) {
        swivel = fabsf(twist) - kComfort;
        if (swivel > kMaxSwivel) swivel = kMaxSwivel;
        if (twist < 0.0f) swivel = -swivel;
        const float t2 = solve(swivel);
        if (t2 != t2) return false;
        twist = unwrap(t2, twist - swivel); // what the forearm helpers still carry
    }
    g_armTwistDeg[hand].store(twist * 57.29578f, std::memory_order_relaxed);

    const float sS = g_armScaleS.load(std::memory_order_relaxed) ? sc : 1.0f;
    float q[4], p[3], rel[3], relR[3];

    // Upper arm at the shoulder, elbow at the elbow.
    quat_mul(qUp, g_ref[upa].q, q);
    write_arm_bone(upa, S, q, sS);
    quat_mul(qFore, g_ref[elb].q, q);
    write_arm_bone(elb, E, q, sS);

    // Clavicle rides the upper arm's swing only loosely: its reference offset
    // from the shoulder, turned by half of the upper-arm rotation.
    {
        float half[4];
        quat_scale_angle(qUp, 0.35f, half);
        v_sub(g_ref[cla].p, U0, rel);
        for (float& c : rel) c *= sc;
        qts_rotate(half, rel, relR);
        for (int i = 0; i < 3; ++i) p[i] = S[i] + relR[i];
        quat_mul(half, g_ref[cla].q, q);
        write_arm_bone(cla, p, q, sS);
    }

    // Twist helpers: along the forearm (reference offset from the elbow,
    // forearm rotation), sharing out the remaining roll - never past 150 deg,
    // beyond which the skin between the helpers folds through itself.
    {
        constexpr float kMaxTwist = 150.0f / 57.29578f;
        float tw = twist;
        if (tw > kMaxTwist) tw = kMaxTwist;
        if (tw < -kMaxTwist) tw = -kMaxTwist;
        const int helpers[2] = {tw1, tw2};
        const float frac[2] = {0.33f, 0.66f};
        for (int k = 0; k < 2; ++k) {
            const int hb = helpers[k];
            const float h = 0.5f * tw * frac[k];
            const float part[4] = {ax[0] * sinf(h), ax[1] * sinf(h), ax[2] * sinf(h), cosf(h)};
            float qh[4];
            v_sub(g_ref[hb].p, E0, rel);
            for (float& c : rel) c *= sc;
            qts_rotate(qFore, rel, relR);
            for (int i = 0; i < 3; ++i) p[i] = E[i] + relR[i];
            quat_mul(qFore, g_ref[hb].q, qh);
            quat_mul(part, qh, q);
            write_arm_bone(hb, p, q, sS);
        }
    }
    g_armSolves.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Where the wrist of `hand` sits after rigid_cluster wrote it with (ptc, qtc)
// from reference `src` about `anchor`.
void written_wrist(int hand, int anchor, const float ptc[3], const float qtc[4], const Qts* src,
                   float Wt[3]) {
    const int wri = hand == 1 ? patterns::kBoneRClusterFirst : patterns::kBoneLWrist;
    const float sc = g_scale[hand].load(std::memory_order_relaxed);
    float rel[3] = {(src[wri].p[0] - src[anchor].p[0]) * sc, (src[wri].p[1] - src[anchor].p[1]) * sc,
                    (src[wri].p[2] - src[anchor].p[2]) * sc};
    float r[3];
    qts_rotate(qtc, rel, r);
    for (int i = 0; i < 3; ++i) Wt[i] = ptc[i] + r[i];
}

// ---- Grip-pose placement: Jack's palm on your palm ---------------------------
// OpenXR's GRIP pose is anchored to the user's hand, not to the controller's
// pointing ray: origin at the palm centroid, -Z through the tube the curled
// fingers make (little finger -> thumb), +X along the palm normal (away from
// the palm on the left hand, into it on the right: "right" for both). The same
// frame is measured on Jack's hand from its own bones - wrist, the four finger
// base knuckles, the thumb - expressed in the WRIST's local frame (it rides
// the wrist rigidly, whatever the animation does to the fingers). Placing the
// hand is then one rigid solve: put that palm frame on the grip frame.

int wrist_of(int hand) { return hand == 1 ? patterns::kBoneRClusterFirst : patterns::kBoneLWrist; }

bool compute_palm_local(int hand) {
    if (!g_refValid || !g_skelInst) return false;
    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    const int w = wrist_of(hand);
    if (w < 0 || w + 15 >= g_boneCount) return false;
    // Finger bases: by NAME first (3ds Max biped: Finger0 = thumb, Finger1..4 =
    // index..little), else the measured layout - wrist, thumb x3, then four
    // chains of three (the right cluster 27 | 28-30 | 31-42 is documented; the
    // left 6 | 7-9 | 10-21 has the same shape).
    int thumb0 = w + 1, thumb1 = w + 2, base[4] = {w + 4, w + 7, w + 10, w + 13};
    bool named = false;
    {
        Skel sk{g_skelInst, g_bones, g_boneCount};
        const wchar_t* names[kMaxBones] = {};
        if (resolve_bone_names(sk, names, g_boneCount) > 0) {
            int f[5] = {-1, -1, -1, -1, -1};
            for (int i = first; i <= last && i < g_boneCount; ++i) {
                if (!names[i]) continue;
                const size_t n = wcslen(names[i]);
                if (n < 7) continue;
                const wchar_t* tail = names[i] + n - 7; // "FingerN"
                if (_wcsnicmp(tail, L"Finger", 6) == 0 && tail[6] >= L'0' && tail[6] <= L'4')
                    f[tail[6] - L'0'] = i;
            }
            if (f[0] >= 0 && f[1] >= 0 && f[2] >= 0 && f[3] >= 0 && f[4] >= 0) {
                thumb0 = f[0];
                thumb1 = f[0] + 1 <= last ? f[0] + 1 : f[0];
                base[0] = f[1]; base[1] = f[2]; base[2] = f[3]; base[3] = f[4];
                named = true;
            }
            if (!g_palmLogged[hand]) {
                BVR_LOG("[bones] %s hand bones: %ls | %ls | %ls %ls %ls %ls (%s)",
                        hand == 1 ? "right" : "left", names[w] ? names[w] : L"?",
                        names[thumb0] ? names[thumb0] : L"?", names[base[0]] ? names[base[0]] : L"?",
                        names[base[1]] ? names[base[1]] : L"?", names[base[2]] ? names[base[2]] : L"?",
                        names[base[3]] ? names[base[3]] : L"?", named ? "by name" : "by layout");
            }
        }
    }
    const float* W = g_ref[w].p;
    // Index = the base nearest the thumb, little = the farthest.
    int idx = 0, lit = 0;
    float dmin = 1e30f, dmax = -1.0f;
    for (int k = 0; k < 4; ++k) {
        float d[3];
        v_sub(g_ref[base[k]].p, g_ref[thumb0].p, d);
        const float l = v_len(d);
        if (l < dmin) { dmin = l; idx = k; }
        if (l > dmax) { dmax = l; lit = k; }
    }
    if (idx == lit) return false;
    float K[3] = {0, 0, 0};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i) K[i] += g_ref[base[k]].p[i] * 0.25f;
    float f[3], wk[3], nrm[3];
    v_sub(g_ref[base[idx]].p, g_ref[base[lit]].p, f); // little -> index: the tube
    if (!v_norm(f)) return false;
    v_sub(K, W, wk);
    v_cross(wk, f, nrm);
    if (!v_norm(nrm)) return false;
    // Palm side: the thumb sits palm-side of the knuckle plane and curled
    // fingertips fall on it.
    float score = 0.0f;
    {
        float t[3];
        v_sub(g_ref[thumb1].p, W, t);
        score += 2.0f * v_dot(t, nrm);
        for (int k = 0; k < 4; ++k) {
            const int tip = base[k] + 2 <= last ? base[k] + 2 : base[k];
            float c[3];
            v_sub(g_ref[tip].p, g_ref[base[k]].p, c);
            score += v_dot(c, nrm);
        }
    }
    float face[3] = {nrm[0], nrm[1], nrm[2]};
    if (score < 0.0f)
        for (float& c : face) c = -c;
    // Grip +X: into the right palm / away from the left palm.
    float r[3];
    for (int i = 0; i < 3; ++i) r[i] = hand == 1 ? -face[i] : face[i];
    const float fr = v_dot(r, f);
    for (int i = 0; i < 3; ++i) r[i] -= fr * f[i];
    if (!v_norm(r)) return false;
    float u[3];
    v_cross(f, r, u);
    const float m[3][3] = {{f[0], r[0], u[0]}, {f[1], r[1], u[1]}, {f[2], r[2], u[2]}};
    float qp[4];
    mat_to_quat(m, qp);
    // Palm centroid: halfway from the wrist to the knuckle line.
    const float C[3] = {(W[0] + K[0]) * 0.5f, (W[1] + K[1]) * 0.5f, (W[2] + K[2]) * 0.5f};
    float wqInv[4], dC[3];
    quat_conj(g_ref[w].q, wqInv);
    v_sub(C, W, dC);
    PalmLocal& pl = g_palm[hand];
    qts_rotate(wqInv, dC, pl.p);
    quat_mul(wqInv, qp, pl.q);
    qts_rotate(wqInv, face, pl.face);
    pl.valid = true;
    if (!g_palmLogged[hand]) {
        g_palmLogged[hand] = true;
        float dk[3];
        v_sub(K, W, dk);
        BVR_LOG("[bones] %s palm frame: wrist->knuckles %.1f, index %d little %d, palm-side score "
                "%.1f",
                hand == 1 ? "right" : "left", v_len(dk), base[idx], base[lit], score);
    }
    return true;
}

// The pose `src` draws `hand` with (mirrors drive() / drive_off_hand()).
const Qts* draw_source(int hand, bool driven) {
    if (driven || hand == 1) return g_ref;
    const bool wantGrip = g_offFollow || g_offPreview;
    if (!wantGrip && g_neutralValid) return g_neutral;
    if (wantGrip && g_gripShapeValid) return g_gripShape;
    return g_ref;
}

// One cluster, moved rigidly: rotate its reference pose by qtc about its
// reference anchor point, then put the anchor point at ptc. Appends every write
// to the reapply cache (the caller resets it once per drive).
// A second pose for rigid_cluster to blend toward, bone by bone (wB = 0..1).
struct ClusterBlend {
    const float* ptc;
    const float* qtc;
    const Qts* src;
    float w;
};

bool rigid_cluster(int hand, int first, int last, int anchor, const float ptc[3],
                   const float qtc[4], const Qts* src = g_ref, const ClusterBlend* blend = nullptr) {
    // Rigid move: rotate the reference cluster by qtc about the reference
    // anchor point, then put the anchor point at the target. Every write is
    // also cached for reapply() - the stereo second pass must be able to
    // restore this exact set after the engine re-evaluates over it.
    const float* pa = src[anchor].p;
    // Viewmodel scale (session 61, see the g_scale block comment): the
    // anchor-relative translations shrink by s for every cluster bone - the
    // cluster scales ABOUT THE ANCHOR, so the anchor write-loc is unchanged
    // by s (the proof metric) - and the .s channel is written only for
    // mode-selected bones, from the PINNED reference, never adopted back.
    // On the off edge (s back to 1.0, mode change) the authored scale is
    // written back explicitly - the engine cannot be relied on to
    // re-evaluate while the drive keeps clearing the dirty flag (the sleeve
    // collapse learned the same lesson).
    const float s = g_scale[hand].load(std::memory_order_relaxed);
    const bool scaling = s != 1.0f;
    const int sMode = g_scaleMode.load(std::memory_order_relaxed);
    for (int i = first; i <= last; ++i) {
        float rel[3] = {(src[i].p[0] - pa[0]) * s, (src[i].p[1] - pa[1]) * s,
                        (src[i].p[2] - pa[2]) * s};
        float rot[3];
        qts_rotate(qtc, rel, rot);
        float p[3] = {ptc[0] + rot[0], ptc[1] + rot[1], ptc[2] + rot[2]};
        float q[4];
        quat_mul(qtc, src[i].q, q);
        if (blend && blend->w > 0.0f) {
            const Qts* sb = blend->src;
            const float* pab = sb[anchor].p;
            const float relB[3] = {(sb[i].p[0] - pab[0]) * s, (sb[i].p[1] - pab[1]) * s,
                                   (sb[i].p[2] - pab[2]) * s};
            float rotB[3], qB[4];
            qts_rotate(blend->qtc, relB, rotB);
            quat_mul(blend->qtc, sb[i].q, qB);
            const float w = blend->w;
            for (int k = 0; k < 3; ++k) p[k] += (blend->ptc[k] + rotB[k] - p[k]) * w;
            const float d = q[0] * qB[0] + q[1] * qB[1] + q[2] * qB[2] + q[3] * qB[3];
            const float sg = d < 0.0f ? -1.0f : 1.0f;
            float n2 = 0.0f;
            for (int k = 0; k < 4; ++k) {
                q[k] += (sg * qB[k] - q[k]) * w;
                n2 += q[k] * q[k];
            }
            const float inv = n2 > 1e-12f ? 1.0f / sqrtf(n2) : 1.0f;
            for (float& c : q) c *= inv;
        }
        if (!write_n(g_bones[i].p, p, 12) || !write_n(g_bones[i].q, q, 16)) {
            g_skelInst = nullptr; // faulted mid-write: revalidate next frame
            g_cacheMs = 0;
            return false;
        }
        bool wantS = scaling && scale_selects(sMode, hand, i, first);
        float sv[3];
        if (wantS) {
            sv[0] = g_ref[i].s[0] * s;
            sv[1] = g_ref[i].s[1] * s;
            sv[2] = g_ref[i].s[2] * s;
            if (write_n(g_bones[i].s, sv, 12)) {
                memcpy(g_lastWrittenS[i], sv, 12);
                g_scaleWrote[i] = true;
            } else {
                wantS = false;
            }
        } else if (g_scaleWrote[i]) {
            write_n(g_bones[i].s, g_ref[i].s, 12); // off edge: authored back
            g_scaleWrote[i] = false;
        }
        CachedBone& cb = g_cache[g_cacheCount++];
        cb.idx = i;
        memcpy(cb.p, p, 12);
        memcpy(cb.q, q, 16);
        cb.writeScale = wantS;
        if (wantS) memcpy(cb.s, sv, 12);
    }

    return true;
}

// World target -> component space against the hands actor (same algebra as
// drive(); see the frame note there).
bool target_to_component(const GamePose& gp, const float qaInv[4], const float actorLoc[3],
                         float ptc[3], float qtc[4]) {
    float qt[4];
    ue_rot_to_quat(gp.rot, qt);
    quat_mul(qaInv, qt, qtc);
    float dWorld[3] = {gp.loc.x - actorLoc[0], gp.loc.y - actorLoc[1], gp.loc.z - actorLoc[2]};
    qts_rotate(qaInv, dWorld, ptc);
    return ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2] <= 500.0f * 500.0f;
}

// ---- Follow v5: the held hand rides the weapon PART under it ----------------
// Every gun carries its own skeleton (the weapon-scale lane binds it): the
// shotgun's SG_Pump, the chemical thrower's Bone_Wrench, the grenade
// launcher's MainBarrelBone and ammo drum, the crossbow's levers. v4 copied the
// ENGINE's off hand instead - right while that hand is on the moving part (the
// pump), wrong when it leaves the gun (the grenade launcher's reload: the
// engine hand goes to load a grenade and dragged yours off with it). The held
// hand now rides the part your grab point is on: the grab point, attached to
// that bone at rest, follows the bone's live motion. Measured (animation log):
// the weapon skeleton's axes are the rig's component axes at rest - the barrel
// bones run along +X and the shotgun pump sits exactly where the engine's
// left hand holds it - with its origin at the attach bone, scaled by the
// weapon-scale lane.
Qts g_wRest[kMaxBones];          // weapon pose at rest (parts home)
int g_wRestCount = 0;
void* g_wRestHold = nullptr;      // the holdable it belongs to
bool g_wRestValid = false;
Qts g_wPrev[kMaxBones];           // last frame's weapon pose (stillness)
uint64_t g_wStillSince = 0, g_wMovingSince = 0;
bool g_wPrevValid = false;
std::mutex g_partMx;              // guards the names + the choice (UI thread reads)
char g_partNames[kMaxBones][40];
int g_partCount = 0;
void* g_partNamesHold = nullptr;
char g_partWant[40] = "";         // "" auto (nearest), "*body", "*hand", or a bone name
std::atomic<int> g_autoPart{-1};  // nearest part to the grab point right now
std::atomic<int> g_ridePartUi{-3}; // what the held hand rides (-1 body, -2 classic, -3 idle)
int g_ridePart = -1;
bool g_rideLocked = false;
bool g_wasFollow = false;
// Watching Jack reload: while you hold the grip through a RELOAD, the held
// hand blends over to the engine's own left hand - its fingers included - on
// the drawn gun, so you see Jack load it; let go and the hand is yours again
// (at your controller) to mime it. Fire-cycle animations (the pump after a
// shot, the chemical thrower's wrench, the crossbow prime) keep riding the
// part. A reload = an animation that starts with the reload button down, or
// with no trigger pull in the second before it.
bool g_jackWant = false;
float g_jackW = 0.0f;
uint64_t g_jackMs = 0;
bool g_animWasOn = false;
bool g_animIsReload = false;
uint64_t g_lastReloadBtnMs = 0;
std::atomic<bool> g_jackReloads{true}; // F10 toggle
std::atomic<bool> g_keepSocket{false};  // EVE injection: leave the off weapon hand's socket in place
std::atomic<bool> g_clipEmpty{false};
std::atomic<bool> g_jackFireCycle{false}; // this weapon: watch Jack between shots too   // the gun's magazine is empty (hands' learner)

// The weapon's parts are still: nothing moved more than a hair for 300 ms (or
// a part has been moving for 4 s - an idle loop, not an animation).
void weapon_still_tick() {
    const uint64_t now = GetTickCount64();
    if (!g_wBones || g_wBoneCount <= 0 || g_wBoneCount > kMaxBones) {
        g_wPrevValid = false;
        return;
    }
    bool moved = !g_wPrevValid;
    for (int i = 0; i < g_wBoneCount && !moved; ++i) {
        const float dx = g_wAnim[i].p[0] - g_wPrev[i].p[0], dy = g_wAnim[i].p[1] - g_wPrev[i].p[1],
                    dz = g_wAnim[i].p[2] - g_wPrev[i].p[2];
        float d = fabsf(g_wAnim[i].q[0] * g_wPrev[i].q[0] + g_wAnim[i].q[1] * g_wPrev[i].q[1] +
                        g_wAnim[i].q[2] * g_wPrev[i].q[2] + g_wAnim[i].q[3] * g_wPrev[i].q[3]);
        if (dx * dx + dy * dy + dz * dz > 0.15f * 0.15f || d < 0.99999f) moved = true; // ~0.5 deg
    }
    memcpy(g_wPrev, g_wAnim, sizeof(Qts) * static_cast<size_t>(g_wBoneCount));
    g_wPrevValid = true;
    if (moved) {
        if (!g_wMovingSince) g_wMovingSince = now; // start of this run of motion
        g_wStillSince = now;
    } else if (now - g_wStillSince > 100) {
        g_wMovingSince = 0; // a real pause ends the run
    }
}
bool weapon_still() {
    const uint64_t now = GetTickCount64();
    return now - g_wStillSince >= 300 || (g_wMovingSince && now - g_wMovingSince >= 4000);
}

// At rest (weapon hand raised): the parts' home pose, and their names.
void capture_weapon_rest() {
    if (!g_wBones || g_wBoneCount <= 0 || g_wBoneCount > kMaxBones || !weapon_still()) return;
    memcpy(g_wRest, g_wAnim, sizeof(Qts) * static_cast<size_t>(g_wBoneCount));
    g_wRestCount = g_wBoneCount;
    g_wRestHold = g_wHoldable;
    g_wRestValid = true;
    if (g_partNamesHold == g_wHoldable) return;
    Skel sk{};
    const wchar_t* names[kMaxBones] = {};
    const bool ok = g_wHoldable && resolve_skel(g_wHoldable, sk) && sk.count == g_wBoneCount;
    if (ok) resolve_bone_names(sk, names, sk.count);
    std::lock_guard<std::mutex> lk(g_partMx);
    g_partCount = ok ? sk.count : 0;
    for (int i = 0; i < g_partCount; ++i) {
        char* o = g_partNames[i];
        size_t k = 0;
        if (names[i])
            for (; k + 1 < sizeof g_partNames[i] && names[i][k]; ++k)
                o[k] = names[i][k] < 128 ? static_cast<char>(names[i][k]) : '?';
        if (!k) k = static_cast<size_t>(_snprintf_s(o, sizeof g_partNames[i], _TRUNCATE, "bone%d", i));
        o[k] = 0;
    }
    g_partNamesHold = g_wHoldable;
}

bool weapon_rest_usable() {
    return g_wRestValid && g_wBones && g_wRestHold == g_wHoldable && g_wRestCount == g_wBoneCount &&
           g_wBoneCount > 0;
}

// The part to ride for a grab point at pw (weapon space, authored units):
// the user's choice for this weapon, else the nearest bone. -1 = the gun body
// only, -2 = the engine's own hand (v4).
int choose_part(const float pw[3], int nearest) {
    char want[40];
    {
        std::lock_guard<std::mutex> lk(g_partMx);
        memcpy(want, g_partWant, sizeof want);
        if (strcmp(want, "*body") == 0) return -1;
        if (strcmp(want, "*hand") == 0) return -2;
        if (want[0] && g_partNamesHold == g_wHoldable)
            for (int i = 0; i < g_partCount; ++i)
                if (strcmp(g_partNames[i], want) == 0) return i;
    }
    (void)pw;
    return nearest;
}

// Jack's palm relative to the cluster anchor for a pose source (unscaled
// units scaled like the cluster), or the anchor itself without a palm frame.
void palm_rel_src(int hand, const Qts* src, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!g_palm[hand].valid && !compute_palm_local(hand)) return;
    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    const int w = wrist_of(hand);
    if (w >= g_boneCount || anchor >= g_boneCount) return;
    float pw[3];
    qts_rotate(src[w].q, g_palm[hand].p, pw);
    const float s = g_scale[hand].load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) out[i] = (src[w].p[i] + pw[i] - src[anchor].p[i]) * s;
}

// The OFF hand, tracked (BioVR's always-visible free hand): its cluster goes
// to g_offGp with the same rigid move, its sleeve collapses like the driven
// one. The WEAPON cluster as the off hand (plasmid raised) keeps the holstered
// gun hidden: bone 43 parks far below by translation (never scale - see the
// hide block in drive()) and 44 collapses, then the cluster counts as hidden
// so the normal restore runs when that hand becomes the driven one again.
bool drive_off_hand(int ih, const float qaInv[4], const float actorLoc[3], bool collapse,
                    const float qtcMain[4], const float ptcMain[3]) {
    int first = 0, last = 0, anchor = 0;
    cluster_of(ih, &first, &last, &anchor);
    if (first < 0 || last >= g_boneCount || anchor < first || anchor > last) return false;
    float ptc[3], qtc[4];
    if (!target_to_component(g_offGp, qaInv, actorLoc, ptc, qtc)) return false;
    const bool wantGripShape = g_offFollow || g_offPreview;
    const Qts* shapeSrc = ih == 0 && !wantGripShape && g_neutralValid ? g_neutral
                          : ih == 0 && wantGripShape && g_gripShapeValid ? g_gripShape
                                                                         : g_ref;
    // Part riding needs the weapon at rest and the grab point in its frame.
    const bool parts = ih == 0 && weapon_rest_usable();
    const float ws = g_wScale.load(std::memory_order_relaxed); // the gun's drawn size
    float palmC[3] = {ptc[0], ptc[1], ptc[2]}, pw[3] = {0, 0, 0};
    if (parts && (g_offFollow || g_offPreview) && ws > 1e-3f) {
        float pr[3], prc[3], mInv0[4];
        palm_rel_src(ih, shapeSrc, pr);
        qts_rotate(qtc, pr, prc);
        for (int k = 0; k < 3; ++k) palmC[k] = ptc[k] + prc[k];
        quat_conj(qtcMain, mInv0);
        const float dcm[3] = {palmC[0] - ptcMain[0], palmC[1] - ptcMain[1], palmC[2] - ptcMain[2]};
        qts_rotate(mInv0, dcm, pw);
        for (float& c : pw) c /= ws;
        int best = -1;
        float bestD = 1e30f;
        for (int i = 0; i < g_wRestCount; ++i) {
            const float dx = pw[0] - g_wRest[i].p[0], dy = pw[1] - g_wRest[i].p[1],
                        dz = pw[2] - g_wRest[i].p[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD) {
                bestD = d2;
                best = i;
            }
        }
        g_autoPart.store(best, std::memory_order_relaxed);
    }
    const bool following = g_offFollow && ih == 0 && g_followBaseValid && g_liveValid;
    if (ih == 0) {
        if (following && !g_wasFollow) g_rideLocked = false; // a new hold picks its part
        g_wasFollow = following;
    }
    if (following) {
        // Two motions, applied in the order they happen:
        //  1. TILT - the drawn gun turns about its attach bone (a pump rocks
        //     the whole shotgun back). The pinned hand was placed for the gun
        //     at rest, so it turns about the attach point with it:
        //     T = qtcMain (ref43_now ref43_rest^-1) qtcMain^-1.
        //  2. The PART under the hand (v5), or the engine hand's stroke (v4).
        const Qts& r43 = g_ref[patterns::kBoneWeaponAttach];
        float mInv[4];
        quat_conj(qtcMain, mInv);
        float r0i[4], tl[4], t1[4], T[4];
        quat_conj(g_followBaseRef43.q, r0i);
        quat_mul(r43.q, r0i, tl);
        quat_mul(qtcMain, tl, t1);
        quat_mul(t1, mInv, T);
        const float qtc0[4] = {qtc[0], qtc[1], qtc[2], qtc[3]};
        float palmT[3];
        {
            const float off[3] = {ptc[0] - ptcMain[0], ptc[1] - ptcMain[1], ptc[2] - ptcMain[2]};
            const float offP[3] = {palmC[0] - ptcMain[0], palmC[1] - ptcMain[1], palmC[2] - ptcMain[2]};
            float offT[3], offPT[3];
            qts_rotate(T, off, offT);
            qts_rotate(T, offP, offPT);
            for (int k = 0; k < 3; ++k) {
                ptc[k] = ptcMain[k] + offT[k];
                palmT[k] = ptcMain[k] + offPT[k];
            }
        }
        quat_mul(T, qtc0, qtc);

        int part = -2;
        if (parts && ws > 1e-3f) {
            if (!g_rideLocked) {
                g_ridePart = choose_part(pw, g_autoPart.load(std::memory_order_relaxed));
                g_rideLocked = true;
            }
            part = g_ridePart;
        }
        g_ridePartUi.store(part, std::memory_order_relaxed);

        if (part >= 0 && part < g_wBoneCount) {
            // The grab point, fixed to the part at rest, where the part has
            // taken it: pw' = b_now (b_rest^-1 pw). Weapon space -> drawn:
            // M = T qtcMain (the gun's rest axes, tilted with it), times ws.
            const Qts& bn = g_wAnim[part];
            const Qts& b0 = g_wRest[part];
            float b0i[4], Rb[4], rel[3], relR[3];
            quat_conj(b0.q, b0i);
            quat_mul(bn.q, b0i, Rb);
            for (int k = 0; k < 3; ++k) rel[k] = pw[k] - b0.p[k];
            qts_rotate(Rb, rel, relR);
            float dw[3];
            for (int k = 0; k < 3; ++k) dw[k] = (bn.p[k] + relR[k] - pw[k]) * ws;
            float M[4], Mi[4], dc[3], u[4], D[4];
            quat_mul(T, qtcMain, M);
            quat_conj(M, Mi);
            qts_rotate(M, dw, dc);
            quat_mul(M, Rb, u);
            quat_mul(u, Mi, D); // the part's turn, in the drawn frame
            // The hand turns WITH the part about the grab point.
            float arm[3], armR[3];
            for (int k = 0; k < 3; ++k) arm[k] = ptc[k] - palmT[k];
            qts_rotate(D, arm, armR);
            for (int k = 0; k < 3; ++k) ptc[k] = palmT[k] + dc[k] + armR[k];
            float q2[4];
            quat_mul(D, qtc, q2);
            memcpy(qtc, q2, sizeof q2);
        } else if (part == -2) {
            // v4: the engine hand's motion RELATIVE to the gun, measured in the
            // gun's live frame and applied in the drawn frame G = qtcMain ref43.
            const Qts& w = g_live[0];
            const Qts& a = g_live[1];
            const Qts& w0 = g_followBase[0];
            const Qts& a0 = g_followBase[1];
            float G[4], Gi[4];
            quat_mul(qtcMain, r43.q, G);
            quat_conj(G, Gi);
            float ai[4], a0i[4];
            quat_conj(a.q, ai);
            quat_conj(a0.q, a0i);
            const float rw[3] = {w.p[0] - a.p[0], w.p[1] - a.p[1], w.p[2] - a.p[2]};
            const float rw0[3] = {w0.p[0] - a0.p[0], w0.p[1] - a0.p[1], w0.p[2] - a0.p[2]};
            float l[3], l0[3];
            qts_rotate(ai, rw, l);
            qts_rotate(a0i, rw0, l0);
            float d[3] = {l[0] - l0[0], l[1] - l0[1], l[2] - l0[2]};
            const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            constexpr float kMaxFollowUu = 40.0f; // a pump stroke, never an equip swing
            if (len > kMaxFollowUu)
                for (float& c : d) c *= kMaxFollowUu / len;
            for (float& c : d) c *= ws;
            float dc[3];
            qts_rotate(G, d, dc);
            for (int k = 0; k < 3; ++k) ptc[k] += dc[k];
            // Orientation: the wrist's own turn relative to the gun
            // (D = G (r r0^-1) G^-1, r = a^-1 w), on top of the tilt.
            float r[4], rr0[4], rr0i[4], dr[4], u1[4], D[4], q3[4];
            quat_mul(ai, w.q, r);
            quat_mul(a0i, w0.q, rr0);
            quat_conj(rr0, rr0i);
            quat_mul(r, rr0i, dr);
            quat_mul(G, dr, u1);
            quat_mul(u1, Gi, D);
            quat_mul(D, qtc, q3);
            memcpy(qtc, q3, sizeof q3);
        }
        // part == -1: the gun body only - the tilt above is the whole ride.
    } else if (ih == 0) {
        g_ridePartUi.store(-3, std::memory_order_relaxed);
    }
    // Shape: the rest grip while held, recording or in the grab zone (so you
    // see how the hand will sit); the relaxed pose otherwise.
    const bool wantGrip = g_offFollow || g_offPreview;
    const bool relaxed = ih == 0 && !wantGrip && g_neutralValid;
    const bool gripShape = ih == 0 && wantGrip && g_gripShapeValid;
    // Jack's own hand on the drawn gun (see g_jackWant): the engine's live left
    // hand relative to its live attach bone, carried by the drawn gun's frame
    // G = qtcMain ref43 live43^-1 and set on the gun at the gun's drawn size.
    ClusterBlend jack{nullptr, nullptr, g_liveL, 0.0f};
    float ptcJ[3], qtcJ[4];
    if (ih == 0) {
        const uint64_t nowJ = GetTickCount64();
        const float dt = g_jackMs ? static_cast<float>(nowJ - g_jackMs) : 0.0f;
        g_jackMs = nowJ;
        if (g_jackWant) g_jackW = fminf(1.0f, g_jackW + dt / 150.0f); // in over 150 ms
        else g_jackW = fmaxf(0.0f, g_jackW - dt / 200.0f);          // home over 200 ms
        if (g_jackW > 0.0f && g_liveValid && anchor < g_boneCount) {
            const Qts& L43 = g_live[1];
            const Qts& r43 = g_ref[patterns::kBoneWeaponAttach];
            float t[4], l43i[4];
            quat_conj(L43.q, l43i);
            quat_mul(qtcMain, r43.q, t);
            quat_mul(t, l43i, qtcJ);
            const float ws = g_wScale.load(std::memory_order_relaxed);
            const float d[3] = {(g_liveL[anchor].p[0] - L43.p[0]) * ws,
                                (g_liveL[anchor].p[1] - L43.p[1]) * ws,
                                (g_liveL[anchor].p[2] - L43.p[2]) * ws};
            float dr[3];
            qts_rotate(qtcJ, d, dr);
            for (int k = 0; k < 3; ++k) ptcJ[k] = ptcMain[k] + dr[k];
            const float x = g_jackW;
            jack = {ptcJ, qtcJ, g_liveL, x * x * (3.0f - 2.0f * x)}; // smoothstep
        }
    }
    const Qts* srcUsed = relaxed ? g_neutral : gripShape ? g_gripShape : g_ref;
    if (!rigid_cluster(ih, first, last, anchor, ptc, qtc, srcUsed, jack.w > 0.0f ? &jack : nullptr))
        return false;
    static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool armed = false;
    if (g_armsOn.load(std::memory_order_relaxed)) {
        float Wt[3];
        written_wrist(ih, anchor, ptc, qtc, srcUsed, Wt);
        if (jack.w > 0.0f) {
            float WtJ[3];
            written_wrist(ih, anchor, ptcJ, qtcJ, g_liveL, WtJ);
            for (int k = 0; k < 3; ++k) Wt[k] += (WtJ[k] - Wt[k]) * jack.w;
        }
        armed = arm_ik(ih, Wt, qaInv, actorLoc);
    }
    if (collapse && !armed) {
        const int* sleeve = ih == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
        const size_t n = ih == 1 ? _countof(patterns::kBoneRSleeve) : _countof(patterns::kBoneLSleeve);
        for (size_t k = 0; k < n; ++k) {
            const int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, ptc, 12);
            write_n(g_bones[idx].s, kZero, 12);
            if (g_cacheSleeveCount < static_cast<int>(_countof(g_cacheSleeve))) {
                CachedSleeve& cs = g_cacheSleeve[g_cacheSleeveCount++];
                cs.idx = idx;
                memcpy(cs.p, ptc, 12);
                memcpy(cs.s, kZero, 12);
                cs.writeQ = false;
            }
        }
    }
    // The EVE holster's injection hangs the syringe on this socket: while it
    // runs, the socket stays where the hand is (parking it took the syringe
    // with it - the hypo vanished the moment the game took it over).
    if (ih == 1 && !g_keepSocket.load(std::memory_order_relaxed)) {
        static const float kFarBelow[3] = {0.0f, 0.0f, -5000.0f};
        const int att = patterns::kBoneWeaponAttach, tip = patterns::kBoneRClusterLast;
        auto hide = [&](int idx, const float* p, bool scale) {
            if (idx < 0 || idx >= g_boneCount) return;
            write_n(g_bones[idx].p, p, 12);
            if (scale) write_n(g_bones[idx].s, kZero, 12);
            g_scaleWrote[idx] = false;
            if (g_cacheHiddenCount < static_cast<int>(_countof(g_cacheHidden))) {
                CachedHidden& ch = g_cacheHidden[g_cacheHiddenCount++];
                ch.idx = idx;
                memcpy(ch.p, p, 12);
                memcpy(ch.s, kZero, 12);
                ch.writeScale = scale;
            }
        };
        hide(att, kFarBelow, false);
        if (tip != att) hide(tip, ptc, true);
        g_hiddenHand = 1;
    }
    return true;
}

// ---- Animation log (Developer tools) ----------------------------------------
// One summary line per weapon animation (fire, pump, reload, twist): how far
// the ENGINE's off hand moved and turned relative to the gun, how far the gun
// tilted, and which of the WEAPON's own bones moved (named) - the data that
// decides how a held hand should ride each gun's moving part. Plus, once per
// weapon at rest, the weapon skeleton and the engine hands in the gun's frame.
std::atomic<bool> g_animLog{false};
char g_dumpedWeapon[64] = {};
struct AnimEpisode {
    bool on = false;
    uint64_t startMs = 0;
    float stroke = 0.0f, turn = 0.0f, tilt = 0.0f;
    float wDisp[kMaxBones];
    float wRot[kMaxBones];
};
AnimEpisode g_ep;

float quat_angle_deg(const float a[4], const float b[4]) {
    float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.29578f;
}

void anim_log_rest(int anchor) {
    if (!g_wRestValid) return;
    if (!g_animLog.load(std::memory_order_relaxed)) return;
    if (strcmp(g_dumpedWeapon, g_placeWeapon) == 0) return;
    strncpy_s(g_dumpedWeapon, sizeof g_dumpedWeapon, g_placeWeapon, _TRUNCATE);
    const Qts& a = g_ref[anchor];
    float ai[4];
    quat_conj(a.q, ai);
    auto local = [&](int b, float out[3]) {
        out[0] = out[1] = out[2] = 0.0f;
        if (b < 0 || b >= g_boneCount) return;
        const float d[3] = {g_ref[b].p[0] - a.p[0], g_ref[b].p[1] - a.p[1], g_ref[b].p[2] - a.p[2]};
        qts_rotate(ai, d, out);
    };
    float lw[3], rw[3], tip[3];
    local(patterns::kBoneLWrist, lw);
    local(patterns::kBoneRClusterFirst, rw);
    local(patterns::kBoneRClusterLast, tip);
    BVR_LOG("[animlog] %s at rest, in the attach bone's frame: engine L wrist (%.2f %.2f %.2f) "
            "R wrist (%.2f %.2f %.2f) bone44 (%.2f %.2f %.2f) | attach q (%.3f %.3f %.3f %.3f) "
            "wscale %.3f",
            g_placeWeapon, lw[0], lw[1], lw[2], rw[0], rw[1], rw[2], tip[0], tip[1], tip[2],
            a.q[0], a.q[1], a.q[2], a.q[3], g_wScale.load(std::memory_order_relaxed));
    Skel sk{};
    if (!g_wHoldable || !resolve_skel(g_wHoldable, sk) || sk.count != g_wRestCount) {
        BVR_LOG("[animlog] %s: no weapon skeleton bound", g_placeWeapon);
        return;
    }
    const wchar_t* names[kMaxBones];
    resolve_bone_names(sk, names, sk.count);
    for (int i = 0; i < sk.count; ++i) {
        const Qts& b = g_wRest[i];
        BVR_LOG("[animlog]   w%2d %-24S pos(%8.2f %8.2f %8.2f) quat(%6.3f %6.3f %6.3f %6.3f)", i,
                names[i] ? names[i] : L"<unnamed>", b.p[0], b.p[1], b.p[2], b.q[0], b.q[1], b.q[2],
                b.q[3]);
    }
}

void anim_log_tick(int anchor, bool busy) {
    if (!g_animLog.load(std::memory_order_relaxed) || !g_followBaseValid || !g_liveValid) {
        g_ep.on = false;
        return;
    }
    const uint64_t now = GetTickCount64();
    if (busy) {
        if (!g_ep.on) {
            g_ep.on = true;
            g_ep.startMs = now;
            g_ep.stroke = g_ep.turn = g_ep.tilt = 0.0f;
            for (int i = 0; i < kMaxBones; ++i) g_ep.wDisp[i] = g_ep.wRot[i] = 0.0f;
        }
        const Qts& w = g_live[0];
        const Qts& a = g_live[1];
        const Qts& w0 = g_followBase[0];
        const Qts& a0 = g_followBase[1];
        float ai[4], a0i[4], l[3], l0[3];
        quat_conj(a.q, ai);
        quat_conj(a0.q, a0i);
        const float rw[3] = {w.p[0] - a.p[0], w.p[1] - a.p[1], w.p[2] - a.p[2]};
        const float rw0[3] = {w0.p[0] - a0.p[0], w0.p[1] - a0.p[1], w0.p[2] - a0.p[2]};
        qts_rotate(ai, rw, l);
        qts_rotate(a0i, rw0, l0);
        const float d[3] = {l[0] - l0[0], l[1] - l0[1], l[2] - l0[2]};
        g_ep.stroke = fmaxf(g_ep.stroke, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        float r[4], r0[4];
        quat_mul(ai, w.q, r);
        quat_mul(a0i, w0.q, r0);
        g_ep.turn = fmaxf(g_ep.turn, quat_angle_deg(r, r0));
        g_ep.tilt = fmaxf(g_ep.tilt, quat_angle_deg(g_ref[anchor].q, g_followBaseRef43.q));
        if (g_wBones && g_wRestCount == g_wBoneCount) {
            for (int i = 0; i < g_wBoneCount; ++i) {
                const float dx = g_wAnim[i].p[0] - g_wRest[i].p[0];
                const float dy = g_wAnim[i].p[1] - g_wRest[i].p[1];
                const float dz = g_wAnim[i].p[2] - g_wRest[i].p[2];
                g_ep.wDisp[i] = fmaxf(g_ep.wDisp[i], sqrtf(dx * dx + dy * dy + dz * dz));
                g_ep.wRot[i] = fmaxf(g_ep.wRot[i], quat_angle_deg(g_wAnim[i].q, g_wRest[i].q));
            }
        }
        return;
    }
    if (!g_ep.on) return;
    g_ep.on = false;
    char parts[512] = {};
    size_t used = 0;
    Skel sk{};
    const wchar_t* names[kMaxBones] = {};
    if (g_wHoldable && resolve_skel(g_wHoldable, sk) && sk.count == g_wBoneCount)
        resolve_bone_names(sk, names, sk.count);
    for (int i = 0; i < g_wBoneCount && i < kMaxBones; ++i) {
        if (g_ep.wDisp[i] < 0.5f && g_ep.wRot[i] < 3.0f) continue;
        char nm[40] = {};
        if (names[i]) {
            size_t k = 0;
            for (; k + 1 < sizeof nm && names[i][k]; ++k)
                nm[k] = names[i][k] < 128 ? static_cast<char>(names[i][k]) : '?';
        } else {
            strcpy_s(nm, "?");
        }
        const int n = _snprintf_s(parts + used, sizeof parts - used, _TRUNCATE,
                                  " w%d %s %.1fUU/%.0fdeg;", i, nm, g_ep.wDisp[i], g_ep.wRot[i]);
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
    const int rp = g_ridePartUi.load(std::memory_order_relaxed);
    char ride[48];
    if (rp >= 0 && rp < g_wBoneCount) {
        std::lock_guard<std::mutex> lk(g_partMx);
        _snprintf_s(ride, sizeof ride, _TRUNCATE, "w%d %s", rp,
                    rp < g_partCount ? g_partNames[rp] : "?");
    } else
        strcpy_s(ride, rp == -1 ? "gun body" : rp == -2 ? "engine hand" : "not held");
    BVR_LOG("[animlog] %s: %u ms | engine off hand moved %.1f UU, turned %.0f deg vs the gun | "
            "gun tilted %.0f deg | held hand rides %s | weapon bones:%s",
            g_placeWeapon, static_cast<unsigned>(now - g_ep.startMs), g_ep.stroke, g_ep.turn,
            g_ep.tilt, ride, used ? parts : " none moved");
}

// Grip placement rest (see g_placeRest): called every frame the weapon hand is
// at rest. The first take after a weapon/hand change is exact (the reference
// equals what the live solve just used, so nothing moves); later takes blend
// in over ~80 ms, so a re-freeze of the idle a hair off the old rest re-seats
// the palm smoothly instead of stepping.
void update_place_rest(int anchor) {
    const int w = wrist_of(1);
    if (w >= g_boneCount || anchor >= g_boneCount) return;
    const Qts* now[2] = {&g_ref[w], &g_ref[anchor]};
    const uint64_t t = GetTickCount64();
    if (!g_placeRestValid) {
        g_placeRest[0] = *now[0];
        g_placeRest[1] = *now[1];
        g_placeRestValid = true;
        g_placeRestMs = t;
        return;
    }
    const float dt = static_cast<float>(t - g_placeRestMs);
    g_placeRestMs = t;
    if (dt <= 0.0f) return;
    const float a = 1.0f - expf(-dt / 80.0f);
    for (int k = 0; k < 2; ++k) {
        Qts& r = g_placeRest[k];
        const Qts& n = *now[k];
        for (int i = 0; i < 3; ++i) r.p[i] += (n.p[i] - r.p[i]) * a;
        const float d = r.q[0] * n.q[0] + r.q[1] * n.q[1] + r.q[2] * n.q[2] + r.q[3] * n.q[3];
        const float sgn = d < 0.0f ? -1.0f : 1.0f;
        float len2 = 0.0f;
        for (int i = 0; i < 4; ++i) {
            r.q[i] += (sgn * n.q[i] - r.q[i]) * a;
            len2 += r.q[i] * r.q[i];
        }
        const float inv = len2 > 1e-12f ? 1.0f / sqrtf(len2) : 1.0f;
        for (float& c : r.q) c *= inv;
    }
}

bool live_wrists(float lw[3], float rw[3], float attach[3]) {
    if (!g_liveValid) return false;
    memcpy(lw, g_live[0].p, 12);
    memcpy(rw, g_liveR.p, 12);
    memcpy(attach, g_live[1].p, 12);
    return true;
}
bool ref_animating_now() { return ref_animating(); }

void log_skeleton(void* actor, const char* tag) {
    Skel sk{};
    if (!actor || !resolve_skel(actor, sk)) {
        BVR_LOG("[%s] %p: no skeleton (rigid mesh or not resolvable)", tag, actor);
        return;
    }
    const wchar_t* names[kMaxBones];
    const int n = sk.count < kMaxBones ? sk.count : kMaxBones;
    resolve_bone_names(sk, names, n);
    BVR_LOG("[%s] %p skeleton: %d bones", tag, actor, sk.count);
    for (int i = 0; i < n; ++i) {
        Qts b{};
        if (!read_n(&sk.bones[i], &b, sizeof b)) break;
        BVR_LOG("[%s]   %2d %-24S pos(%7.2f %7.2f %7.2f) quat(%6.3f %6.3f %6.3f %6.3f)", tag, i,
                names[i] ? names[i] : L"<unnamed>", b.p[0], b.p[1], b.p[2], b.q[0], b.q[1], b.q[2], b.q[3]);
    }
}

void set_anim_log(bool on) { g_animLog.store(on, std::memory_order_relaxed); }

void set_ride_part(const char* want) {
    std::lock_guard<std::mutex> lk(g_partMx);
    if (strncmp(g_partWant, want ? want : "", sizeof g_partWant - 1) == 0) return;
    strncpy_s(g_partWant, sizeof g_partWant, want ? want : "", _TRUNCATE);
    g_rideLocked = false; // takes effect on the current hold too
}

int weapon_part_names(char (*out)[40], int cap) {
    std::lock_guard<std::mutex> lk(g_partMx);
    const int n = g_partCount < cap ? g_partCount : cap;
    for (int i = 0; i < n; ++i) memcpy(out[i], g_partNames[i], sizeof g_partNames[i]);
    return n;
}

int ride_part_auto() { return g_autoPart.load(std::memory_order_relaxed); }
void set_jack_fire_cycle(bool on) { g_jackFireCycle.store(on, std::memory_order_relaxed); }
void set_keep_weapon_socket(bool on) { g_keepSocket.store(on, std::memory_order_relaxed); }
void set_clip_empty(bool empty) { g_clipEmpty.store(empty, std::memory_order_relaxed); }
void note_weapon_shot() { g_lastShotMs.store(GetTickCount64(), std::memory_order_relaxed); }
void set_jack_reloads(bool on) { g_jackReloads.store(on, std::memory_order_relaxed); }
bool jack_reloads() { return g_jackReloads.load(std::memory_order_relaxed); }
int ride_part_active() { return g_ridePartUi.load(std::memory_order_relaxed); }

void set_active_weapon(const char* key) {
    if (!key) key = "";
    if (strncmp(key, g_placeWeapon, sizeof g_placeWeapon - 1) == 0) return;
    strncpy_s(g_placeWeapon, sizeof g_placeWeapon, key, _TRUNCATE);
    g_placeRestValid = false; // a new gun holds differently: live until it settles
    g_followBaseValid = false;
}

bool drive(const FrameContext& ctx, void* handsActor, const GamePose& gp, int hand) {
    // Telemetry window: opened here (the once-per-frame pass-1 path) so every
    // module's lines for one sample land together in the log.
    if (g_telemetry.load(std::memory_order_relaxed)) {
        uint64_t now = GetTickCount64();
        g_tlmWindowOpen = (now - g_lastTlmMs >= 200);
        if (g_tlmWindowOpen) g_lastTlmMs = now;
    } else {
        g_tlmWindowOpen = false;
    }

    if (!handsActor || !locate(handsActor)) return false;

    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    if (first < 0 || last >= g_boneCount || anchor < first || anchor > last) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            BVR_LOG("[bones] %s cluster not configured (first %d last %d anchor %d) - "
                    "use vrbones lcluster",
                    hand == 1 ? "RIGHT" : "LEFT", first, last, anchor);
        }
        return false;
    }

    // Reference refresh: if the anchor no longer holds what we last wrote,
    // the engine re-evaluated and the array is a fresh animated pose.
    Qts cur{};
    if (!read_n(&g_bones[anchor], &cur, sizeof cur)) return false;
    bool engineEvaluated =
        !g_hasWritten[hand] || memcmp(&cur, &g_lastWrittenAnchor[hand], sizeof cur) != 0;
    if (engineEvaluated || !g_refValid) {
        // Session 20 sway kill: the idle breathing is the authored idle
        // ANIMATION (channel 0/1 - no script parameter to zero, measured),
        // and it reaches the VR rig only through this recapture. With the
        // kill armed, a fresh engine pose replaces the reference ONLY when
        // it differs by a REAL animation's magnitude (equip/reload/melee -
        // they track live and re-freeze when they settle); idle wobble
        // (measured +-1.2 deg / sub-UU on the anchor) stays frozen out.
        Qts fresh[kMaxBones];
        if (!read_n(g_bones, fresh, sizeof(Qts) * static_cast<size_t>(g_boneCount)))
            return false;
        if (patterns::kBoneLWrist < g_boneCount && patterns::kBoneWeaponAttach < g_boneCount) {
            g_live[0] = fresh[patterns::kBoneLWrist];
            g_live[1] = fresh[patterns::kBoneWeaponAttach];
            if (patterns::kBoneRClusterFirst < g_boneCount) g_liveR = fresh[patterns::kBoneRClusterFirst];
            g_liveValid = true;
            int lf = 0, ll = 0, la = 0;
            cluster_of(0, &lf, &ll, &la);
            for (int i = lf; i <= ll && i < g_boneCount; ++i)
                if (i >= 0) g_liveL[i] = fresh[i];
        }
        bool adopt = true;
        if (g_refValid && g_swayKill.load(std::memory_order_relaxed)) {
            adopt = false;
            uint64_t nowMs = GetTickCount64();
            float maxPos = 0.0f, maxAng = 0.0f;
            static const int kProbe[2] = {patterns::kBoneLWrist, patterns::kBoneWeaponAttach};
            for (int k = 0; k < 2; ++k) {
                int b = kProbe[k];
                if (b >= g_boneCount) continue;
                float dp[3] = {fresh[b].p[0] - g_ref[b].p[0], fresh[b].p[1] - g_ref[b].p[1],
                               fresh[b].p[2] - g_ref[b].p[2]};
                float dot = fresh[b].q[0] * g_ref[b].q[0] + fresh[b].q[1] * g_ref[b].q[1] +
                            fresh[b].q[2] * g_ref[b].q[2] + fresh[b].q[3] * g_ref[b].q[3];
                if (dot < 0.0f) dot = -dot;
                if (dot > 1.0f) dot = 1.0f;
                float angDeg = 2.0f * acosf(dot) * kRadToDeg;
                float posUu = sqrtf(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
                if (posUu > maxPos) maxPos = posUu;
                if (angDeg > maxAng) maxAng = angDeg;
            }
            if (maxPos > kSwayPosThreshUu || maxAng > kSwayAngThreshDeg)
                g_lastBigDeltaMs = nowMs;
            // The pistol's fire kick sits near the 6 UU / 12 deg gate, so
            // against some frozen references it never cleared it: no fire
            // animation at all (only our simulated recoil), and a reference
            // frozen off its settled pose tilted the barrel aim until a hand
            // swap recaptured it. A real shot always tracks.
            if (nowMs - g_lastShotMs.load(std::memory_order_relaxed) < kShotTrackMs)
                g_lastBigDeltaMs = nowMs;
            // Track through the animation AND a settle window past its last
            // big frame, so the eventual freeze holds the SETTLED pose.
            adopt = (nowMs - g_lastBigDeltaMs) < kSwaySettleMs;
            if (adopt && !g_refTracking) g_refTrackingSince = nowMs;
            g_refTracking = adopt;
            if (g_telemetry.load(std::memory_order_relaxed) &&
                nowMs - g_lastSwayTlmMs >= 1000) {
                g_lastSwayTlmMs = nowMs;
                BVR_LOG("[tlm] sway probe: dpos=%.2f UU dang=%.2f deg (thresh %.1f/%.1f) %s",
                        maxPos, maxAng, kSwayPosThreshUu, kSwayAngThreshDeg,
                        adopt ? "TRACKING" : "frozen");
            }
        }
        if (!g_swayKill.load(std::memory_order_relaxed)) g_refTracking = false;
        if (adopt || !g_refValid) {
            // Session 61: pin the scale rows of bones OUR scale writes own.
            // If the bank still holds exactly what we last wrote, the engine
            // did not restamp scale (the BS2 behaviour) - adopting it into
            // g_ref would compound refS * s^n on the next compose, so the
            // previous reference row is kept. If the bank differs, the
            // engine genuinely restamped scale (the Infinite behaviour) -
            // adopt it and count it; the vrbones status readout decides
            // pin-vs-adopt is the right architecture from that number.
            if (g_refValid) {
                for (int i = 0; i < g_boneCount; ++i) {
                    if (!g_scaleWrote[i]) continue;
                    if (memcmp(fresh[i].s, g_lastWrittenS[i], 12) == 0) {
                        memcpy(fresh[i].s, g_ref[i].s, 12);
                    } else {
                        g_scaleRestamps.fetch_add(1, std::memory_order_relaxed);
                        g_scaleWrote[i] = false; // the bank is the engine's again
                    }
                }
            }
            // Arms: the same rule for the sleeve bones. Where a channel still
            // holds exactly what the last drive wrote there (IK or collapse),
            // the engine did not restamp it, and adopting it would turn our
            // own solved arm into the next solve's REFERENCE - the error then
            // compounds, and with the sway kill it froze in until the next
            // big animation re-adopted (the contorted arm that only a reload
            // or weapon switch fixed).
            if (g_refValid) {
                for (int k = 0; k < g_cacheSleeveCount; ++k) {
                    const CachedSleeve& cs = g_cacheSleeve[k];
                    if (cs.idx < 0 || cs.idx >= g_boneCount) continue;
                    Qts& f = fresh[cs.idx];
                    const Qts& r = g_ref[cs.idx];
                    if (memcmp(f.p, cs.p, 12) == 0) memcpy(f.p, r.p, 12);
                    if (memcmp(f.s, cs.s, 12) == 0) memcpy(f.s, r.s, 12);
                    if (cs.writeQ && memcmp(f.q, cs.q, 16) == 0) memcpy(f.q, r.q, 16);
                }
            }
            memcpy(g_ref, fresh, sizeof(Qts) * static_cast<size_t>(g_boneCount));
            g_refValid = true;
        }
    }

    // Follow rest pose: the live wrist-to-gun relation, accepted as REST once
    // it has stayed within kRestHoldUu for kRestHoldMs. A pump or crank keeps
    // moving, so the rest from before the stroke stays in force through it.
    // Never while firing or just after: the chemical thrower's trigger lever
    // stays DOWN for as long as you fire, and a shotgun pump follows the shot -
    // both would otherwise be accepted as the new rest after 300 ms and the
    // hand would spring back off them.
    static uint64_t s_lastTriggerMs = 0;
    {
        uint8_t lt = 0, rt = 0;
        bvr::input::last_composed_triggers(&lt, &rt);
        if (rt >= 30) s_lastTriggerMs = GetTickCount64();
    }
    const bool firingWindow = GetTickCount64() - s_lastTriggerMs < 1000;
    // Plasmid up: the next raise re-settles - placement AND the follow base.
    // A base kept from an earlier raise made the held hand tilt by the
    // difference between two raises' idle freezes whenever you swapped back
    // before the gun settled (frequent plasmid/gun swaps).
    if (hand != 1) {
        g_placeRestValid = false;
        g_followBaseValid = false;
    }
    // Nor while the reference is following an animation: the grenade
    // launcher's reload twists the whole gun with the engine's left hand
    // holding still ON it, so the hand-to-gun relation alone looked like rest
    // mid-twist - the tilt then reset and the held hand jumped off the gun.
    const bool animating = ref_animating();
    if (g_liveValid && (firingWindow || animating)) g_restCandMs = 0;
    if (g_liveValid && !firingWindow && !animating) {
        const uint64_t nowR = GetTickCount64();
        auto rel_dist = [](const Qts* a, const Qts* b) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float d = (a[0].p[k] - a[1].p[k]) - (b[0].p[k] - b[1].p[k]);
                d2 += d * d;
            }
            return sqrtf(d2);
        };
        if (!g_restCandMs || rel_dist(g_live, g_restCand) > kRestHoldUu) {
            g_restCand[0] = g_live[0];
            g_restCand[1] = g_live[1];
            g_restCandMs = nowR;
        } else if (nowR - g_restCandMs >= kRestHoldMs && hand == 1) {
            // Weapon hand raised only: with the plasmid up the attach bone is
            // the HOLSTERED gun, and a base taken there tilted the held hand by
            // the difference on the next raise.
            g_followBase[0] = g_live[0];
            g_followBase[1] = g_live[1];
            g_followBaseValid = true;
            g_followBaseRef43 = g_ref[patterns::kBoneWeaponAttach];
            // The grip shape is taken only while the weapon hand is raised:
            // that is when the engine's left hand is the one on the gun.
            if (hand == 1) {
                update_place_rest(anchor);
                capture_weapon_rest();
                anim_log_rest(anchor);
                {
                    float qi[4];
                    static const float kX[3] = {1.0f, 0.0f, 0.0f};
                    static const float kY[3] = {0.0f, 1.0f, 0.0f};
                    static const float kZ[3] = {0.0f, 0.0f, 1.0f};
                    quat_conj(g_ref[patterns::kBoneWeaponAttach].q, qi);
                    qts_rotate(qi, kX, g_barrelLocal);
                    qts_rotate(qi, kY, g_barrelRightLocal);
                    qts_rotate(qi, kZ, g_barrelUpLocal);
                    g_barrelLocalValid = true;
                }
                int lf = 0, ll = 0, la = 0;
                cluster_of(0, &lf, &ll, &la);
                for (int i = lf; i <= ll && i < g_boneCount; ++i)
                    if (i >= 0) g_gripShape[i] = g_liveL[i];
                g_gripShapeValid = true;
            }
        }
    }

    if (hand == 1) {
        const uint64_t nowJ = GetTickCount64();
        uint16_t btn = 0;
        bvr::input::last_composed_buttons(&btn);
        if (btn & 0x4000) g_lastReloadBtnMs = nowJ; // XINPUT_GAMEPAD_X: reload
        // A reload outlives the 4 s cap ref_animating() puts on any one run of
        // motion (that cap is for idle loops): a shotgun's shell-by-shell load
        // and the crossbow's reload-and-slam both run past it, and Jack's hand
        // used to vanish for the rest (shells flying in on their own). Once a
        // run is a reload it stays one while the engine keeps animating
        // (g_refTracking, uncapped) and across the short pauses between its
        // strokes, up to 15 s.
        static uint64_t s_reloadStartMs = 0, s_reloadLastMoveMs = 0;
        if (g_animIsReload) {
            if (g_refTracking) s_reloadLastMoveMs = nowJ;
            if (nowJ - s_reloadLastMoveMs > 350 || nowJ - s_reloadStartMs > 15000) g_animIsReload = false;
        }
        const bool wasReload = g_animIsReload;
        if (animating && !g_animWasOn)
            g_animIsReload = g_animIsReload || !firingWindow || nowJ - g_lastReloadBtnMs < 800;
        // An empty magazine makes any animation the reload - including the
        // automatic one that runs straight on from the last shot.
        if (animating && (g_clipEmpty.load(std::memory_order_relaxed) ||
                          g_jackFireCycle.load(std::memory_order_relaxed)))
            g_animIsReload = true;
        if (g_animIsReload && !wasReload) {
            s_reloadStartMs = nowJ;
            s_reloadLastMoveMs = nowJ;
        }
        const bool animStart = animating && !g_animWasOn;
        g_animWasOn = animating;
        g_jackWant = g_jackReloads.load(std::memory_order_relaxed) && g_offFollow &&
                     g_animIsReload && g_liveValid;
        // One line per reload-class animation (never per shot): every input
        // to "show Jack's hand", so an intermittent miss names its cause.
        if (animStart && !wasReload && (g_animIsReload || nowJ - g_lastReloadBtnMs < 800)) {
            BVR_LOG("[bones] %s reload anim: jack hand %s (setting %d, grip held %d, counted as reload %d "
                    "[X %llums ago, firing window %d, mag empty %d, between-shots %d], live pose %d)",
                    g_placeWeapon, g_jackWant ? "SHOWN" : "not shown",
                    g_jackReloads.load(std::memory_order_relaxed) ? 1 : 0, g_offFollow ? 1 : 0,
                    g_animIsReload ? 1 : 0, static_cast<unsigned long long>(nowJ - g_lastReloadBtnMs),
                    firingWindow ? 1 : 0, g_clipEmpty.load(std::memory_order_relaxed) ? 1 : 0,
                    g_jackFireCycle.load(std::memory_order_relaxed) ? 1 : 0, g_liveValid ? 1 : 0);
        }
    } else {
        g_jackWant = false;
        g_animWasOn = g_animIsReload = false;
    }
    if (hand == 1) {
        weapon_still_tick();
        anim_log_tick(anchor, g_liveValid && (firingWindow || animating));
    }
    else g_ep.on = false;

    // Neutral capture (v2: the first cut waited for the sway kill to report
    // SETTLED, which a looping plasmid idle never does): once the plasmid hand
    // has been up for 1.2 s and is not casting, take its pose. Re-taken on
    // every raise; saved the first time each session.
    if (!g_neutralLoaded) load_neutral();
    if (hand == 0 && g_refValid) {
        uint8_t lt = 0, rt = 0;
        bvr::input::last_composed_triggers(&lt, &rt);
        const uint64_t nowN = GetTickCount64();
        if (!g_neutralSince) g_neutralSince = nowN;
        if (lt >= 30) g_neutralSince = nowN; // casting: wait for the hand to settle again
        if (!g_neutralTaken && nowN - g_neutralSince > 1200) {
            g_neutralTaken = true;
            for (int i = first; i <= last; ++i) g_neutral[i] = g_ref[i];
            g_neutralValid = true;
            if (!g_neutralSaved) {
                g_neutralSaved = true;
                save_neutral(first, last);
            }
        }
    } else {
        g_neutralSince = 0;
        g_neutralTaken = false;
    }

    // Hide-inactive bookkeeping, BEFORE the rigid write: if the hand about to
    // be driven is the one currently collapsed (hand switch), or the feature
    // just turned off, restore it from the reference first - the rigid write
    // below sets p/q but never touches .s, so a zero scale left behind would
    // keep the incoming hand invisible.
    bool hideInactive = g_hideInactive.load(std::memory_order_relaxed);
    const bool offTrack = hideInactive && g_offTrack;
    if (g_hiddenHand >= 0 && (!hideInactive || g_hiddenHand == hand || offTrack)) {
        restore_hidden(g_hiddenHand);
        g_hiddenHand = -1;
    }

    // World target -> component space, composed against the ACTOR transform.
    // THE FRAME MATTERS, and the in-headset telemetry settled it (2026-07-26,
    // session 12 part 3): during an +-80 deg head-yaw sweep the actor rotation
    // held constant (stick-only, no head-look) while the world target stayed
    // solid - and the user saw the gun move REVERSED, which is exactly
    // actor-frame rendering composed against the camera frame. The renderer
    // orients the rig by the ACTOR fields; composing against them makes the
    // bone values head-independent end to end (the head enters only the view
    // matrix, as it should). The one-day detour through "compose against
    // fc.cam" came from misreading a flat screenshot - see STATUS session 12.
    float actorLoc[3];
    int32_t actorRotRaw[3];
    if (!read_n(static_cast<uint8_t*>(handsActor) + patterns::kActorLocOffset, actorLoc, 12) ||
        !read_n(static_cast<uint8_t*>(handsActor) + patterns::kActorViewDirOffset, actorRotRaw,
                12))
        return false;
    FRotator actorRot{actorRotRaw[0], actorRotRaw[1], actorRotRaw[2]};

    float qa[4], qt[4], qaInv[4], qtc[4];
    ue_rot_to_quat(actorRot, qa);
    memcpy(g_lastQa, qa, sizeof qa);
    memcpy(g_lastActorLoc, actorLoc, sizeof actorLoc);
    g_lastActorValid = true;
    ue_rot_to_quat(gp.rot, qt);
    quat_conj(qa, qaInv);
    quat_mul(qaInv, qt, qtc); // target rotation, component space

    float dWorld[3] = {gp.loc.x - actorLoc[0], gp.loc.y - actorLoc[1], gp.loc.z - actorLoc[2]};
    float ptc[3];
    qts_rotate(qaInv, dWorld, ptc); // target anchor position, component space

    // Sanity: a target further than ~10 m from the actor is a mapping bug,
    // not a pose - refuse to smear the mesh across the map.
    if (ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2] > 500.0f * 500.0f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            BVR_LOG("[bones] target %.0f UU from actor - refusing (mapping bug?)",
                    sqrtf(ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2]));
        }
        return false;
    }

    // Render lock: nudge the whole cluster so the renderer's foreground
    // transform lands the anchor on the world-correct pixel (fails soft to
    // the uncorrected pose when no fresh capture exists - e.g. rig off
    // screen, menu, or the watch not hitting).
    if (g_renderLock.load(std::memory_order_relaxed) != 0) {
        float dLat[3], dDepth[3];
        if (render_lock_delta(ctx, gp, qaInv, actorRot, actorLoc, ptc, dLat, dDepth)) {
            float gl = g_lockGain.load(std::memory_order_relaxed);
            float gd = g_lockDepthGain.load(std::memory_order_relaxed);
            ptc[0] += dLat[0] * gl + dDepth[0] * gd;
            ptc[1] += dLat[1] * gl + dDepth[1] * gd;
            ptc[2] += dLat[2] * gl + dDepth[2] * gd;
            g_lockSolves.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_lockSkips.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (g_tlmWindowOpen) {
        BVR_LOG("[tlm] cam loc=(%.1f %.1f %.1f) rot=(%d %d %d) base=(%.1f %.1f %.1f) "
                "dyaw=%.2f",
                ctx.camX, ctx.camY, ctx.camZ, ctx.camPitch, ctx.camYaw, ctx.camRoll, ctx.baseX,
                ctx.baseY, ctx.baseZ, ctx.driveYawOffsetRad * kRadToDeg);
        BVR_LOG("[tlm] actor loc=(%.1f %.1f %.1f) rot=(%d %d %d) | target loc=(%.1f %.1f "
                "%.1f) rot=(%d %d %d) hand=%d",
                actorLoc[0], actorLoc[1], actorLoc[2], actorRotRaw[0], actorRotRaw[1],
                actorRotRaw[2], gp.loc.x, gp.loc.y, gp.loc.z, gp.rot.pitch, gp.rot.yaw,
                gp.rot.roll, hand);
        BVR_LOG("[tlm] comp p=(%.2f %.2f %.2f) q=(%.3f %.3f %.3f %.3f) | anchorBefore "
                "p=(%.2f %.2f %.2f) engineEval=%d reapplies=%u lock=%.2f solves=%u skips=%u",
                ptc[0], ptc[1], ptc[2], qtc[0], qtc[1], qtc[2], qtc[3], cur.p[0], cur.p[1],
                cur.p[2], engineEvaluated ? 1 : 0,
                g_reapplies.load(std::memory_order_relaxed),
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
    }

    g_cacheCount = 0;
    g_cacheSleeveCount = 0;
    if (!rigid_cluster(hand, first, last, anchor, ptc, qtc)) return false;

    // Sleeve collapse: zero scale hides the geometry; pinning the position at
    // the target keeps any residual skin inside the fist instead of smeared
    // toward the shoulder. On the off-transition the reference values are
    // written back explicitly - the engine cannot be relied on to re-evaluate
    // while the drive keeps clearing the dirty flag.
    bool collapse = g_collapse.load(std::memory_order_relaxed);
    bool armed = false;
    if (g_armsOn.load(std::memory_order_relaxed)) {
        float Wt[3];
        written_wrist(hand, anchor, ptc, qtc, g_ref, Wt);
        armed = arm_ik(hand, Wt, qaInv, actorLoc);
    }
    const bool collapseThis = collapse && !armed;
    const int* sleeve = hand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
    const size_t sleeveCount = hand == 1 ? _countof(patterns::kBoneRSleeve)
                                         : _countof(patterns::kBoneLSleeve);
    if (collapseThis) {
        static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, ptc, 12);
            write_n(g_bones[idx].s, kZero, 12);
            if (g_cacheSleeveCount < static_cast<int>(_countof(g_cacheSleeve))) {
                CachedSleeve& cs = g_cacheSleeve[g_cacheSleeveCount++];
                cs.idx = idx;
                memcpy(cs.p, ptc, 12);
                memcpy(cs.s, kZero, 12);
                cs.writeQ = false;
            }
        }
    } else if (g_wasCollapsed && !armed) {
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, g_ref[idx].p, 12);
            write_n(g_bones[idx].s, g_ref[idx].s, 12);
        }
    }
    g_wasCollapsed = collapseThis;
    g_collapsedHand = collapseThis ? hand : -1;

    // Collapse the whole INACTIVE hand: cluster + its sleeve (session 19).
    // Zero scale hides the skin exactly like the sleeve collapse; positions
    // pin at the driven target so residual geometry stays inside the fist.
    // EXCEPTION - the weapon-attach bone hides by TRANSLATION, scale
    // untouched: the engine's attach path inverse-decomposes chain scale
    // (session 16: any wrist-chain scale blows the attached weapon up
    // near-plane, and zero would be 1/0), so the equipped gun is parked far
    // below the actor in component space and frustum-culled instead.
    g_cacheHiddenCount = 0;
    bool offDone = false;
    if (offTrack) offDone = drive_off_hand(1 - hand, qaInv, actorLoc, collapse, qtc, ptc);
    if (hideInactive && !offDone) {
        const int ih = 1 - hand;
        int hFirst = 0, hLast = 0, hAnchor = 0;
        cluster_of(ih, &hFirst, &hLast, &hAnchor);
        static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        static const float kFarBelow[3] = {0.0f, 0.0f, -5000.0f};
        const int* hSleeve = ih == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
        const size_t hSleeveCount = ih == 1 ? _countof(patterns::kBoneRSleeve)
                                            : _countof(patterns::kBoneLSleeve);
        auto hideBone = [&](int idx) {
            if (idx < 0 || idx >= g_boneCount) return;
            bool isAttach = ih == 1 && idx == patterns::kBoneWeaponAttach;
            const float* p = isAttach ? kFarBelow : ptc;
            write_n(g_bones[idx].p, p, 12);
            if (!isAttach) write_n(g_bones[idx].s, kZero, 12);
            g_scaleWrote[idx] = false; // hide owns the .s channel now
            if (g_cacheHiddenCount < static_cast<int>(_countof(g_cacheHidden))) {
                CachedHidden& ch = g_cacheHidden[g_cacheHiddenCount++];
                ch.idx = idx;
                memcpy(ch.p, p, 12);
                memcpy(ch.s, kZero, 12);
                ch.writeScale = !isAttach;
            }
        };
        for (int i = hFirst; i <= hLast; ++i) hideBone(i);
        for (size_t k = 0; k < hSleeveCount; ++k) hideBone(hSleeve[k]);
        g_hiddenHand = ih;
    }

    if (!read_n(&g_bones[anchor], &g_lastWrittenAnchor[hand], sizeof(Qts))) return false;
    g_hasWritten[hand] = true;
    set_dirty(0); // render-side evaluate-if-dirty must not rebuild over us
    g_cacheSkelInst = g_skelInst;
    g_cacheMs = GetTickCount64();
    g_writes.fetch_add(1, std::memory_order_relaxed);
    g_lastHand.store(hand, std::memory_order_relaxed);
    return true;
}

void set_arm_target(int hand, bool valid, const float shoulderW[3], const float poleW[3],
                    const float outW[3]) {
    if (hand != 0 && hand != 1) return;
    g_armTargetValid[hand] = valid && shoulderW && poleW && outW;
    if (!g_armTargetValid[hand]) return;
    memcpy(g_armShoulderW[hand], shoulderW, 12);
    memcpy(g_armPoleW[hand], poleW, 12);
    memcpy(g_armOutW[hand], outW, 12);
}
void set_arms(bool on) { g_armsOn.store(on, std::memory_order_relaxed); }
void set_arm_length(float m) { g_armLength.store(m, std::memory_order_relaxed); }
float arm_length() { return g_armLength.load(std::memory_order_relaxed); }
bool arms() { return g_armsOn.load(std::memory_order_relaxed); }
void set_arm_scale_skin(bool on) { g_armScaleS.store(on, std::memory_order_relaxed); }
bool arm_scale_skin() { return g_armScaleS.load(std::memory_order_relaxed); }
float arm_twist_deg(int hand) {
    return hand == 0 || hand == 1 ? g_armTwistDeg[hand].load(std::memory_order_relaxed) : 0.0f;
}
void arm_stats(unsigned* solves, unsigned* stretched) {
    *solves = g_armSolves.load(std::memory_order_relaxed);
    *stretched = g_armStretched.load(std::memory_order_relaxed);
}

bool grip_to_anchor(int hand, bool driven, const float gripLoc[3], const float gripQ[4],
                    float palmDepthUu, float outLoc[3], float outQ[4]) {
    if (hand != 0 && hand != 1) return false;
    if (!g_palm[hand].valid && !compute_palm_local(hand)) return false;
    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    const Qts* src = draw_source(hand, driven);
    const int w = wrist_of(hand);
    if (w >= g_boneCount || anchor >= g_boneCount) return false;
    const PalmLocal& pl = g_palm[hand];
    // The weapon hand solves against the gun's REST relation (g_placeRest) so
    // the gun stays rigid to the controller through its own animations.
    const bool rest = driven && hand == 1 && g_placeRestValid;
    const Qts& W = rest ? g_placeRest[0] : src[w];
    const Qts& A = rest ? g_placeRest[1] : src[anchor];
    // Palm frame in component space for the pose that will actually be drawn.
    float qPalm[4], pw[3], face[3];
    quat_mul(W.q, pl.q, qPalm);
    qts_rotate(W.q, pl.p, pw);
    qts_rotate(W.q, pl.face, face);
    const float s = g_scale[hand].load(std::memory_order_relaxed);
    // Relative to the anchor, scaled like the cluster, then pushed toward the
    // palm surface: the grip origin sits inside the fist, a handle's radius in.
    float rel[3];
    for (int i = 0; i < 3; ++i) rel[i] = (W.p[i] + pw[i] - A.p[i]) * s + face[i] * palmDepthUu;
    // Drive frame: q = qGrip * qPalm^-1, anchor = gripLoc - q * rel.
    float pinv[4], rr[3];
    quat_conj(qPalm, pinv);
    quat_mul(gripQ, pinv, outQ);
    qts_rotate(outQ, rel, rr);
    for (int i = 0; i < 3; ++i) outLoc[i] = gripLoc[i] - rr[i];
    return true;
}

bool barrel_dir_target(float yawDeg, float pitchDeg, float out[3]) {
    if (!g_refValid || g_boneCount <= patterns::kBoneWeaponAttach) return false;
    if (!g_barrelLocalValid) return barrel_ref_axis(out);
    // The weapon's own barrel angle in its idle pose (yaw right+, pitch up+,
    // the rig's un-mirrored frame): some models are authored toed in toward
    // the crosshair rather than straight down the view.
    const float y = yawDeg / 57.29578f, pt = pitchDeg / 57.29578f;
    const float cf = cosf(pt) * cosf(y), cr = cosf(pt) * sinf(y), cu = sinf(pt);
    float local[3];
    for (int i = 0; i < 3; ++i)
        local[i] = g_barrelLocal[i] * cf + g_barrelRightLocal[i] * cr + g_barrelUpLocal[i] * cu;
    // Carried by the attach bone's CURRENT reference rotation, so the barrel
    // follows the gun through its own animations (a pump rocks it back).
    qts_rotate(g_ref[patterns::kBoneWeaponAttach].q, local, out);
    return true;
}

// World pose of a bone the last drive wrote (cluster or arm), else false.
bool written_world_pose(int idx, float loc[3], float q[4]) {
    if (!g_lastActorValid) return false;
    const float* p = nullptr;
    const float* r = nullptr;
    for (int k = 0; k < g_cacheCount && !p; ++k)
        if (g_cache[k].idx == idx) {
            p = g_cache[k].p;
            r = g_cache[k].q;
        }
    for (int k = 0; k < g_cacheSleeveCount && !p; ++k)
        if (g_cacheSleeve[k].idx == idx && g_cacheSleeve[k].writeQ) {
            p = g_cacheSleeve[k].p;
            r = g_cacheSleeve[k].q;
        }
    if (!p) return false;
    float rp[3];
    qts_rotate(g_lastQa, p, rp);
    for (int i = 0; i < 3; ++i) loc[i] = g_lastActorLoc[i] + rp[i];
    quat_mul(g_lastQa, r, q);
    return true;
}

int hands_bone_index(const wchar_t* name) {
    if (!g_skelInst || !g_bones || g_boneCount <= 0 || !name) return -1;
    Skel sk{g_skelInst, g_bones, g_boneCount};
    const wchar_t* names[kMaxBones];
    const int n = g_boneCount < kMaxBones ? g_boneCount : kMaxBones;
    resolve_bone_names(sk, names, n);
    for (int i = 0; i < n; ++i)
        if (names[i] && wcscmp(names[i], name) == 0) return i;
    return -1;
}

bool weapon_socket_world(int socket, float loc[3], float q[4]) {
    // From the drawn weapon WRIST, not the socket bone itself: while a plasmid
    // is raised the drive parks the socket (43) far below to hide the
    // holstered gun. The wrist->socket relation is the reference's.
    const int w = patterns::kBoneRClusterFirst, a = socket;
    if (!g_refValid || a < 0 || a >= g_boneCount) return false;
    float wl[3], wq[4];
    if (!written_world_pose(w, wl, wq)) return false;
    float wi[4], rel[3], relW[3], qr[4];
    quat_conj(g_ref[w].q, wi);
    const float s = g_scale[1].load(std::memory_order_relaxed);
    const float d[3] = {(g_ref[a].p[0] - g_ref[w].p[0]) * s, (g_ref[a].p[1] - g_ref[w].p[1]) * s,
                        (g_ref[a].p[2] - g_ref[w].p[2]) * s};
    qts_rotate(wi, d, rel);
    qts_rotate(wq, rel, relW);
    for (int i = 0; i < 3; ++i) loc[i] = wl[i] + relW[i];
    quat_mul(wi, g_ref[a].q, qr);
    quat_mul(wq, qr, q);
    return true;
}

bool written_world(int idx, float out[3]) {
    for (int k = 0; k < g_cacheCount; ++k) {
        if (g_cache[k].idx != idx) continue;
        float r[3];
        qts_rotate(g_lastQa, g_cache[k].p, r);
        for (int i = 0; i < 3; ++i) out[i] = g_lastActorLoc[i] + r[i];
        return g_lastActorValid;
    }
    return false;
}

void set_off_follow(bool on) { g_offFollow = on; }
void set_off_preview(bool on) { g_offPreview = on; }

void set_off_target(bool track, const GamePose* gp) {
    g_offTrack = track && gp;
    if (gp) g_offGp = *gp;
}

void reapply() {
    // Only replay a FRESH write (the paired first pass of this frame). A stale
    // cache must never keep painting an old pose after the drive stops.
    if (!g_cacheSkelInst || g_cacheSkelInst != g_skelInst || !g_bones) return;
    if (GetTickCount64() - g_cacheMs > 100) return;
    g_reapplies.fetch_add(1, std::memory_order_relaxed);
    for (int k = 0; k < g_cacheCount; ++k) {
        const CachedBone& cb = g_cache[k];
        if (cb.idx >= g_boneCount) continue;
        if (!write_n(g_bones[cb.idx].p, cb.p, 12) || !write_n(g_bones[cb.idx].q, cb.q, 16)) {
            g_skelInst = nullptr;
            return;
        }
        if (cb.writeScale) write_n(g_bones[cb.idx].s, cb.s, 12);
    }
    for (int k = 0; k < g_cacheSleeveCount; ++k) {
        const CachedSleeve& cs = g_cacheSleeve[k];
        if (cs.idx >= g_boneCount) continue;
        write_n(g_bones[cs.idx].p, cs.p, 12);
        write_n(g_bones[cs.idx].s, cs.s, 12);
        if (cs.writeQ) write_n(g_bones[cs.idx].q, cs.q, 16);
    }
    for (int k = 0; k < g_cacheHiddenCount; ++k) {
        const CachedHidden& ch = g_cacheHidden[k];
        if (ch.idx >= g_boneCount) continue;
        write_n(g_bones[ch.idx].p, ch.p, 12);
        if (ch.writeScale) write_n(g_bones[ch.idx].s, ch.s, 12);
    }
    set_dirty(0);
    // The weapon's own skeleton gets the same second-pass replay - without it
    // the engine's second CalcView re-evaluates the weapon pose over our
    // write and the eyes render different gun sizes.
    if (g_wHoldable && g_wWrittenValid && g_wBones) {
        for (int i = 0; i < g_wBoneCount; ++i) {
            if (!write_n(g_wBones[i].p, g_wWritten[i].p, 12) ||
                !write_n(g_wBones[i].q, g_wWritten[i].q, 16) ||
                !write_n(g_wBones[i].s, g_wWritten[i].s, 12)) {
                g_wSkelInst = nullptr;
                return;
            }
        }
        wskel_set_dirty(0);
    }
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[bones] usage: vrbones status|list [n]|poke <idx> <dUU>|freeze on|off|"
                "collapse on|off|ref|anchor <idx>|lcluster <lo> <hi> <anchor>");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "status") == 0) {
        BVR_LOG("[bones] inst=%p bones=%p count=%d writes=%u lastHand=%d refValid=%d "
                "collapse=%d hideinactive=%d hiddenHand=%d",
                g_skelInst, static_cast<void*>(g_bones), g_boneCount,
                g_writes.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed), g_refValid ? 1 : 0,
                g_collapse.load(std::memory_order_relaxed) ? 1 : 0,
                g_hideInactive.load(std::memory_order_relaxed) ? 1 : 0, g_hiddenHand);
        // Session 30: the drive-residue state, on demand. Until now cacheAge
        // existed only on the `[b1r] cine edge` line and the sleeve latch was
        // printed nowhere at all, so a hands regression check had to provoke a
        // cutscene edge to read the state it wanted to verify. These are the
        // three things release() clears, plus the gate that decides whether it
        // runs, so one command answers "did the hands get handed back".
        BVR_LOG("[bones] drive residue: cacheAge=%llums wasCollapsed=%d collapsedHand=%d "
                "reapplies=%u | cineHold=%d cineDrive=%s",
                static_cast<unsigned long long>(g_cacheMs ? GetTickCount64() - g_cacheMs : 0),
                g_wasCollapsed ? 1 : 0, g_collapsedHand,
                g_reapplies.load(std::memory_order_relaxed),
                bvr::hud::cinematic_hold() ? 1 : 0,
                bvr::vr::cine_drive_name(bvr::vr::cine_drive()));
        int lockMode = g_renderLock.load(std::memory_order_relaxed);
        BVR_LOG("[bones] render lock: %s |delta|=%.2f UU gain=%.2f dgain=%.2f solves=%u "
                "skips=%u",
                lockMode == 0 ? "off" : lockMode == 2 ? "DIFF" : "ABS",
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockGain.load(std::memory_order_relaxed),
                g_lockDepthGain.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
        BVR_LOG("[bones] right cluster %d-%d anchor %d | left %d-%d anchor %d",
                patterns::kBoneRClusterFirst, patterns::kBoneRClusterLast,
                g_rAnchorOverride.load() >= 0 ? g_rAnchorOverride.load()
                                              : patterns::kBoneWeaponAttach,
                g_lFirst.load(), g_lLast.load(), g_lAnchor.load());
        int sw = 0;
        for (int i = 0; i < g_boneCount; ++i)
            if (g_scaleWrote[i]) ++sw;
        BVR_LOG("[bones] scale: L=%.3f R=%.3f mode=%d (0 cluster-sans-43/44, 1 fingers, "
                "2 wrist, 3 trans-only) scaleWrote=%d engine scale-restamps=%u",
                g_scale[0].load(std::memory_order_relaxed),
                g_scale[1].load(std::memory_order_relaxed),
                g_scaleMode.load(std::memory_order_relaxed), sw,
                g_scaleRestamps.load(std::memory_order_relaxed));
        BVR_LOG("[bones] wskel: ws=%.3f %s holdable=%p count=%d drives=%u adopts=%u",
                g_wScale.load(std::memory_order_relaxed),
                g_wHoldable ? "BOUND" : "dropped", g_wHoldable, g_wBoneCount, g_wDrives,
                g_wAdopts);
    } else if (strcmp(verb, "list") == 0) {
        if (!g_bones) {
            BVR_LOG("[bones] no skeleton located yet (enable the drive or poke once)");
            return;
        }
        int n = g_boneCount;
        sscanf_s(rest, "%d", &n);
        if (n > g_boneCount) n = g_boneCount;
        for (int i = 0; i < n; ++i) {
            Qts b{};
            if (!read_n(&g_bones[i], &b, sizeof b)) break;
            BVR_LOG("[bones] %2d pos(%7.2f %7.2f %7.2f) quat(%6.3f %6.3f %6.3f %6.3f) "
                    "scale(%.2f %.2f %.2f)",
                    i, b.p[0], b.p[1], b.p[2], b.q[0], b.q[1], b.q[2], b.q[3], b.s[0], b.s[1],
                    b.s[2]);
        }
    } else if (strcmp(verb, "skel") == 0) {
        // Session 20 muzzle probe: dump ANY actor's skeleton WITH bone names.
        // "skel hands" (default) = the AHands rig; "skel weapon" = the
        // equipped weapon's own skeleton, where the muzzle bone lives.
        bool wantWeapon = strncmp(rest, "weapon", 6) == 0;
        void* actor = wantWeapon ? hands::weapon_actor() : hands::hands_actor();
        if (!actor) {
            BVR_LOG("[bones] skel: no live %s actor (arm the drive; fire once for the "
                    "weapon)",
                    wantWeapon ? "weapon" : "hands");
            return;
        }
        Skel sk{};
        if (!resolve_skel(actor, sk)) {
            BVR_LOG("[bones] skel: %s actor %p has no SkeletonInstance at +0x%X (or the "
                    "vtable did not validate)",
                    wantWeapon ? "weapon" : "hands", actor, patterns::kActorSkelInstOffset);
            return;
        }
        const wchar_t* names[kMaxBones];
        int named = resolve_bone_names(sk, names, sk.count);
        BVR_LOG("[bones] skel %s: actor=%p inst=%p bones=%p count=%d (%d named)",
                wantWeapon ? "WEAPON" : "HANDS", actor, sk.inst, static_cast<void*>(sk.bones),
                sk.count, named);
        for (int i = 0; i < sk.count; ++i) {
            Qts b{};
            if (!read_n(&sk.bones[i], &b, sizeof b)) break;
            BVR_LOG("[bones]  %2d %-24S pos(%8.2f %8.2f %8.2f) quat(%6.3f %6.3f %6.3f %6.3f)",
                    i, names[i] ? names[i] : L"<unnamed>", b.p[0], b.p[1], b.p[2], b.q[0],
                    b.q[1], b.q[2], b.q[3]);
        }
    } else if (strcmp(verb, "poke") == 0) {
        int idx = -1;
        float d = 30.0f;
        if (sscanf_s(rest, "%d %f", &idx, &d) < 1 || !g_bones || idx < 0 ||
            idx >= g_boneCount) {
            BVR_LOG("[bones] usage: vrbones poke <idx> [dUU] (skeleton must be located; "
                    "freeze first or the engine re-evaluates over it)");
            return;
        }
        float z = 0.0f;
        if (read_n(&g_bones[idx].p[2], &z, 4)) {
            z += d;
            if (write_n(&g_bones[idx].p[2], &z, 4)) {
                set_dirty(0);
                BVR_LOG("[bones] poked bone %d z %+0.1f UU -> %.2f", idx, d, z);
            }
        }
    } else if (strcmp(verb, "freeze") == 0) {
        if (!g_skelInst) {
            BVR_LOG("[bones] no skeleton located yet");
            return;
        }
        int v = strncmp(rest, "on", 2) == 0 ? 1 : 0;
        write_n(static_cast<uint8_t*>(g_skelInst) + patterns::kSkelInstFreezeOffset, &v, 4);
        if (!v) set_dirty(1); // unfreeze: let the engine rebuild its own pose
        BVR_LOG("[bones] skeleton freeze %s", v ? "ON" : "off");
    } else if (strcmp(verb, "collapse") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_collapse.store(on, std::memory_order_relaxed);
        if (!on) set_dirty(1); // engine re-evaluation restores sleeve scales
        BVR_LOG("[bones] sleeve collapse %s", on ? "ON" : "off");
    } else if (strcmp(verb, "ref") == 0) {
        g_refValid = false;
        g_hasWritten[0] = g_hasWritten[1] = false;
        set_dirty(1);
        BVR_LOG("[bones] reference pose recapture queued (next engine evaluation)");
    } else if (strcmp(verb, "anchor") == 0) {
        int idx = -1;
        sscanf_s(rest, "%d", &idx);
        g_rAnchorOverride.store(idx, std::memory_order_relaxed);
        BVR_LOG("[bones] right anchor override = %d (-1 = default %d)", idx,
                patterns::kBoneWeaponAttach);
    } else if (strcmp(verb, "lock") == 0) {
        int mode = strncmp(rest, "off", 3) == 0    ? 0
                   : strncmp(rest, "diff", 4) == 0 ? 2
                                                   : 1; // on/abs
        g_renderLock.store(mode, std::memory_order_relaxed);
        BVR_LOG("[bones] render lock %s", mode == 0   ? "OFF"
                                          : mode == 2 ? "DIFF (head-split cancel only)"
                                                      : "ABS (anchor to true world pixel)");
    } else if (strcmp(verb, "lockgain") == 0) {
        float g = 0.5f;
        if (sscanf_s(rest, "%f", &g) == 1 && g >= 0.0f && g <= 2.0f) {
            g_lockGain.store(g, std::memory_order_relaxed);
            BVR_LOG("[bones] render lock lateral gain = %.2f", g);
        } else {
            BVR_LOG("[bones] usage: vrbones lockgain <0..2> (current %.2f)",
                    g_lockGain.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "lockpull") == 0) {
        float p = 65.0f;
        if (sscanf_s(rest, "%f", &p) == 1 && p >= 0.0f && p <= 200.0f) {
            g_lockPull.store(p, std::memory_order_relaxed);
            BVR_LOG("[bones] fg eye pull-back (matched lens) = %.1f UU", p);
        } else {
            BVR_LOG("[bones] usage: vrbones lockpull <0..200> (current %.1f)",
                    g_lockPull.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "lockdgain") == 0) {
        float g = 0.5f;
        if (sscanf_s(rest, "%f", &g) == 1 && g >= 0.0f && g <= 2.0f) {
            g_lockDepthGain.store(g, std::memory_order_relaxed);
            BVR_LOG("[bones] render lock depth gain = %.2f", g);
        } else {
            BVR_LOG("[bones] usage: vrbones lockdgain <0..2> (current %.2f)",
                    g_lockDepthGain.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "log") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_telemetry.store(on, std::memory_order_relaxed);
        BVR_LOG("[bones] telemetry %s%s", on ? "ON" : "off",
                on ? " - [tlm] lines at ~5 Hz (head/ctrl/cam/actor/target/bones)" : "");
    } else if (strcmp(verb, "scalemode") == 0) {
        int m = -1;
        if (sscanf_s(rest, "%d", &m) == 1 && m >= 0 && m <= 3) {
            g_scaleMode.store(m, std::memory_order_relaxed);
            // Bones the outgoing mode scaled and the incoming one does not get
            // their authored .s back on the next drive frame (the off-edge
            // branch in the write loop) - no bank write needed here.
            BVR_LOG("[bones] scale mode = %d (%s)", m,
                    m == 0   ? "cluster minus attach/muzzle .s - intended ship mode"
                    : m == 1 ? "fingers-only .s (wrist keeps authored)"
                    : m == 2 ? "wrist-only .s"
                             : "translation-only (no .s writes)");
        } else {
            BVR_LOG("[bones] usage: vrbones scalemode <0..3> (current %d)",
                    g_scaleMode.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "lcluster") == 0) {
        int lo = -1, hi = -1, an = -1;
        if (sscanf_s(rest, "%d %d %d", &lo, &hi, &an) == 3) {
            g_lFirst.store(lo, std::memory_order_relaxed);
            g_lLast.store(hi, std::memory_order_relaxed);
            g_lAnchor.store(an, std::memory_order_relaxed);
            BVR_LOG("[bones] left cluster = %d-%d anchor %d", lo, hi, an);
        } else {
            BVR_LOG("[bones] usage: vrbones lcluster <lo> <hi> <anchor>");
        }
    } else {
        BVR_LOG("[bones] unknown command '%s' (status|list|poke|freeze|collapse|ref|anchor|"
                "lcluster|scalemode|lock|lockgain|lockdgain|lockpull|log)",
                verb);
    }
}

void draw_debug_ui() {
    ImGui::Text("Bones: count %d writes %u hand %d", g_boneCount,
                g_writes.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed));
    bool col = g_collapse.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Hide the driven arm (collapse sleeve bones)", &col))
        g_collapse.store(col, std::memory_order_relaxed);
    bool hide = g_hideInactive.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Hide the inactive hand", &hide))
        set_hide_inactive(hide);
    int lockMode = g_renderLock.load(std::memory_order_relaxed);
    if (ImGui::RadioButton("lock off", &lockMode, 0) ||
        ImGui::RadioButton("lock ABS (true position)", &lockMode, 1) ||
        ImGui::RadioButton("lock DIFF (head-split cancel only)", &lockMode, 2))
        g_renderLock.store(lockMode, std::memory_order_relaxed);
    float gl = g_lockGain.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("lock lateral gain", &gl, 0.0f, 2.0f, "%.2f"))
        g_lockGain.store(gl, std::memory_order_relaxed);
    float gd = g_lockDepthGain.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("lock depth gain", &gd, 0.0f, 2.0f, "%.2f"))
        g_lockDepthGain.store(gd, std::memory_order_relaxed);
    ImGui::Text("lock |delta| %.1f UU, solves %u, skips %u",
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
    (void)g_status;
}

} // namespace bvr::b1r::bones
