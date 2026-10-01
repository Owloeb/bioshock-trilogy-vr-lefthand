#pragma once
// Viewmodel mirror probe (left-handed mode, BioShock 1 first).
//
// THE PROBLEM. The first-person rig is a RIGHT hand holding the weapon; the
// left-handed toggle moves it to the left controller but it is still a right
// hand. The developer's recorded dead end is mirroring at the SKELETON
// (negative bone scale): it trips the weapon-attach inverse-scale hazard and
// flips triangle winding.
//
// THIS ROUTE mirrors AFTER the engine has posed everything, at the constant
// buffer each foreground (viewmodel) draw uploads. That buffer carries the
// draw's full component->clip transform, so reflecting it reflects the
// finished rig with no engine state touched:
//
//   * The reflection plane is the HEAD'S vertical mid-plane, identical for both
//     eyes. Flipping each eye's image about its own centre would invert the
//     stereo disparity (a pseudoscopic, inside-out-in-depth gun). In an eye's
//     view space the head centre sits at x = c = -eyeSign * IPD/2, so the
//     reflection x' = 2c - x becomes, on the clip-x row of a symmetric
//     projection (clip.x = x_view / tanH):
//         rowX' = -rowX,  rowX'.w += 2c / tanH
//     A pure LEFT-multiply, so it stays correct on the engine's rigid path
//     where each section's matrix already has its bones baked in.
//   * A reflection flips winding, so the draw gets a rasterizer state with
//     FrontCounterClockwise toggled.
//   * The game side feeds the rig the controller poses reflected about the
//     same plane, so the reflected render lands on the real hands.
//
// FOREGROUND IDENTIFICATION. Under the shipped fg-fov match, fg constants carry
// the same lens as the world, so the buffer cannot tell the passes apart. The
// developer's lens-blind marker is the per-section fg-bake RETURN ADDRESSES on
// the draw's call stack (ENGINE_NOTES session 21) - the adapter supplies them
// and every candidate draw scans its stack for them.
//
// LIGHTING TIERS. Fg draws also use larger cb tiers whose transform does not
// sit at the base tier's offset. The probe DISCOVERS it: it looks for a block
// equal to a recent base-tier fg matrix and logs the offset it finds. Fg draws
// in a tier with no transform found are skipped by default (no unmirrored
// ghost), togglable.
//
// Disarmed cost: one relaxed atomic load per Map, Unmap and DrawIndexed.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11RasterizerState;

namespace bvr::vm_mirror {

struct TierSpec {
    uint32_t bytes; // cb0 ByteWidth
    int rowX;       // float index of the clip-x row, or -1 = discover
};

// Game adapter, once: fg-bake return-address RVAs (game exe) + cb tiers.
void configure(const uint32_t* fgRetRvas, int nRvas, const TierSpec* tiers, int nTiers);
bool configured();

// Game thread, per frame.
void set_armed(bool on);
bool armed();
void set_eye_half_uu(float halfIpdUu);
// UU per metre, for the arm's-reach filter that rejects world meshes which
// happen to go through the same bake code as the viewmodel.
void set_world_scale(float uuPerMetre);

// v5: the reflection plane, per eye, in that eye's view space as the draw's
// own clip transform reconstructs it (x right = clip.x*tanH, y up =
// clip.y*tanV, z forward = clip.w). Plane: n.X = d, n unit. eye 0 = left,
// 1 = right. The game publishes it every frame; invalid = draw unreflected.
void set_plane(int eye, const float n[3], float d, bool valid);
// Plane mode chosen in the overlay; the GAME side acts on it (it decides
// where the rig goes), the render side only applies the published plane.
enum class PlaneMode { Gun = 0, Head = 1 };
PlaneMode plane_mode();
float plane_shift_uu(); // overlay slider: lateral plane offset
void set_plane_shift_uu(float uu); // game side loads the per-weapon trim into it
void set_ui_note(const char* text); // a status line the game shows in the overlay
// Game-specific probe controls drawn under the mirror section.
void set_game_ui(void (*fn)());

// v6 effects census: record the stack signature of EVERY draw for a few
// seconds, first without casting (baseline), then while casting; the log lists
// the signatures only the second recording saw - the plasmid effect draws.
bool census_active();
void census_draw(ID3D11DeviceContext* ctx, void* esp, int kind, unsigned count);

// ---- frame_inspector hooks (render thread) ---------------------------------
bool wants_map_tracking();
void on_present();
void on_unmap(ID3D11Resource* res, const void* data, uint32_t bytes);
struct DrawToken {
    bool skip = false;                        // drop this draw
    bool restore = false;                     // restore rasterizer after the draw
    ID3D11RasterizerState* original = nullptr; // held ref, released in after_draw
};
DrawToken before_draw(ID3D11DeviceContext* ctx, void* esp);
void after_draw(ID3D11DeviceContext* ctx, DrawToken& t);

// Probe knobs + live counters (drawn under the "Mirror viewmodel" checkbox).
void draw_debug_ui();

} // namespace bvr::vm_mirror
