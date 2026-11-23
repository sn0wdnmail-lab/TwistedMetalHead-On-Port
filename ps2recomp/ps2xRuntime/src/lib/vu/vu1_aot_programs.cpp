// Ahead-of-time compiled VU1 and VU0 microprograms: none in this repository.
//
// The real table is generated from the game's own VU microcode, so it is not published here. To
// make it from your copy of the game: run the game once with PS2X_VU1_CAPTURE=1 and
// PS2X_VU0_CAPTURE=1 (it records each VU code image it sees), then run tools/vu1aot/vu1aot_gen on
// those recordings. It writes this file plus vu1_aot_programs_p0.cpp .. _p7.cpp next to it (see
// port/BUILDING.md). Without it every VU program runs on the fast interpreter core: same results,
// slower in big battles.

#include "ps2_vu_fast_inl.h"

extern const Vu1AotProgram g_vu1AotPrograms[] = {{0ull, nullptr, 0u, 0u}};
extern const size_t g_vu1AotProgramCount = 0;
