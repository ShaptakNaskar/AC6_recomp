// Single-player cheats, each a toggle under AC6/Cheats (F4 overlay or toml).
//
// Each hook sits on the guest instruction that stores a decremented counter
// and, while its cheat is on, adds the decrement back - the store then writes
// the value that was there before. Found with the developer scanner
// (ac6_mem_scanner.cpp) and a hardware write watchpoint on the counter:
//
//   player aircraft object +272  (0x110)   health (float, max at +276)
//                          +6008 (0x1778)  standard missiles
//                          +6012 (0x177C)  special weapon
//
// The gun needs nothing: the game already gives it unlimited ammunition.

#include <rex/cvar.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(ac6_cheat_infinite_missiles, false, "AC6/Cheats",
                    "Standard missiles are never used up");
REXCVAR_DEFINE_BOOL(ac6_cheat_infinite_special, false, "AC6/Cheats",
                    "Special weapon ammunition is never used up");
REXCVAR_DEFINE_BOOL(ac6_cheat_no_damage, false, "AC6/Cheats",
                    "The player's aircraft takes no damage");

namespace {
constexpr uint32_t kOffHealth = 272;

// Set while the player's damage override (rex_sub_8222B750) is inside its
// call to the shared damage handler: the aircraft being damaged then. Enemy
// classes call the shared handler straight from their vtables, and vtables
// themselves vary between missions, so the call site is what marks the player.
thread_local uint32_t t_player_being_damaged = 0;
}  // namespace

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

// 0x8222B818 / 0x8222B81C: around `bl 0x8229BC98` in the player override.
void ac6PlayerDamageEnterHook(PPCRegister& r31) { t_player_being_damaged = r31.u32; }
void ac6PlayerDamageExitHook(PPCRegister&) { t_player_being_damaged = 0; }

// 0x8229C024 (in rex_sub_8229BC98, the damage handler for every aircraft):
// the join point after all of its health stores - weapon damage, collision,
// scripted kills and resets - and before its death check. f28 holds the
// health loaded on entry, on every path that reaches here. Undo any drop for
// the player; heals still apply.
void ac6NoDamageHook(PPCRegister& r31, PPCRegister& f28) {
  if (!REXCVAR_GET(ac6_cheat_no_damage)) return;
  if (!r31.u32 || r31.u32 != t_player_being_damaged) return;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  uint8_t* health = memory->TranslateVirtual<uint8_t*>(r31.u32 + kOffHealth);
  const float before = static_cast<float>(f28.f64);
  if (rex::memory::load_and_swap<float>(health) < before) {
    rex::memory::store_and_swap<float>(health, before);
  }
}
