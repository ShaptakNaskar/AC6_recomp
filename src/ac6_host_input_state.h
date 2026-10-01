#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace ac6 {

// UI-thread events / guest-thread polls. Focus and overlay transitions discard
// held input, so returning to gameplay never resurrects an old press or delta.
class HostInputState {
 public:
  void SetFocus(bool focused) {
    std::lock_guard lock(mutex_);
    if (focused_ != focused) { focused_ = focused; Clear(); }
  }
  bool Focused() const { std::lock_guard lock(mutex_); return focused_; }
  void SetOverlay(bool overlay) {
    std::lock_guard lock(mutex_);
    if (overlay_ != overlay) { overlay_ = overlay; Clear(); }
  }
  void Reset() { std::lock_guard lock(mutex_); Clear(); }
  void Key(unsigned key, bool down) {
    std::lock_guard lock(mutex_);
    if (key < keys_.size() && (!down || (focused_ && !overlay_))) keys_[key] = down;
  }
  bool Held(unsigned key, int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    if (!focused_ || overlay_) return false;
    if (key == 0x0E) return now_ms < wheel_up_until_;
    if (key == 0x0F) return now_ms < wheel_down_until_;
    if (key == 0x10) return keys_[0xA0] || keys_[0xA1];
    if (key == 0x11) return keys_[0xA2] || keys_[0xA3];
    if (key == 0x12) return keys_[0xA4] || keys_[0xA5];
    return key < keys_.size() && keys_[key];
  }
  void Wheel(bool up, int64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (focused_ && !overlay_) (up ? wheel_up_until_ : wheel_down_until_) = now_ms + 90;
  }
  void RequestCapture(bool capture, int64_t now_ms) {
    std::lock_guard lock(mutex_);
    capture_until_ = capture && focused_ && !overlay_ ? now_ms + 100 : 0;
  }
  bool WantsCapture(int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    return focused_ && !overlay_ && now_ms < capture_until_;
  }
  void SetCaptured(bool captured) {
    std::lock_guard lock(mutex_);
    captured = captured && focused_ && !overlay_;
    if (captured_ != captured) { captured_ = captured; dx_ = dy_ = 0; }
  }
  bool Captured() const { std::lock_guard lock(mutex_); return captured_; }
  void Motion(double dx, double dy) {
    std::lock_guard lock(mutex_);
    if (focused_ && !overlay_ && captured_ && std::isfinite(dx) && std::isfinite(dy)) {
      dx_ += dx; dy_ += dy;
    }
  }
  bool TakeMotion(double& dx, double& dy) {
    std::lock_guard lock(mutex_);
    dx = dx_; dy = dy_; dx_ = dy_ = 0;
    return focused_ && !overlay_ && captured_;
  }

 private:
  void Clear() {
    keys_.fill(false);
    wheel_up_until_ = wheel_down_until_ = capture_until_ = 0;
    captured_ = false;
    dx_ = dy_ = 0;
  }
  mutable std::mutex mutex_;
  std::array<bool, 256> keys_{};
  bool focused_ = false, overlay_ = false, captured_ = false;
  int64_t wheel_up_until_ = 0, wheel_down_until_ = 0, capture_until_ = 0;
  double dx_ = 0, dy_ = 0;
};

}  // namespace ac6
