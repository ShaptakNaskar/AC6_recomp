// Single-player cheats, each a toggle under AC6/Cheats (F4 overlay or toml).
//
// Each hook sits on the guest instruction that stores a decremented counter
// and, while its cheat is on, adds the decrement back - the store then writes
// the value that was there before. Found with the developer scanner
// (ac6_mem_scanner.cpp) and a hardware write watchpoint on the counter:
//
//   player weapon object +6008 (0x1778)  standard missiles
//                        +6012 (0x177C)  special weapon
//
// The gun needs nothing: the game already gives it unlimited ammunition.

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ac6_cheat_infinite_missiles, false, "AC6/Cheats",
                    "Standard missiles are never used up");
REXCVAR_DEFINE_BOOL(ac6_cheat_infinite_special, false, "AC6/Cheats",
                    "Special weapon ammunition is never used up");

// 0x8222CAE4 (in rex_sub_8222C658): stw r10,6008(r31), r10 = count - 1.
void ac6InfiniteMissilesHook(PPCRegister& r10) {
  if (REXCVAR_GET(ac6_cheat_infinite_missiles)) {
    r10.s64 = r10.s64 + 1;
  }
}

// 0x82231DC8 (in rex_sub_82230DD8): stw r7,6012(r31), r7 = count - 1. Runs
// once per round of a multi-target salvo.
void ac6InfiniteSpecialHook(PPCRegister& r7) {
  if (REXCVAR_GET(ac6_cheat_infinite_special)) {
    r7.s64 = r7.s64 + 1;
  }
}
