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

// What the holster needs from the drawn rig, in ENGINE world space - where
// the syringe must be placed for the viewmodel mirror to show it on the hand
// you see.
struct Targets {
    bool socketOk = false;      // the weapon hand's gun socket ("Pistol")
    float socket[3];
    float sockF[3], sockU[3];   // its forward / up
    bool wristOk = false;       // the plasmid hand's wrist
    float wrist[3];
    bool elbowOk = false;       // the plasmid elbow (arms on)
    float elbow[3];
    float worldScale = 50.0f;   // UU per metre
};
// Game thread, once per frame from hands after the rig is drawn.
void set_targets(const Targets& t);
// The hands-bone index of the syringe's socket, once found (-1 before).
int socket_bone();
// F10 section (render thread).
void draw_debug_ui();

} // namespace bvr::b1r::eve
