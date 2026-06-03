// Resolves a Hyprland window to its address + PID for window mirroring. Uses
// `hyprctl -j` (a generic Hyprland tool) and matches by app-id (class) or title.
// This is our own native resolution; it does not use any skill-cast helper.
#ifndef CAST_CASTBRIDGE_WINDOW_RESOLVER_H_
#define CAST_CASTBRIDGE_WINDOW_RESOLVER_H_

#include <string>

namespace castbridge {

struct WindowMatch {
  bool found = false;
  // False when the window manager could not be queried at all (e.g. hyprctl is
  // absent on a non-Hyprland compositor) — distinct from "queried, no match".
  bool wm_available = true;
  std::string address;  // Hyprland address, e.g. "0x55..."
  int pid = 0;
  std::string title;
  std::string app_class;
};

// Resolve a window. Empty selector -> the active window; otherwise match the
// class (case-insensitive exact) or the title (case-insensitive substring),
// picking the most-recently-focused match.
WindowMatch ResolveWindow(const std::string& selector);

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_WINDOW_RESOLVER_H_
