#include "cast/castbridge/daemon.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <future>
#include <string>
#include <thread>

#include "cast/castbridge/device_lister.h"
#include "cast/castbridge/ipc_server.h"
#include "cast/castbridge/media_controller.h"
#include "cast/castbridge/mirror_controller.h"
#include "cast/castbridge/nm_relay.h"  // SocketPath()
#include "cast/castbridge/window_resolver.h"
#include "cast/castbridge/youtube_controller.h"
#include "json/value.h"
#include "platform/api/time.h"
#include "platform/base/interface_info.h"
#include "platform/impl/logging.h"
#include "platform/impl/network_interface.h"
#include "platform/impl/platform_client_posix.h"
#include "platform/impl/task_runner.h"
#include "util/chrono_helpers.h"
#include "util/json/json_serialization.h"
#include "util/osp_logging.h"

namespace castbridge {

namespace {

// Set while the IPC server is running; cleared before teardown so a signal that
// arrives during shutdown does not touch the destroyed stack object. Atomic
// because the handler may run on any thread; Stop() is async-signal-safe (it
// only sets an atomic flag and writes the wake eventfd).
std::atomic<IpcServer*> g_server{nullptr};

void HandleSignal(int /*sig*/) {
  IpcServer* const server = g_server.load(std::memory_order_acquire);
  if (server) {
    server->Stop();
  }
}

// Pick the LAN interface for discovery: first non-loopback, non-virtual
// interface that has an IPv4 address.
bool ChooseInterface(openscreen::InterfaceInfo* out) {
  for (const openscreen::InterfaceInfo& iface :
       openscreen::GetNetworkInterfaces()) {
    if (iface.type == openscreen::InterfaceInfo::Type::kLoopback) {
      continue;
    }
    const std::string& n = iface.name;
    if (n.rfind("docker", 0) == 0 || n.rfind("veth", 0) == 0 ||
        n.rfind("virbr", 0) == 0 || n.rfind("br-", 0) == 0 ||
        n.rfind("tailscale", 0) == 0 || n.rfind("tun", 0) == 0 ||
        n.rfind("wg", 0) == 0) {
      continue;
    }
    if (iface.GetIpAddressV4()) {
      *out = iface;
      return true;
    }
  }
  return false;
}

Json::Value DeviceArray(const std::vector<Device>& devices) {
  Json::Value arr(Json::arrayValue);
  for (const Device& d : devices) {
    Json::Value v(Json::objectValue);
    v["id"] = d.id;
    v["name"] = d.name;
    v["model"] = d.model;
    v["ip"] = d.ip;
    v["port"] = d.port;
    arr.append(v);
  }
  return arr;
}

std::string SerializeOrEmpty(const Json::Value& v) {
  auto result = openscreen::json::Stringify(v);
  return result.is_value() ? result.value() : std::string();
}

Json::Value MakeError(const std::string& code, const std::string& message) {
  Json::Value err(Json::objectValue);
  err["code"] = code;
  err["message"] = message;
  return err;
}

Json::Value MediaData(const MediaStatus& m) {
  if (!m.active) {
    return Json::Value::null;
  }
  Json::Value d(Json::objectValue);
  d["state"] = m.state;
  d["title"] = m.title;
  d["position"] = m.position;
  d["duration"] = m.duration;
  d["mediaSessionId"] = m.media_session_id;
  return d;
}

// Single source of truth for the session state (used by `status` and by the
// pushed `session` event).
Json::Value BuildSessionData(MirrorController& mirror,
                             MediaController& media,
                             YouTubeController& yt) {
  const MirrorController::Status ms = mirror.GetStatus();
  const MediaStatus md = media.Snapshot();
  const bool yt_active = yt.active();
  const YouTubeStatus yd = yt.Snapshot();

  Json::Value data(Json::objectValue);
  Json::Value mirror_json = Json::Value::null;
  if (ms.active) {
    mirror_json = Json::Value(Json::objectValue);
    mirror_json["mode"] = ms.mode;
    mirror_json["target"] = ms.target;
    mirror_json["device"] = ms.device;
  }
  std::string sess = "idle";
  if (ms.active) {
    sess = "mirror";
  } else if (yt_active) {
    sess = "youtube";
  } else if (md.active) {
    sess = "media";
  }
  data["session"] = sess;
  data["mirror"] = mirror_json;
  data["media"] = MediaData(md);
  if (yt_active) {
    Json::Value y(Json::objectValue);
    // Real state once the event channel has reported; "PLAYING" is the optimistic
    // default the controller seeds at load, before the first poll frame arrives.
    y["state"] = yd.state.empty() ? "PLAYING" : yd.state;
    y["title"] = yt.title();
    y["position"] = yd.position;
    y["duration"] = yd.duration;
    data["youtube"] = y;
  }
  return data;
}

// Resolve which device an action targets. Fills ip/name on success; on failure
// fills *err (with candidates for the ambiguous case) and returns false.
bool ResolveDevice(DeviceLister& lister,
                   const Json::Value& args,
                   std::string* ip,
                   std::string* name,
                   Json::Value* err) {
  const std::string explicit_ip = args.get("ip", "").asString();
  if (!explicit_ip.empty()) {
    *ip = explicit_ip;
    *name = args.get("device", "").asString();
    return true;
  }
  std::string want = args.get("deviceId", "").asString();
  if (want.empty()) {
    want = args.get("device", "").asString();
  }
  const std::vector<Device> devices = lister.Snapshot();
  if (!want.empty()) {
    for (const Device& d : devices) {
      if (d.id == want || d.name == want) {
        *ip = d.ip;
        *name = d.name;
        return true;
      }
    }
    *err = MakeError("no_devices", "device not found: " + want);
    return false;
  }
  if (devices.size() == 1) {
    *ip = devices[0].ip;
    *name = devices[0].name;
    return true;
  }
  if (devices.empty()) {
    *err = MakeError("no_devices", "no Cast devices found on this network");
    return false;
  }
  *err = MakeError("ambiguous", "multiple devices found; choose one");
  (*err)["candidates"] = DeviceArray(devices);
  return false;
}

void HandleRequest(IpcServer& server,
                   DeviceLister& lister,
                   MirrorController& mirror,
                   MediaController& media,
                   YouTubeController& yt,
                   int conn,
                   const std::string& line) {
  auto parsed = openscreen::json::Parse(line);
  Json::Value resp(Json::objectValue);
  if (!parsed.is_value()) {
    resp["ok"] = false;
    Json::Value err(Json::objectValue);
    err["code"] = "usage";
    err["message"] = "invalid JSON request";
    resp["error"] = err;
    server.Send(conn, SerializeOrEmpty(resp));
    return;
  }

  const Json::Value& msg = parsed.value();
  const std::string action = msg.get("action", "").asString();
  const Json::Value args = msg.get("args", Json::Value(Json::objectValue));
  resp["id"] = msg.get("id", Json::Value::null);
  resp["action"] = action;

  if (action == "devices") {
    Json::Value data(Json::objectValue);
    data["candidates"] = DeviceArray(lister.Snapshot());
    resp["ok"] = true;
    resp["data"] = data;
  } else if (action == "mirror-window") {
    std::string ip, name;
    Json::Value err;
    if (!ResolveDevice(lister, args, &ip, &name, &err)) {
      resp["ok"] = false;
      resp["error"] = err;
    } else {
      std::string selector = args.get("selector", "librewolf").asString();
      WindowMatch w = ResolveWindow(selector);
      if (!w.found) {
        resp["ok"] = false;
        if (!w.wm_available) {
          resp["error"] = MakeError(
              "no_wm",
              "could not query the window manager — window mirroring needs "
              "Hyprland; use screen mirror instead");
        } else {
          resp["error"] = MakeError("no_window",
                                    "no window matched '" + selector + "'");
        }
      } else {
        const std::string label = w.title.empty() ? w.app_class : w.title;
        std::string e = mirror.StartWindow(ip, w.address, w.pid, label, name);
        if (e.empty()) {
          Json::Value data(Json::objectValue);
          data["mode"] = "window";
          data["target"] = label;
          data["device"] = name;
          resp["ok"] = true;
          resp["data"] = data;
        } else {
          resp["ok"] = false;
          resp["error"] = MakeError("exec", e);
        }
      }
    }
  } else if (action == "mirror-screen") {
    std::string ip, name;
    Json::Value err;
    if (!ResolveDevice(lister, args, &ip, &name, &err)) {
      resp["ok"] = false;
      resp["error"] = err;
    } else {
      std::string output = args.get("output", "").asString();
      std::string e = mirror.StartScreen(ip, output, name);
      if (e.empty()) {
        Json::Value data(Json::objectValue);
        data["mode"] = "output";
        data["target"] = output;
        data["device"] = name;
        resp["ok"] = true;
        resp["data"] = data;
      } else {
        resp["ok"] = false;
        resp["error"] = MakeError("exec", e);
      }
    }
  } else if (action == "media-load") {
    std::string ip, name;
    Json::Value err;
    const std::string url = args.get("url", "").asString();
    if (url.empty() || url.size() > 4096 ||
        (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
      resp["ok"] = false;
      resp["error"] = MakeError("usage", "invalid or unsupported URL");
    } else if (!ResolveDevice(lister, args, &ip, &name, &err)) {
      resp["ok"] = false;
      resp["error"] = err;
    } else {
      LoadRequest req;
      req.url = url;
      req.content_type = args.get("contentType", "").asString();
      req.title = args.get("title", "").asString();
      req.current_time = args.get("currentTime", 0.0).asDouble();
      const Json::Value id = resp["id"];
      media.LoadAsync(ip, req, [&server, conn, id](bool ok,
                                                   const std::string& e) {
        Json::Value r(Json::objectValue);
        r["id"] = id;
        r["action"] = "media-load";
        r["ok"] = ok;
        if (ok) {
          Json::Value d(Json::objectValue);
          d["loaded"] = true;
          r["data"] = d;
        } else {
          r["error"] = MakeError("exec", e);
        }
        server.Send(conn, SerializeOrEmpty(r));
      });
      return;  // async: the completion sends the reply
    }
  } else if (action == "youtube-load") {
    std::string ip, name;
    Json::Value err;
    const std::string video_id = args.get("videoId", "").asString();
    static const auto valid_id = [](const std::string& v) {
      if (v.size() != 11) return false;
      for (char c : v) {
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
      }
      return true;
    };
    if (!valid_id(video_id)) {
      resp["ok"] = false;
      resp["error"] = MakeError("usage", "invalid YouTube video id");
    } else if (!ResolveDevice(lister, args, &ip, &name, &err)) {
      resp["ok"] = false;
      resp["error"] = err;
    } else {
      const double start = args.get("currentTime", 0.0).asDouble();
      const Json::Value id = resp["id"];
      // A new YouTube session supersedes any media session.
      media.StopAsync([](bool, const std::string&) {});
      yt.LoadAsync(ip, video_id, start, [&server, conn, id](bool ok,
                                                            const std::string& e) {
        Json::Value r(Json::objectValue);
        r["id"] = id;
        r["action"] = "youtube-load";
        r["ok"] = ok;
        if (ok) {
          Json::Value d(Json::objectValue);
          d["loaded"] = true;
          d["info"] = e;  // screenId during bring-up
          r["data"] = d;
        } else {
          r["error"] = MakeError("exec", e);
        }
        server.Send(conn, SerializeOrEmpty(r));
      });
      return;  // async
    }
  } else if (action == "media-control") {
    const std::string cmd = args.get("cmd", "").asString();
    double value = 0;
    if (cmd == "volume") {
      int v = args.get("value", 0).asInt();
      value = (v < 0 ? 0 : (v > 100 ? 100 : v)) / 100.0;
    } else if (cmd == "seek") {
      value = args.get("value", 0.0).asDouble();
      if (value < 0) {
        value = 0;
      }
    } else if (cmd == "mute") {
      value = args.get("value", false).asBool() ? 1.0 : 0.0;
    }
    const Json::Value id = resp["id"];
    auto route = [&yt, &media](const std::string& c, double v,
                               MediaController::Completion done) {
      if (yt.active()) {
        yt.ControlAsync(c, v, done);
      } else {
        media.ControlAsync(c, v, done);
      }
    };
    route(cmd, value, [&server, conn, id, cmd](bool ok, const std::string& e) {
      Json::Value r(Json::objectValue);
      r["id"] = id;
      r["action"] = "media-control";
      r["ok"] = ok;
      if (ok) {
        Json::Value d(Json::objectValue);
        d["cmd"] = cmd;
        r["data"] = d;
      } else {
        r["error"] = MakeError("exec", e);
      }
      server.Send(conn, SerializeOrEmpty(r));
    });
    return;  // async
  } else if (action == "status") {
    resp["ok"] = true;
    resp["data"] = BuildSessionData(mirror, media, yt);
  } else if (action == "stop") {
    // Tear down whatever is active without blocking the IPC thread, and reply
    // immediately so the UI feels instant. mirror.Stop() is thread-safe; media
    // and youtube stops already run on the TaskRunner / worker threads.
    std::thread([&mirror] { mirror.Stop(); }).detach();
    media.StopAsync([](bool, const std::string&) {});
    yt.StopAsync([](bool, const std::string&) {});
    Json::Value data(Json::objectValue);
    data["stopped"] = true;
    resp["ok"] = true;
    resp["data"] = data;
  } else {
    resp["ok"] = false;
    resp["error"] = MakeError("usage", "unknown action '" + action + "'");
  }
  server.Send(conn, SerializeOrEmpty(resp));
}

}  // namespace

int RunDaemon() {
  std::signal(SIGPIPE, SIG_IGN);
  openscreen::SetLogLevel(openscreen::LogLevel::kInfo);

  auto* const task_runner = new openscreen::TaskRunnerImpl(&openscreen::Clock::now);
  openscreen::PlatformClientPosix::Create(
      openscreen::milliseconds(50),
      std::unique_ptr<openscreen::TaskRunnerImpl>(task_runner));

  bool ok = false;
  // Controllers live in an inner scope so they are fully torn down (worker
  // threads + monitors joined) BEFORE PlatformClientPosix::ShutDown() frees the
  // TaskRunner — avoiding use-after-free from late worker tasks.
  {
  DeviceLister lister(*task_runner);
  MirrorController mirror;
  MediaController media(*task_runner);
  YouTubeController yt(*task_runner);
  IpcServer server(SocketPath());

  // Push media-status events (live position) — no polling on the extension side.
  media.set_on_status([&server](const MediaStatus& m) {
    Json::Value evt(Json::objectValue);
    evt["type"] = "media-status";
    evt["data"] = MediaData(m);
    server.Broadcast(SerializeOrEmpty(evt));
  });

  // Single source of truth: push a `session` event on every lifecycle change
  // (start / stop / natural end), so the toolbar badge stays correct without
  // polling and regardless of whether the popup is open.
  auto broadcast_session = [&server, &mirror, &media, &yt] {
    Json::Value evt(Json::objectValue);
    evt["type"] = "session";
    evt["data"] = BuildSessionData(mirror, media, yt);
    server.Broadcast(SerializeOrEmpty(evt));
  };
  mirror.set_on_change(broadcast_session);
  media.set_on_change(broadcast_session);
  yt.set_on_change(broadcast_session);
  // A YouTube playback state change (PLAYING <-> PAUSED, natural end) re-pushes
  // the same `session` event, which now carries the real state from the Lounge
  // event channel instead of the old hardcoded "PLAYING".
  yt.set_on_status([broadcast_session](const YouTubeStatus&) {
    broadcast_session();
  });

  // Push a devices-changed event whenever discovery updates (task thread).
  lister.set_on_change([&server, &lister] {
    Json::Value evt(Json::objectValue);
    evt["type"] = "devices-changed";
    Json::Value data(Json::objectValue);
    data["candidates"] = DeviceArray(lister.Snapshot());
    evt["data"] = data;
    server.Broadcast(SerializeOrEmpty(evt));
  });

  server.set_request_handler(
      [&server, &lister, &mirror, &media, &yt](int conn, const std::string& line) {
        HandleRequest(server, lister, mirror, media, yt, conn, line);
      });

  g_server.store(&server, std::memory_order_release);
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::thread tr_thread([task_runner] { task_runner->RunUntilStopped(); });

  task_runner->PostTask([&lister] {
    openscreen::InterfaceInfo iface;
    if (ChooseInterface(&iface)) {
      lister.Start(iface);
    } else {
      OSP_LOG_ERROR << "castbridge: no usable network interface for discovery";
    }
  });

  ok = server.Run();  // blocks until Stop() (signal)

  // A signal arriving from here on must not touch the soon-to-be-destroyed
  // stack objects (server, controllers): drop the global so HandleSignal is a
  // no-op during teardown.
  g_server.store(nullptr, std::memory_order_release);

  // Tear down sessions while the TaskRunner is still alive. The cast clients
  // (TLS connections) MUST be destroyed on the TaskRunner thread, so do it via a
  // posted task and wait for it before stopping the runner.
  mirror.Stop();  // signals cast_sender + joins its monitor (no openscreen objs)
  {
    std::promise<void> torn;
    task_runner->PostTask([&] {
      media.ResetClient();
      yt.ResetClient();
      lister.Shutdown();
      torn.set_value();
    });
    torn.get_future().wait_for(std::chrono::seconds(3));
  }
  task_runner->PostTask([task_runner] { task_runner->RequestStopSoon(); });
  tr_thread.join();
  }  // controllers destroyed here (clients already null; workers/monitors joined)

  openscreen::PlatformClientPosix::ShutDown();
  return ok ? 0 : 1;
}

}  // namespace castbridge
