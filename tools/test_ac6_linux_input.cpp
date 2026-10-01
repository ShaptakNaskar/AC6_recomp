#include "ac6_linux_input.h"

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <rex/cvar.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

REXCVAR_DEFINE_BOOL(ac6_kbm_enabled, true, "Test", "Enable host input test");

void Require(bool ok, const char* text) {
  if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(1); }
}
void Pump(int ms = 30, bool capture = false) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  do {
    if (capture) ac6::RequestLinuxMouseCapture(true);
    while (g_main_context_iteration(nullptr, FALSE)) {}
    g_usleep(1000);
  } while (std::chrono::steady_clock::now() < end);
}
int main(int argc, char** argv) {
  gtk_init(&argc, &argv);
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
  gtk_widget_show_all(window);
  Pump();
  GdkWindow* native = gtk_widget_get_window(window);
  Display* display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(window));
  const Window xid = gdk_x11_window_get_xid(native);
  GdkDeviceManager* manager = gdk_display_get_device_manager(gtk_widget_get_display(window));
  int major = 0, minor = 0;
  Require(GDK_IS_X11_DEVICE_MANAGER_XI2(manager), "GDK XI2 manager");
  g_object_get(manager, "major", &major, "minor", &minor, nullptr);
  Require(ac6::StartLinuxInput(window), "start native input");
  int after_major = 0, after_minor = 0;
  g_object_get(manager, "major", &after_major, "minor", &after_minor, nullptr);
  Require(after_major == major && after_minor == minor, "GDK protocol negotiation preserved");
  XSetInputFocus(display, xid, RevertToParent, CurrentTime); XFlush(display); Pump();
  Require(ac6::LinuxInputFocused(), "native focus gain");
  auto key = [&](KeySym sym, bool down) {
    XTestFakeKeyEvent(display, XKeysymToKeycode(display, sym), down, CurrentTime);
    XFlush(display); Pump();
  };
  key(XK_w, true); Require(ac6::LinuxKeyHeld('W'), "raw key press");
  key(XK_w, false); Require(!ac6::LinuxKeyHeld('W'), "raw key release");
  key(XK_Control_L, true);
  Require(ac6::LinuxKeyHeld(0xA2) && ac6::LinuxKeyHeld(0x11), "left/generic control");
  key(XK_Control_L, false);
  XTestFakeButtonEvent(display, 1, True, CurrentTime); XFlush(display); Pump();
  Require(ac6::LinuxKeyHeld(1), "left mouse press");
  XTestFakeButtonEvent(display, 1, False, CurrentTime); XFlush(display); Pump();
  Require(!ac6::LinuxKeyHeld(1), "left mouse release");
  XTestFakeButtonEvent(display, 4, True, CurrentTime);
  XTestFakeButtonEvent(display, 4, False, CurrentTime); XFlush(display); Pump();
  Require(ac6::LinuxKeyHeld(0x0E), "wheel up pulse");
  Pump(100); Require(!ac6::LinuxKeyHeld(0x0E), "wheel pulse expiry");
  Pump(30, true); Require(ac6::LinuxMouseCaptured(), "native pointer grab");
  // Both resets happen before GTK gets a tick; a new lease must reacquire.
  ac6::SetLinuxInputOverlay(true); ac6::SetLinuxInputOverlay(false);
  ac6::RequestLinuxMouseCapture(true); Pump(30, true);
  Require(ac6::LinuxMouseCaptured(), "rapid overlay reset reconciles native grab");
  double dx = 0, dy = 0;
  ac6::TakeLinuxMouseMotion(dx, dy);
  XTestFakeRelativeMotionEvent(display, 12, 7, CurrentTime); XFlush(display);
  Pump(30, true);
  Require(ac6::TakeLinuxMouseMotion(dx, dy) && dx > 0 && dy > 0, "raw relative motion");
  ac6::SetLinuxInputOverlay(true); Pump();
  Require(!ac6::LinuxMouseCaptured(), "overlay releases pointer");
  key(XK_w, true); ac6::SetLinuxInputOverlay(false);
  Require(!ac6::LinuxKeyHeld('W'), "overlay input does not leak on close");
  key(XK_w, false); key(XK_w, true);
  Pump(30, true);
  // Focus another client window: root focus would still route keys to the
  // window under the (grabbed) pointer, which is this one.
  GtkWidget* other = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_show_all(other);
  Pump();
  XSetInputFocus(display, gdk_x11_window_get_xid(gtk_widget_get_window(other)),
                 RevertToParent, CurrentTime);
  XFlush(display); Pump();
  Require(!ac6::LinuxInputFocused() && !ac6::LinuxMouseCaptured() && !ac6::LinuxKeyHeld('W'),
          "focus loss clears keys and pointer grab");
  XSetInputFocus(display, xid, RevertToParent, CurrentTime); XFlush(display); Pump();
  Require(ac6::LinuxInputFocused() && !ac6::LinuxKeyHeld('W'), "focus regain has no stuck key");
  key(XK_w, false);
  Pump(30, true);
  REXCVAR_SET(ac6_kbm_enabled, false); Pump();
  Require(!ac6::LinuxMouseCaptured(), "disable releases pointer");
  ac6::StopLinuxInput(); gtk_widget_destroy(window); Pump();
  std::cout << "Native XInput2 key/button/wheel/motion, capture, focus and overlay tests passed.\n";
}
