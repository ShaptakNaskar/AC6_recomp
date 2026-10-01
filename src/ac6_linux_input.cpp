#include "ac6_linux_input.h"
#include "ac6_host_input_state.h"

#include <chrono>
#include <algorithm>
#include <vector>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, ac6_kbm_enabled);

namespace ac6 {
namespace {
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

unsigned KeyFromSym(KeySym sym) {
  if (sym >= XK_a && sym <= XK_z) return unsigned(sym - XK_a + 'A');
  if (sym >= XK_A && sym <= XK_Z) return unsigned(sym);
  if (sym >= XK_0 && sym <= XK_9) return unsigned(sym);
  if (sym >= XK_F1 && sym <= XK_F24) return unsigned(sym - XK_F1 + 0x70);
  if (sym >= XK_KP_0 && sym <= XK_KP_9) return unsigned(sym - XK_KP_0 + 0x60);
  switch (sym) {
    case XK_BackSpace: return 0x08; case XK_Tab: return 0x09;
    case XK_Return: case XK_KP_Enter: return 0x0D;
    case XK_Escape: return 0x1B; case XK_space: return 0x20;
    case XK_Page_Up: return 0x21; case XK_Page_Down: return 0x22;
    case XK_End: return 0x23; case XK_Home: return 0x24;
    case XK_Left: return 0x25; case XK_Up: return 0x26;
    case XK_Right: return 0x27; case XK_Down: return 0x28;
    case XK_Insert: return 0x2D; case XK_Delete: return 0x2E;
    case XK_Shift_L: return 0xA0; case XK_Shift_R: return 0xA1;
    case XK_Control_L: return 0xA2; case XK_Control_R: return 0xA3;
    case XK_Alt_L: case XK_Meta_L: return 0xA4;
    case XK_Alt_R: case XK_Meta_R: case XK_ISO_Level3_Shift: return 0xA5;
    case XK_Super_L: return 0x5B; case XK_Super_R: return 0x5C;
    case XK_Caps_Lock: return 0x14; case XK_Num_Lock: return 0x90;
    case XK_Scroll_Lock: return 0x91; case XK_Pause: return 0x13;
    case XK_Print: return 0x2C; case XK_Menu: return 0x5D;
    case XK_KP_Insert: return 0x60; case XK_KP_End: return 0x61;
    case XK_KP_Down: return 0x62; case XK_KP_Page_Down: return 0x63;
    case XK_KP_Left: return 0x64; case XK_KP_Begin: return 0x65;
    case XK_KP_Right: return 0x66; case XK_KP_Home: return 0x67;
    case XK_KP_Up: return 0x68; case XK_KP_Page_Up: return 0x69;
    case XK_KP_Multiply: return 0x6A; case XK_KP_Add: return 0x6B;
    case XK_KP_Subtract: return 0x6D;
    case XK_KP_Decimal: case XK_KP_Delete: return 0x6E;
    case XK_KP_Divide: return 0x6F;
    case XK_semicolon: return 0xBA; case XK_equal: return 0xBB;
    case XK_comma: return 0xBC; case XK_minus: return 0xBD;
    case XK_period: return 0xBE; case XK_slash: return 0xBF;
    case XK_grave: return 0xC0; case XK_bracketleft: return 0xDB;
    case XK_backslash: return 0xDC; case XK_bracketright: return 0xDD;
    case XK_apostrophe: return 0xDE;
    default: return 0;
  }
}

class LinuxInput {
 public:
  HostInputState state;
  bool Start(GtkWidget* window) {
    Stop();
    state.SetOverlay(false);
    GdkDisplay* gd = gtk_widget_get_display(window);
    if (!GDK_IS_X11_DISPLAY(gd)) return false;
    display_ = gdk_x11_display_get_xdisplay(gd);
    // GTK already negotiated XI on this shared connection. Repeating a lower
    // XIQueryVersion request can raise BadValue, so reuse GDK's negotiated
    // protocol and opcode rather than changing the display's version.
    GdkDeviceManager* manager = gdk_display_get_device_manager(gd);
    int major = 0, minor = 0;
    if (GDK_IS_X11_DEVICE_MANAGER_XI2(manager))
      g_object_get(manager, "major", &major, "minor", &minor, "opcode", &opcode_, nullptr);
    if (major < 2 || (major == 2 && minor < 1)) {
      display_ = nullptr;
      REXLOG_WARN("[KBM] Linux input requires XInput 2.1 or later");
      return false;
    }
    window_ = window;
    root_ = DefaultRootWindow(display_);
    // Preserve GDK's selections on this connection rather than replacing them.
    int count = 0;
    XIEventMask* selected = XIGetSelectedEvents(display_, root_, &count);
    for (int i = 0; i < count; ++i) {
      if (selected[i].deviceid == XIAllMasterDevices)
        previous_mask_.assign(selected[i].mask, selected[i].mask + selected[i].mask_len);
    }
    if (selected) XFree(selected);
    auto mask = previous_mask_;
    mask.resize(std::max(mask.size(), size_t(XIMaskLen(XI_LASTEVENT))), 0);
    for (int type : {XI_RawMotion, XI_RawButtonPress, XI_RawButtonRelease,
                     XI_RawKeyPress, XI_RawKeyRelease}) XISetMask(mask.data(), type);
    XIEventMask selection{XIAllMasterDevices, int(mask.size()), mask.data()};
    XISelectEvents(display_, root_, &selection, 1);
    gdk_window_add_filter(nullptr, Filter, this);
    focus_handler_ = g_signal_connect(window_, "notify::has-toplevel-focus",
                                      G_CALLBACK(FocusChanged), this);
    destroy_handler_ = g_signal_connect(window_, "destroy", G_CALLBACK(Destroyed), this);
    state.SetFocus(gtk_window_has_toplevel_focus(GTK_WINDOW(window_)));
    timer_ = g_timeout_add(8, Tick, this);
    REXLOG_INFO("[KBM] Linux XInput2 keyboard and relative mouse input initialized");
    return true;
  }
  void Stop() {
    state.SetFocus(false);
    if (timer_) { g_source_remove(timer_); timer_ = 0; }
    Release();
    if (window_) {
      g_signal_handler_disconnect(window_, focus_handler_);
      g_signal_handler_disconnect(window_, destroy_handler_);
      gdk_window_remove_filter(nullptr, Filter, this);
      XIEventMask selection{XIAllMasterDevices, int(previous_mask_.size()), previous_mask_.data()};
      XISelectEvents(display_, root_, &selection, 1);
    }
    window_ = nullptr;
    display_ = nullptr;
    previous_mask_.clear();
  }

 private:
  static void FocusChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxInput*>(data);
    self.state.SetFocus(gtk_window_has_toplevel_focus(GTK_WINDOW(self.window_)));
    if (!self.state.Focused()) self.Release();
  }
  static void Destroyed(GtkWidget*, gpointer data) { static_cast<LinuxInput*>(data)->Stop(); }
  void Release() {
    if (grabbed_) {
      gdk_seat_ungrab(seat_);
      // Do not move the pointer in another app after Alt-Tab / focus loss.
      if (state.Focused()) gdk_device_warp(pointer_, screen_, saved_x_, saved_y_);
      grabbed_ = false;
    }
    state.SetCaptured(false);
  }
  // The grab hides the cursor but does not confine it. Pin it to the window
  // centre like the Windows path, so a windowed game never loses the pointer
  // to whatever lies under it; warps produce no raw motion. On Xwayland a
  // warp while grabbed and hidden also turns on Wayland pointer lock.
  void Recenter() {
    GdkWindow* native = gtk_widget_get_window(window_);
    if (!native) return;
    gint x = 0, y = 0;
    gdk_window_get_origin(native, &x, &y);
    gdk_device_warp(pointer_, screen_, x + gdk_window_get_width(native) / 2,
                    y + gdk_window_get_height(native) / 2);
  }
  static gboolean Tick(gpointer data) {
    auto& self = *static_cast<LinuxInput*>(data);
    const bool enabled = REXCVAR_GET(ac6_kbm_enabled);
    if (!enabled && self.was_enabled_) self.state.Reset();
    self.was_enabled_ = enabled;
    // A focus/overlay reset invalidates shared capture immediately. Even if
    // gameplay renewed its lease before this UI tick, retire the old grab
    // before reacquiring so native and shared ownership cannot diverge.
    if (self.grabbed_ && !self.state.Captured()) self.Release();
    if (!enabled || !self.state.WantsCapture(NowMs())) {
      self.Release();
    } else if (!self.grabbed_) {
      GdkWindow* native = gtk_widget_get_window(self.window_);
      if (!native) return G_SOURCE_CONTINUE;
      GdkDisplay* display = gtk_widget_get_display(self.window_);
      self.seat_ = gdk_display_get_default_seat(display);
      self.pointer_ = gdk_seat_get_pointer(self.seat_);
      if (!self.pointer_) return G_SOURCE_CONTINUE;
      GdkCursor* cursor = gdk_cursor_new_for_display(display, GDK_BLANK_CURSOR);
      gdk_device_get_position(self.pointer_, &self.screen_, &self.saved_x_, &self.saved_y_);
      const auto status = gdk_seat_grab(self.seat_, native, GDK_SEAT_CAPABILITY_POINTER,
                                      FALSE, cursor, nullptr, nullptr, nullptr);
      if (cursor) g_object_unref(cursor);
      if (status == GDK_GRAB_SUCCESS) {
        self.grabbed_ = true;
        self.state.SetCaptured(true);
      }
    }
    if (self.grabbed_) self.Recenter();
    return G_SOURCE_CONTINUE;
  }
  static GdkFilterReturn Filter(GdkXEvent* native, GdkEvent*, gpointer data) {
    auto& self = *static_cast<LinuxInput*>(data);
    auto& event = *static_cast<XEvent*>(native);
    if (event.type != GenericEvent || event.xcookie.extension != self.opcode_ ||
        !event.xcookie.data || !REXCVAR_GET(ac6_kbm_enabled) || !self.state.Focused())
      return GDK_FILTER_CONTINUE;
    const auto& raw = *static_cast<const XIRawEvent*>(event.xcookie.data);
    switch (event.xcookie.evtype) {
      case XI_RawKeyPress:
      case XI_RawKeyRelease: {
        // Repeat events cannot resurrect a key discarded on focus/overlay loss.
        if (raw.flags & XIKeyRepeat) break;
        KeySym sym = XkbKeycodeToKeysym(self.display_, raw.detail, 0, 0);
        const KeySym shifted = XkbKeycodeToKeysym(self.display_, raw.detail, 0, 1);
        // Number-row bindings identify the key independent of Shift, including
        // layouts such as AZERTY where the digit is the shifted symbol.
        if (shifted >= XK_0 && shifted <= XK_9) sym = shifted;
        const unsigned key = KeyFromSym(sym);
        if (key) self.state.Key(key, event.xcookie.evtype == XI_RawKeyPress);
      } break;
      case XI_RawButtonPress:
      case XI_RawButtonRelease: {
        const bool down = event.xcookie.evtype == XI_RawButtonPress;
        if ((raw.detail == 4 || raw.detail == 5) && down)
          self.state.Wheel(raw.detail == 4, NowMs());
        else {
          unsigned key = 0;
          switch (raw.detail) {
            case 1: key = 1; break; case 2: key = 4; break; case 3: key = 2; break;
            case 8: key = 5; break; case 9: key = 6; break;
          }
          if (key) self.state.Key(key, down);
        }
      } break;
      case XI_RawMotion: {
        // Processed (accelerated) values, not raw_values: they match the
        // cursor-pixel deltas the Windows path feeds into the same
        // sensitivity settings, so one ac6_input.toml feels alike on both.
        double dx = 0, dy = 0;
        const double* value = raw.valuators.values;
        for (int axis = 0; axis < raw.valuators.mask_len * 8; ++axis) {
          if (!XIMaskIsSet(raw.valuators.mask, axis)) continue;
          if (axis == 0) dx = *value;
          if (axis == 1) dy = *value;
          ++value;
        }
        self.state.Motion(dx, dy);
      } break;
    }
    return GDK_FILTER_CONTINUE;  // SDK hotkeys and ImGui still receive events.
  }
  GtkWidget* window_ = nullptr;
  Display* display_ = nullptr;
  Window root_ = 0;
  int opcode_ = 0;
  std::vector<unsigned char> previous_mask_;
  gulong focus_handler_ = 0, destroy_handler_ = 0;
  guint timer_ = 0;
  bool grabbed_ = false, was_enabled_ = false;
  GdkSeat* seat_ = nullptr;
  GdkDevice* pointer_ = nullptr;
  GdkScreen* screen_ = nullptr;
  gint saved_x_ = 0, saved_y_ = 0;
};

// Stable lifetime: guest polls may continue briefly during UI shutdown. Native
// resources are detached on the UI thread while this state object stays valid.
LinuxInput g_input;
}  // namespace

bool StartLinuxInput(GtkWidget* window) { return g_input.Start(window); }
void StopLinuxInput() { g_input.Stop(); }
bool LinuxInputFocused() { return g_input.state.Focused(); }
bool LinuxKeyHeld(unsigned key) { return g_input.state.Held(key, NowMs()); }
bool LinuxMouseCaptured() { return g_input.state.Captured(); }
void SetLinuxInputOverlay(bool owned) { g_input.state.SetOverlay(owned); }
void RequestLinuxMouseCapture(bool capture) { g_input.state.RequestCapture(capture, NowMs()); }
bool TakeLinuxMouseMotion(double& dx, double& dy) { return g_input.state.TakeMotion(dx, dy); }

}  // namespace ac6
