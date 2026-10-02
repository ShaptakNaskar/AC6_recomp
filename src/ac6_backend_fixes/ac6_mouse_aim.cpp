// Point-and-fly mouse steering, War Thunder "mouse aim" style.
//
// The mouse moves an aim direction held in world space; a reticle marks it on
// screen, and each input poll turns the aircraft towards it with the classic
// mouse-flight controller: bank hard towards the aim while it is well off the
// nose, level the wings as it comes close, and pull to bring the nose on.
//
// Two things come from the game:
//   - the player's aircraft: `this` of its per-frame update (rex_sub_82233FB0,
//     a method of the player aircraft classes), captured by a hook;
//   - its orientation: a rotation matrix inside that object.
// The chase camera keeps the nose at the screen centre and banks with the
// aircraft, so the reticle is projected from the aircraft's own axes with an
// assumed vertical field of view (ac6_mouse_aim_fov).

#include "ac6_mouse_aim.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>

REXCVAR_DEFINE_DOUBLE(ac6_mouse_aim_fov, 55.0, "AC6/Enhancements",
                      "Mouse aim: the chase camera's vertical field of view in degrees, "
                      "used to place the reticle");
REXCVAR_DEFINE_DOUBLE(ac6_mouse_aim_gain, 5.0, "AC6/Enhancements",
                      "Mouse aim: how hard the aircraft turns towards the reticle");

namespace ac6::mouse_aim {
namespace {

// World matrix of the player's aircraft at +0x20: four float rows 16 bytes
// apart (vec4-padded), right-handed and Y up. Row 0 is right, row 1 up and
// row 3 the position; the aircraft flies along -row 2.
constexpr uint32_t kOffOrientation = 0x20;
constexpr uint32_t kStride = 16;

// Mouse pixels per radian of aim movement at sensitivity 1.
constexpr double kPixelsPerRadian = 900.0;
// The aim never goes closer than this to straight up or down.
constexpr double kMaxAimPitch = 1.48;  // ~85 degrees
// Beyond this angle off the nose, roll fully towards the aim; inside it,
// blend towards wings level.
constexpr double kAggressiveTurnAngle = 0.17;  // ~10 degrees
// The aim stays inside this fraction of the visible screen: the camera
// follows the aircraft, not the aim, so an aim off screen could not be seen.
constexpr double kScreenMargin = 0.9;

struct Vec3 {
  double x = 0, y = 0, z = 0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(Vec3 a) {
  const double len = std::sqrt(Dot(a, a));
  return len > 1e-9 ? a * (1.0 / len) : Vec3{0, 0, 1};
}
// Rodrigues rotation of v about unit axis k by angle a.
Vec3 Rotate(Vec3 v, Vec3 k, double a) {
  const double c = std::cos(a), s = std::sin(a);
  return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0 - c));
}

struct Orientation {
  Vec3 right, up, forward;
};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<uint32_t> g_player{0};
std::atomic<int64_t> g_player_seen_ms{0};

// Width / height of the display, published by the reticle for the aim limit.
std::atomic<double> g_aspect{16.0 / 9.0};

double TanHalfFov() {
  const double fov = std::clamp(REXCVAR_GET(ac6_mouse_aim_fov), 10.0, 150.0);
  return std::tan(fov * 3.14159265358979 / 360.0);
}

std::mutex g_mutex;
bool g_have_aim = false;
Vec3 g_aim;          // world-space unit vector
Vec3 g_aim_local;    // aim in aircraft axes (right, up, forward), for drawing
int64_t g_steer_ms = 0;
double g_stick_x = 0, g_stick_y = 0;  // virtual joystick, for drawing
int64_t g_stick_ms = 0;

bool ReadOrientation(Orientation& out) {
  const uint32_t player = g_player.load(std::memory_order_relaxed);
  if (!player || NowMs() - g_player_seen_ms.load(std::memory_order_relaxed) > 500) return false;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return false;
  const uint8_t* m = memory->TranslateVirtual<const uint8_t*>(player + kOffOrientation);
  auto row = [&](int r) {
    const uint8_t* p = m + r * kStride;
    return Vec3{rex::memory::load_and_swap<float>(p), rex::memory::load_and_swap<float>(p + 4),
                rex::memory::load_and_swap<float>(p + 8)};
  };
  out = {row(0), row(1), row(2) * -1.0};
  // A torn or uninitialised read must not steer the aircraft.
  for (const Vec3& v : {out.right, out.up, out.forward}) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
        std::fabs(Dot(v, v) - 1.0) > 0.01) {
      return false;
    }
  }
  return true;
}

class ReticleDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ReticleDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  // Draws on the foreground layer only, never a window: it must not count as
  // an overlay that owns the mouse, or steering would hand the pointer back.
  bool IsVisible() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.5f;
    if (cy > 0) g_aspect.store(double(cx) / cy, std::memory_order_relaxed);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 color = IM_COL32(120, 255, 140, 220);
    Vec3 local;
    double stick_x, stick_y;
    bool stick = false;
    {
      std::lock_guard lock(g_mutex);
      const int64_t now = NowMs();
      if (now - g_stick_ms <= 250) {
        stick = true;
        stick_x = g_stick_x;
        stick_y = g_stick_y;
      } else if (!g_have_aim || now - g_steer_ms > 250) {
        return;
      }
      local = g_aim_local;
    }
    if (stick) {
      // Ring of full deflection, the deadzone, and the pointer joined to the
      // centre.
      const float radius = cy * 0.35f;
      const ImVec2 centre(cx, cy);
      const ImVec2 pointer(cx + float(stick_x) * radius, cy - float(stick_y) * radius);
      draw->AddCircle(centre, radius, IM_COL32(120, 255, 140, 60), 64, 1.5f);
      draw->AddCircle(centre, radius * float(kStickDeadzone), IM_COL32(120, 255, 140, 120), 24,
                      1.5f);
      draw->AddLine(centre, pointer, IM_COL32(120, 255, 140, 110), 1.5f);
      draw->AddCircle(pointer, 10.0f, color, 24, 2.0f);
      return;
    }
    const double focal = cy / TanHalfFov();
    if (local.z > 0.05) {
      const float sx = float(cx + local.x / local.z * focal);
      const float sy = float(cy - local.y / local.z * focal);
      if (sx > -50 && sx < io.DisplaySize.x + 50 && sy > -50 && sy < io.DisplaySize.y + 50) {
        draw->AddCircle(ImVec2(sx, sy), 14.0f, color, 24, 2.0f);
        draw->AddCircleFilled(ImVec2(sx, sy), 2.5f, color);
        return;
      }
    }
    // Off screen or behind: an arrow at the edge pointing towards the aim.
    const double angle = std::atan2(-local.y, local.x);
    const float r = std::min(cx, cy) * 0.85f;
    const ImVec2 tip(cx + float(std::cos(angle)) * r, cy + float(std::sin(angle)) * r);
    const ImVec2 left(tip.x - float(std::cos(angle - 0.4)) * 22,
                      tip.y - float(std::sin(angle - 0.4)) * 22);
    const ImVec2 right(tip.x - float(std::cos(angle + 0.4)) * 22,
                       tip.y - float(std::sin(angle + 0.4)) * 22);
    draw->AddTriangleFilled(tip, left, right, color);
  }
};

}  // namespace

bool Steer(double dx, double dy, double& out_x, double& out_y) {
  Orientation o;
  if (!ReadOrientation(o)) return false;
  std::lock_guard lock(g_mutex);
  if (!g_have_aim) {
    g_aim = o.forward;
    g_have_aim = true;
  }
  // Horizontal mouse turns the aim about world up (+Y, the altitude axis);
  // vertical mouse raises or lowers it, clamped short of the poles.
  const Vec3 world_up{0, 1, 0};
  g_aim = Normalize(Rotate(g_aim, world_up, -dx / kPixelsPerRadian));
  const double pitch = std::asin(std::clamp(g_aim.y, -1.0, 1.0));
  const double new_pitch =
      std::clamp(pitch + dy / kPixelsPerRadian, -kMaxAimPitch, kMaxAimPitch);
  const Vec3 aim_right = Normalize(Cross(world_up, g_aim));
  g_aim = Normalize(Rotate(g_aim, aim_right, -(new_pitch - pitch)));

  Vec3 local{Dot(g_aim, o.right), Dot(g_aim, o.up), Dot(g_aim, o.forward)};
  // Keep the aim on screen: clamp its projection to the visible area and
  // move the world aim back onto that edge, so turning cannot carry it off.
  const double limit_y = TanHalfFov() * kScreenMargin;
  const double limit_x = limit_y * g_aspect.load(std::memory_order_relaxed);
  const double depth = std::max(local.z, 1e-3);
  const double tx = std::clamp(local.x / depth, -limit_x, limit_x);
  const double ty = std::clamp(local.y / depth, -limit_y, limit_y);
  if (local.z <= 1e-3 || tx != local.x / depth || ty != local.y / depth) {
    local = Normalize(Vec3{tx, ty, 1.0});
    g_aim = Normalize(o.right * local.x + o.up * local.y + o.forward * local.z);
  }
  g_aim_local = local;
  g_steer_ms = NowMs();

  const double gain = REXCVAR_GET(ac6_mouse_aim_gain);
  const double off_nose = std::acos(std::clamp(local.z, -1.0, 1.0));
  const double aggressive_roll = std::clamp(local.x * gain, -1.0, 1.0);
  const double wings_level_roll = std::clamp(o.right.y * gain, -1.0, 1.0);
  const double influence = std::clamp(off_nose / kAggressiveTurnAngle, 0.0, 1.0);
  out_x = wings_level_roll + (aggressive_roll - wings_level_roll) * influence;
  // Behind the aircraft, pull through rather than push over.
  out_y = local.z < 0 ? 1.0 : std::clamp(local.y * gain, -1.0, 1.0);
  return true;
}

void ShowStick(double x, double y) {
  std::lock_guard lock(g_mutex);
  g_stick_x = x;
  g_stick_y = y;
  g_stick_ms = NowMs();
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_have_aim = false;
}

void CreateReticle(rex::ui::ImGuiDrawer* drawer) {
  // The drawer owns registered dialogs for the life of the UI.
  new ReticleDialog(drawer);
}

}  // namespace ac6::mouse_aim

// 0x82233FB0: entry of the player aircraft's per-frame update; r3 = this.
void ac6PlayerUpdateHook(PPCRegister& r3) {
  using namespace ac6::mouse_aim;
  const uint32_t previous = g_player.exchange(r3.u32, std::memory_order_relaxed);
  g_player_seen_ms.store(NowMs(), std::memory_order_relaxed);
  if (previous != r3.u32) {
    REXLOG_WARN("[mouse aim] player aircraft object {:08X}", r3.u32);
  }
}
