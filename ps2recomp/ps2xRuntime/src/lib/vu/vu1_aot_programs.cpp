// Ahead-of-time compiled VU1 microprograms: none in this repository.
//
// The real table is generated from the game's own VU1 code, so it is not published here. To make
// it from your copy of the game: run the game once with PS2X_VU1_CAPTURE=1 (it records each VU1
// code image it sees), then run tools/vu1aot/vu1aot_gen on those recordings and put its output in
// place of this file (see port/BUILDING.md). Without it every VU1 program runs on the fast
// interpreter core: same results, slower in big battles.

#include "ps2_vu_fast_inl.h"

extern const Vu1AotProgram g_vu1AotPrograms[] = {{0ull, nullptr, 0u, 0u}};
extern const size_t g_vu1AotProgramCount = 0;
