#pragma once
// EVE hypo holster + physical injection (BioShock 1) - stage 1: the PROBE.
//
// The feature (pull a hypo from the hip, put the needle in the plasmid arm,
// pull the trigger) needs four engine facts nobody has measured yet:
//   1. what the game draws during an EVE injection - is the syringe its own
//      actor / holdable, attached to which hand, with which bones;
//   2. which of Jack's hands holds it and where the jab lands;
//   3. where the EVE level and the hypo count live, and how a cast, an
//      injection (X with a plasmid raised), an automatic injection and a
//      pickup change them;
//   4. how long the injection runs and when the EVE arrives within it.
// The probe watches for exactly that and writes it to bioshockvr.log
// ("[eve]" lines). It is read-only: nothing in the game is changed. F10 ->
// Developer tools -> "EVE probe".

namespace bvr::b1r::eve {

// Game thread, once per frame from hands::on_calcview.
void tick();
// From the attach-update hook (game thread): parent gained/updated child.
void on_attach(void* parent, void* child);
// The probe is running (the attach hook installs itself while it is).
bool probe_on();
// F10 section (render thread).
void draw_debug_ui();

} // namespace bvr::b1r::eve
