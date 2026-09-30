#pragma once
// M7-v2 bone drive: place the viewmodel's HAND CLUSTER at the controller by
// writing the engine's own evaluated skeleton, so everything the engine
// derives from bones - the attached weapon's render transform above all -
// follows for free (the where-you-write principle, ENGINE_NOTES).
//
// Mechanism (all offsets in patterns.h, derivations in ENGINE_NOTES):
// the AHands actor carries a `SkeletonInstance` at +0x3FC whose bone array is
// component-space hkQsTransforms. The engine re-evaluates it lazily behind a
// dirty flag; our write lands from the CalcView detour - after the engine's
// tick, before the render build - and was live-proven to be what that frame
// renders. The whole hand cluster (wrist + fingers + weapon-attach bone) is
// moved RIGIDLY: rotate the reference pose about the anchor bone's reference
// point, then translate the anchor point onto the target. No IK, by design -
// the user dropped arm articulation, which is exactly what makes this shape
// of drive sufficient.
//
// This module owns the skeleton MECHANISM only. Pose POLICY (which XR pose,
// trims, offsets, enable state, hand choice) stays in hands.cpp, which calls
// drive() with the finished game-space target - the same GamePose the actor
// pinning used, so `vrhands test`/`simpose` flat lanes exercise this path
// unchanged.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::bones {

void init(const bvr::pattern_scan::ProcessImage& image);

// Move `hand`'s cluster (0 left, 1 right) so its anchor bone sits at `gp`.
// `handsActor` is the live AHands actor (validated by the caller). Game
// thread, once per frame from hands::on_calcview. Returns false if the
// skeleton could not be reached this frame (caller may fall back).
bool drive(const FrameContext& ctx, void* handsActor, const GamePose& gp, int hand);

// Re-write the values the last drive() produced. The stereo second pass runs
// the ENGINE's CalcView again (which re-evaluates the skeleton over our
// write) but deliberately skips the drive body - without this, the right eye
// bakes the engine pose while the left bakes ours (live-proven: under flat
// stereo the bone array read back the engine idle pose every frame despite
// the drive writing). Called from the CalcView detour's second-pass branch,
// after the original returns. Cheap (cached memcpy), safe when nothing was
// written this frame.
void reapply();

// Always-visible off hand (BioVR port): where the cluster drive() does NOT own
// goes on the next drive() - and on every Route B re-drive until changed.
// track=false (or gp null) restores the old behaviour: the off cluster is
// collapsed by hide-inactive. Tracking needs hide-inactive on (it replaces the
// collapse). Game thread, before drive().
void set_off_target(bool track, const GamePose* gp);
// While the off hand holds the gun (two-handed grip), also carry the engine's
// own off-hand motion relative to the gun (shotgun pump, chemical thrower
// crank) onto it. Game thread, before drive().
void set_off_follow(bool on);

// Grip-pose placement: given where the OpenXR GRIP pose is (game space, the
// quaternion in the skeleton convention - ue_rot_to_quat), the drive target
// (anchor location + frame) that puts Jack's measured palm frame exactly on
// it, for the pose `hand` will be drawn with (driven = the raised hand).
// palmDepthUu pushes the palm centroid toward the palm surface. False until
// the rig has a reference pose.
bool grip_to_anchor(int hand, bool driven, const float gripLoc[3], const float gripQ[4],
                    float palmDepthUu, float outLoc[3], float outQ[4]);
// The weapon now in hand (class key). A change drops the weapon hand's rest
// relation, so the new gun is placed live until it settles. Game thread,
// before grip_to_anchor for the driven weapon hand.
void set_active_weapon(const char* key);
// Developer tools: log one summary per weapon animation (engine off hand, gun
// tilt, the weapon's own moving bones) to bioshockvr.log.
void set_anim_log(bool on);
// World position of a bone the last drive wrote (e.g. 43/44: the gun barrel).
bool written_world(int idx, float out[3]);
// The gun's barrel direction in the drive target's frame: its idle forward,
// learned at rest in the attach bone's frame and carried through animations.
// Falls back to 43->44 before the first rest.
// yaw/pitch (deg, right+/up+, un-mirrored rig frame) = that weapon's own barrel
// angle in its idle pose.
bool barrel_dir_target(float yawDeg, float pitchDeg, float out[3]);
// Jack's palm centroid relative to the anchor, in the drive target's frame,
// for the pose `hand` is drawn with (same solve as grip_to_anchor).
bool palm_in_target(int hand, bool driven, float palmDepthUu, float out[3]);

// Arms (two-bone IK, experimental): instead of collapsing a visible hand's
// sleeve, pose clavicle / upper arm / elbow / twist helpers from a shoulder
// point to the written wrist. Targets are in ENGINE world space (already
// mirrored when the viewmodel mirror is on) and persist until changed; pole =
// the direction the elbow bends toward. Game thread, before drive().
void set_arm_target(int hand, bool valid, const float shoulderW[3], const float poleW[3],
                    const float outW[3]);
void set_arms(bool on);
void set_arm_length(float mult); // 1.0 = Jack's own arm (reference pose)
float arm_length();
bool arms();
void set_arm_scale_skin(bool on); // scale the arm skin like the hands (default on)
bool arm_scale_skin();
void arm_stats(unsigned* solves, unsigned* stretched);
// The wrist's roll about the forearm the helpers carry, after the elbow lift
// (deg; readout).
float arm_twist_deg(int hand);
// Draw the off hand in its grip shape without following (grab-point recording,
// grab-zone preview). Game thread, before drive().
void set_off_preview(bool on);

// The old actors died with the old world; drop every cached pointer.
void on_world_change();

// Session 29: hand the skeleton back to the engine, deliberately.
//
// Stopping the drive is NOT enough to restore an authored animation, and that
// asymmetry is the suspected cause of "the controllable rig hands instead of
// the cinematic ones". Three pieces of our state outlive the last drive() call:
// reapply() keeps repainting the cached pose for up to 100 ms AND keeps
// clearing the dirty flag while it does (so it actively suppresses the engine
// re-evaluation that would undo us); restore_hidden() is only ever called from
// inside drive(), so a collapsed inactive hand stays collapsed and the
// weapon-attach bone stays parked far below; and the frozen sway reference
// survives, so the first frame after the cutscene rebuilds from a pre-cutscene
// pose. release() undoes all three and sets the dirty flag so the engine takes
// the skeleton back on its next evaluation. Idempotent; game thread.
void release(const char* why);

// Snapshot for the session-29 cinematic-edge instrument: what our drive left
// behind at the moment a cutscene started or ended. Game thread.
void debug_state(int* hiddenHand, unsigned long long* cacheAgeMs, bool* refValid);

// Session 19: collapse the whole INACTIVE hand's cluster + sleeve while the
// other hand drives (default ON; `vrhands hideinactive on|off`). The
// weapon-attach bone hides by translation, never scale - the attach path
// inverse-decomposes chain scale (session 16). Restores from the reference
// on toggle-off and on hand switch, before the incoming hand is driven.
void set_hide_inactive(bool on);
bool hide_inactive();

// Session 61: per-cluster viewmodel scale (1.0 = authored; `vrhands scale`).
// The cluster's anchor-relative translations shrink by s (scale about the
// anchor - the anchor write-loc is unchanged, which is the proof metric) and
// the hkQsTransform .s channel is written for the probe-mode-selected bones
// only, NEVER for the weapon-attach (43) or muzzle (44) bones - the s16
// "cluster scale blows the weapon up" test always wrote 43's .s; leaving the
// channel engine-owned entirely is the cell it never tested (BS2's bisection
// localised its identical blowup to the pivot's own scale channel). hand -1 =
// both. Probe modes: `vrbones scalemode <0..3>`.
void set_scale(int hand, float s);
float scale(int hand);

// Session 61: uniform weapon scale (`vrhands wscale <f>`; 1.0 = authored) -
// drives the equipped holdable's OWN SkeletonInstance: translations
// uniformly about the component origin (the grip), quats adopted per frame
// (weapon animations keep playing while scaled), scale channel pinned to the
// captured reference. At 1.0 the lane drops the skeleton entirely and
// restores the captured pose. Runs from hands::on_calcview via
// wskel_drive(); wskel_release() is the explicit hand-back (weapon switch
// and world change are handled internally).
void set_weapon_scale(float ws);
float weapon_scale();
void wskel_drive();
void wskel_release(const char* why);

// Session 20: freeze the drive's reference against the idle animation's
// breathing (default ON; `vrhands swaykill on|off`). Real animations pass an
// anchor-delta threshold and re-freeze when they settle. Measured baseline:
// +-1.2 deg of barrel-direction wobble at idle without it.
void set_sway_kill(bool on);
bool sway_kill();

// Seam commands (game thread): status | list [n] | skel [hands|weapon] |
// poke <idx> <dUU> |
// freeze on|off | collapse on|off | ref | anchor <idx> | lcluster <lo> <hi> <anchor> |
// log on|off (in-headset telemetry: head/controller/camera/actor/target/bone
// samples at ~5 Hz so a headset session can be diagnosed from the log)
void handle_command(const char* args);

// True while `vrbones log on` - camera.cpp and hands.cpp contribute their
// raw-pose lines to the same telemetry stream (each site throttles itself to
// ~5 Hz; the log timestamps correlate the lines of one sample).
bool telemetry_on();

// |render-lock position delta| applied last frame, UU. POSITION-only by
// construction (the lock never touches rotation) - `vraim synccheck` quotes it
// so the position story stays separate from the rotation-divergence gate.
float lock_delta_mag();

// Session 20 muzzle ray: the rendered barrel axis (hands-rig bones 43->44 in
// the current reference pose), expressed in the drive target's local frame -
// world barrel dir = target rotation (x) d0 (see the impl derivation). False
// until a reference pose exists. Game thread.
bool barrel_ref_axis(float d0[3]);
// Mirror probe v6: bone 44 (the per-weapon muzzle-ish tip) relative to the
// weapon-attach anchor, in the drive target's local frame (UE fwd/right/up),
// scaled like the drive scales cluster translations. Game thread.
bool muzzle_ref_offset(float out[3]);
// Route B probe: the live hands bone array (hkQsTransform[count]) or null.
void* bone_array(int* count);
// The hands SkeletonInstance the drive is bound to (identity for Route B).
void* skeleton_instance();

// Overlay section (render thread only).
void draw_debug_ui();

} // namespace bvr::b1r::bones
