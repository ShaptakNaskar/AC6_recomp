#include "ac6_host_input_state.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
  ac6::HostInputState state;
  double dx = 1, dy = 1;
  state.Key('W', true);
  assert(!state.Held('W', 0));
  state.SetFocus(true);
  state.Key('W', true);
  state.Key(0xA2, true);
  assert(state.Held('W', 0) && state.Held(0xA2, 0) && state.Held(0x11, 0));
  assert(!state.Held(0xA3, 0));
  state.Key(0xA2, false);
  assert(!state.Held(0x11, 0));
  state.Wheel(true, 10);
  assert(state.Held(0x0E, 99) && !state.Held(0x0E, 100));
  state.RequestCapture(true, 10);
  assert(state.WantsCapture(109) && !state.WantsCapture(110));
  state.Motion(8, 9);
  assert(!state.TakeMotion(dx, dy) && dx == 0 && dy == 0);
  state.SetCaptured(true);
  state.Motion(10, -3); state.Motion(2, 1);
  assert(state.TakeMotion(dx, dy) && dx == 12 && dy == -2);
  assert(state.TakeMotion(dx, dy) && dx == 0 && dy == 0);
  state.Motion(std::numeric_limits<double>::infinity(), 1);
  assert(state.TakeMotion(dx, dy) && dx == 0 && dy == 0);
  state.Motion(50, 50);
  state.SetOverlay(true);
  assert(!state.Captured() && !state.WantsCapture(10) && !state.Held('W', 10));
  state.Key('W', true); state.Wheel(true, 10);
  state.RequestCapture(true, 10);
  state.SetOverlay(false);
  assert(!state.Held('W', 10) && !state.Held(0x0E, 10) && !state.WantsCapture(10));
  assert(!state.TakeMotion(dx, dy) && dx == 0 && dy == 0);
  state.Key('W', true); state.SetCaptured(true); state.Motion(1, 1);
  state.SetFocus(false);
  assert(!state.Held('W', 10) && !state.Captured());
  state.SetFocus(true);
  assert(!state.Held('W', 10) && !state.TakeMotion(dx, dy) && dx == 0 && dy == 0);
  state.Key(999, true);
  assert(!state.Held(999, 0));
  std::cout << "Host input focus/overlay, modifiers, wheel, capture and motion tests passed.\n";
}
