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
//   static 0x829F13E8                        hangar credits (u32)
//
// The gun needs nothing: the game already gives it unlimited ammunition.

#include <algorithm>

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
REXCVAR_DEFINE_BOOL(ac6_cheat_rapid_fire, false, "AC6/Cheats",
                    "Missiles and special weapons reload ten times faster");
REXCVAR_DEFINE_BOOL(ac6_cheat_unlimited_credits, false, "AC6/Cheats",
                    "Purchases cost nothing; buying anything tops credits up to 9,999,999");

namespace {
constexpr uint32_t kOffHealth = 272;
constexpr double kRapidFireReloadScale = 0.1;
// Fits the hangar's credit display; checked in game.
constexpr uint32_t kCreditsTopUp = 9'999'999;

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

// rex_sub_82230DD8 stores the decremented special count on two paths:
// 0x82231DC8 stw r7,6012(r31) once per round of a multi-target salvo, and
// 0x822316A8 stw r9,6012(r31) for single-launch weapons such as the ADMM.
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

// 0x8214D0D8 (in sub_8214CFA0, the hangar purchase): stw r9,4(r11) with
// r8 = credits and r9 = credits - price. Keep the balance and raise it, so a
// fresh save only needs one cheap purchase before everything is affordable.
void ac6UnlimitedCreditsHook(PPCRegister& r8, PPCRegister& r9) {
  if (REXCVAR_GET(ac6_cheat_unlimited_credits)) {
    r9.u64 = std::max(r8.u32, kCreditsTopUp);
  }
}

// 0x8222CABC (in rex_sub_8222C658): stfsx f0,r11,r31 sets the fired missile
// launcher's timer to the aircraft's reload time (+6064).
void ac6RapidFireMissileHook(PPCRegister& f0) {
  if (REXCVAR_GET(ac6_cheat_rapid_fire)) f0.f64 *= kRapidFireReloadScale;
}

// 0x82232288 (in rex_sub_82230DD8): fnmsubs f0,f13,f28,f0 counts every
// special-weapon launcher's reload timer down, timer -= rate * frame time,
// with f13 = the launcher's rate. The timer's start value is stored from
// about twenty places (one per weapon type and launcher layout), so speed
// up the shared countdown instead.
void ac6RapidFireSpecialHook(PPCRegister& f13) {
  if (REXCVAR_GET(ac6_cheat_rapid_fire)) f13.f64 /= kRapidFireReloadScale;
}
