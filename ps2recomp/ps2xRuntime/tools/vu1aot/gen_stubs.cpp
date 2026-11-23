// vu1aot_gen writes the compiled VU1 programs, so it links without them: this empty table keeps
// the library's vu1_aot_programs objects out of the tool, and a stale or half-written set of
// generated files can never stop the generator itself from being rebuilt.
#include "../../src/lib/vu/ps2_vu_fast_inl.h"

extern const Vu1AotProgram g_vu1AotPrograms[] = {{0ull, nullptr, 0u, 0u}};
extern const size_t g_vu1AotProgramCount = 0;
