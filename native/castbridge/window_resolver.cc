#include "cast/castbridge/window_resolver.h"

#include <cstdio>
#include <string>

#include "json/value.h"
#include "util/json/json_serialization.h"

namespace castbridge {

namespace {

// Run a fixed command and capture its stdout. The command is a constant (no
// untrusted input is interpolated), so popen is safe here.
std::string Capture(const char* cmd) {
  std::string out;
  FILE* f = popen(cmd, "r");
  if (!f) {
    return out;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  pclose(f);
  return out;
}

std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c += 32;
    }
  }
  return s;
}

bool MatchesSelector(const Json::Value& client, const std::string& sel_lc) {
  const std::string cls = ToLower(client.get("class", "").asString());
  if (cls == sel_lc) {
    return true;
  }
  const std::string title = ToLower(client.get("title", "").asString());
  return !sel_lc.empty() && title.find(sel_lc) != std::string::npos;
}

WindowMatch FromClient(const Json::Value& c) {
  WindowMatch m;
  m.found = true;
  m.address = c.get("address", "").asString();
  m.pid = c.get("pid", 0).asInt();
  m.title = c.get("title", "").asString();
  m.app_class = c.get("class", "").asString();
  return m;
}

}  // namespace

WindowMatch ResolveWindow(const std::string& selector) {
  if (selector.empty()) {
    const std::string raw = Capture("hyprctl -j activewindow");
    if (raw.empty()) {
      WindowMatch m;
      m.wm_available = false;
      return m;
    }
    auto parsed = openscreen::json::Parse(raw);
    if (parsed.is_value() && parsed.value().isObject() &&
        parsed.value().isMember("address")) {
      return FromClient(parsed.value());
    }
    return {};
  }

  const std::string raw = Capture("hyprctl -j clients");
  if (raw.empty()) {
    WindowMatch m;
    m.wm_available = false;
    return m;
  }
  auto parsed = openscreen::json::Parse(raw);
  if (!parsed.is_value() || !parsed.value().isArray()) {
    return {};
  }
  const std::string sel_lc = ToLower(selector);
  const Json::Value* best = nullptr;
  int best_focus = 1 << 30;
  for (const Json::Value& c : parsed.value()) {
    if (!MatchesSelector(c, sel_lc)) {
      continue;
    }
    const int focus = c.get("focusHistoryID", 1 << 29).asInt();
    if (focus < best_focus) {
      best_focus = focus;
      best = &c;
    }
  }
  return best ? FromClient(*best) : WindowMatch{};
}

}  // namespace castbridge
