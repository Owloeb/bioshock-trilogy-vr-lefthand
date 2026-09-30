#pragma once
// Route B probe (plasmid effects follow the real hand): hardware DATA
// watchpoints on the left-hand bones, to measure WHO writes and reads them and
// in what order within a frame - the engine's animation evaluation, the effects
// system, the renderer, and the mod's own drive.
//
// Four debug registers (DR0-DR3) on four left-cluster bones' pos.x, read/write,
// set on the game thread by a helper thread (a thread cannot reliably set its
// own debug registers). A vectored exception handler records every hit into a
// ring - EIP, the first game-exe return addresses on the stack, which bone -
// and never logs from inside the handler. Markers from the hands drive
// (drive begin/end) interleave the same ring, so the log shows the frame's
// order. Captures run 2 s, then the registers are cleared the same way.
//
// Two captures, like the effects census: baseline (not casting) and while
// casting; the log lists the read sites only the casting capture saw.
//
// Read-only by construction: nothing in the game is changed, and a hit costs
// one exception (~microseconds) only while a capture runs.

#include <cstdint>

namespace bvr::b1r::bonewatch {

// Game thread (hands drive): marks for the frame-order trace.
void mark_drive_begin();
void mark_drive_end();
// Game thread, once per frame: starts a requested capture, flushes results.
void tick(void* boneArrayBase, int boneCount);
// Overlay (any thread): request a capture. casting = the differential pass.
void request(bool casting);
// Effects probe (v9): watch four arbitrary addresses instead of the bones -
// the gun's and its attached effects' Location fields - to find the code that
// repositions attached effects each frame. labels name each slot in the log.
void request_custom(const uintptr_t addr[4], const char* const labels[4]);
void draw_debug_ui();

} // namespace bvr::b1r::bonewatch
