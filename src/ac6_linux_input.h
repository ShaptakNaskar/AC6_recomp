#pragma once

struct _GtkWidget;

namespace ac6 {
#if !defined(_WIN32)
// Start/stop on the GTK UI thread. Poll/state functions are thread-safe and
// never call GTK or Xlib from a guest worker thread.
bool StartLinuxInput(_GtkWidget* window);
void StopLinuxInput();
bool LinuxInputFocused();
bool LinuxKeyHeld(unsigned key);
bool LinuxMouseCaptured();
void SetLinuxInputOverlay(bool owned);
void RequestLinuxMouseCapture(bool capture);
bool TakeLinuxMouseMotion(double& dx, double& dy);
#endif
}  // namespace ac6
