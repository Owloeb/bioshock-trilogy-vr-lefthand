#pragma once
// Always-visible off hand + two-handed grip (the BioVR M6-S1/S2 port, rebuilt
// for this mod's role-swapped, mirror-capable rig).
//
// OFF HAND. BioShock draws one raised hand; the mod used to collapse the
// other. With this on, the other cluster tracks its own controller every frame
// (bones::set_off_target), so both of your hands are always there.
//
// TWO-HANDED GRIP. Every weapon gets ONE recorded grab point: in F10 press
// "Set grab point", hold the gun, put your off hand where the grip should be
// and squeeze the off-hand grip. The spot (and how that hand was turned) is
// stored in the weapon controller's own frame, per weapon and per hand setup,
// in twohand.ini. After that, bringing the off hand within the grab radius
// buzzes it; squeeze to take hold. While held:
//   - the weapon aims along rear hand -> front hand (the recorded point is
//     kept on that line), and
//   - the off hand sits exactly on the recorded spot of the gun.
// Both happen at the XR pose funnel (vr::set_two_hand_grip), so the rig, the
// fire ray, the laser, the mirror and the swing detector all agree without
// knowing the feature exists. Let go of the grip to release.
//
// The off-hand grip normally raises the plasmid hand (it composes to a
// bumper). Inside the grab zone, while holding, and while a capture is armed,
// that bumper is reserved - so reaching for the fore-end cannot switch you to
// plasmids. Outside the zone it works exactly as before.

namespace bvr::b1r::twohand {

// Game thread, once per frame from hands::on_calcview, BEFORE any hand pose
// is read (so the two-handed poses apply to this frame). weaponRaised = the
// weapon role owns the viewmodel; gameplay = normal play (not a cutscene).
void tick(bool weaponRaised, bool gameplay);

bool off_hand_enabled(); // F10 setting: draw the off hand at its controller
bool gripped();          // two-handing right now
// Show the off hand in its GRIP shape rather than the relaxed one: while a
// grab point is being recorded (so you can see how the hand will sit) and
// while the hand is inside the grab zone (a preview of the hold).
bool preview_grip();

// F10 section (render thread).
void draw_debug_ui();

} // namespace bvr::b1r::twohand
