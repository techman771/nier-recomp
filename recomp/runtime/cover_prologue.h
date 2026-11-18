// Forced into the recompiled sources of a coverage build only (see NOTES.md, "Function coverage"):
// every guest function reports its name while coverage is on.
#include "ppc_recomp_shared.h"
extern "C" unsigned char gCoverOn;
extern "C" void CoverHit(const char* name);
#undef PPC_FUNC_PROLOGUE
#define PPC_FUNC_PROLOGUE() do { __builtin_assume(((size_t)base & 0x1F) == 0); if (__builtin_expect(gCoverOn, 0)) CoverHit(__func__); } while (0)
