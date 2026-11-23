// The VU1 tools link the runtime library but not the recompiled game code, whose function table
// the runtime refers to. An empty table stands in for it.
#include "ps2_runtime.h"

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};
