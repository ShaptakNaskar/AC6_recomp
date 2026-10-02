#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace ac6::mouse_aim {

// Point-and-fly steering (steer_model = "aim"): the mouse moves an aim
// direction in the world and the aircraft flies itself towards it. Feeds the
// same virtual stick as the other mouse models: x = roll (right positive),
// y = pull (nose up positive), both in [-1, 1]. Returns false when the
// player's aircraft is not known, and the caller then leaves the stick alone.
// dx/dy are mouse deltas in pixels after the bindings' inversion (dy positive
// = aim up) and sensitivity.
bool Steer(double dx, double dy, double& out_x, double& out_y);

// Forgets the aim direction, so the next Steer() starts on the aircraft's nose.
void Reset();

// Virtual joystick: pointer offsets inside this radius (of full deflection)
// fly straight.
inline constexpr double kStickDeadzone = 0.08;

// Shows the virtual-joystick pointer (steer_model = "vector") at stick x/y,
// each in [-1, 1] (x right, y = pull = up on screen), for this frame.
void ShowStick(double x, double y);

// Registers the reticle overlay with the UI. Call once from OnCreateDialogs.
void CreateReticle(rex::ui::ImGuiDrawer* drawer);

}  // namespace ac6::mouse_aim
