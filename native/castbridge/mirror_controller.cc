#include "cast/castbridge/mirror_controller.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace castbridge {

namespace {

std::string SelfDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return ".";
  }
  buf[n] = '\0';
  std::string p(buf);
  auto pos = p.find_last_of('/');
  return pos == std::string::npos ? "." : p.substr(0, pos);
}

std::string SenderBin() {
  const char* env = std::getenv("CASTBRIDGE_SENDER_BIN");
  if (env && *env) {
    return env;
  }
  return SelfDir() + "/cast_sender";  // sibling in the same build dir
}

std::string Bitrate() {
  const char* env = std::getenv("CASTBRIDGE_BITRATE");
  return (env && *env) ? env : "16000000";
}

std::string LogPath() {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  std::string base = (xdg && *xdg)
                         ? std::string(xdg)
                         : std::string(std::getenv("HOME")) + "/.local/state";
  std::string dir = base + "/castbridge";
  mkdir(dir.c_str(), 0700);
  return dir + "/mirror.log";
}

std::string TailFile(const std::string& path, size_t max) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) {
    return "";
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long start =
      size > static_cast<long>(max) ? size - static_cast<long>(max) : 0;
  fseek(f, start, SEEK_SET);
  std::string out;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  fclose(f);
  return out;
}

}  // namespace

MirrorController::~MirrorController() {
  Stop();
}

namespace {
enum class StartResult { kStreaming, kFailed, kExited };

// Wait for the sender to report its outcome on the dedicated status pipe. The
// child writes "streaming" once negotiation succeeds or "failed" on negotiation
// error (see LoopingFileCastAgent + CASTBRIDGE_STATUS_FD); pipe EOF or process
// exit means it died without reporting. This replaces scraping the human-facing
// log for "Streaming to ", which coupled us to the library's log wording.
StartResult WaitForStatus(int read_fd, pid_t pid) {
  constexpr int kTimeoutMs = 12000;
  std::string buf;
  for (int waited = 0; waited < kTimeoutMs; waited += 200) {
    struct pollfd pfd = {read_fd, POLLIN, 0};
    if (poll(&pfd, 1, 200) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      char tmp[256];
      ssize_t n = read(read_fd, tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.find("streaming") != std::string::npos) {
          return StartResult::kStreaming;
        }
        if (buf.find("failed") != std::string::npos) {
          return StartResult::kFailed;
        }
      } else if (n == 0) {
        return StartResult::kExited;  // all write ends closed: child gone
      }
    }
    if (waitpid(pid, nullptr, WNOHANG) == pid) {
      return StartResult::kExited;
    }
  }
  return StartResult::kFailed;  // timeout
}
}  // namespace

std::string MirrorController::Launch(const std::string& ip,
                                     const std::string& target,
                                     int audio_pid,
                                     const std::string& audio_app,
                                     const std::string& mode,
                                     const std::string& label,
                                     const std::string& device) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  StopInternal();  // tear down any prior session (joins its monitor)

  const std::string bin = SenderBin();
  if (access(bin.c_str(), X_OK) != 0) {
    return "native sender binary not found: " + bin;
  }
  const std::string endpoint = ip + ":8009";
  const std::string bitrate = Bitrate();
  const std::string log = LogPath();

  std::vector<std::string> args = {bin, "-c", "h264", "-m", bitrate};
  std::string pid_str;
  if (audio_pid > 0) {
    pid_str = std::to_string(audio_pid);
    args.push_back("-A");
    args.push_back(pid_str);
  }
  // Also match the app's audio node by name (browsers report a launcher PID on
  // their PipeWire node, so the window PID alone misses their audio).
  if (!audio_app.empty()) {
    args.push_back("-N");
    args.push_back(audio_app);
  }
  args.push_back(endpoint);
  args.push_back(target);

  // The receiver occasionally drops the streaming OFFER (transient
  // AnswerTimeout); retry once before reporting failure.
  std::string last_err;
  for (int attempt = 0; attempt < 2; ++attempt) {
    int status_pipe[2];
    if (pipe(status_pipe) != 0) {
      return "pipe failed";
    }
    pid_t pid = fork();
    if (pid < 0) {
      close(status_pipe[0]);
      close(status_pipe[1]);
      return "fork failed";
    }
    if (pid == 0) {
      // Own process group so StopInternal can signal the whole tree, not just
      // the direct child.
      setpgid(0, 0);
      close(status_pipe[0]);  // child only writes
      // Hand the write end to cast_sender via a fixed env var; it survives exec
      // (not close-on-exec) and is where the agent reports
      // "streaming"/"failed".
      setenv("CASTBRIDGE_STATUS_FD", std::to_string(status_pipe[1]).c_str(), 1);
      int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
      std::vector<char*> argv;
      for (auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
      }
      argv.push_back(nullptr);
      execv(bin.c_str(), argv.data());
      _exit(127);
    }

    close(status_pipe[1]);  // parent only reads
    // No lock held here: WaitForStatus only blocks on the pipe / child.
    const StartResult result = WaitForStatus(status_pipe[0], pid);
    close(status_pipe[0]);

    if (result == StartResult::kStreaming) {
      uint64_t gen;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pid_ = pid;
        status_ = {true, mode, label, device};
        gen = ++gen_;
      }
      monitor_ = std::thread(&MirrorController::Monitor, this, pid, gen);
      if (on_change_) {
        on_change_();
      }
      return "";
    }
    last_err = "sender did not start streaming:\n" + TailFile(log, 800);
    if (result != StartResult::kExited) {
      kill(-pid, SIGKILL);       // whole process group
      waitpid(pid, nullptr, 0);  // reap the failed attempt
    }
  }
  return last_err;
}

std::string MirrorController::StartWindow(const std::string& ip,
                                          const std::string& address,
                                          int audio_pid,
                                          const std::string& audio_app,
                                          const std::string& label,
                                          const std::string& device) {
  if (address.empty()) {
    return "could not resolve a window address";
  }
  return Launch(ip, "window:addr=" + address, audio_pid, audio_app, "window",
                label, device);
}

std::string MirrorController::StartScreen(const std::string& ip,
                                          const std::string& output,
                                          const std::string& device) {
  const std::string target = output.empty() ? "screen" : "screen:" + output;
  return Launch(ip, target, 0, "", "output", output.empty() ? "screen" : output,
                device);
}

// Reaps the sender when it exits (stop OR natural death: window closed, TV
// off), and announces the change. The generation guard prevents a stale monitor
// from touching a newer session. Only this thread calls waitpid on the child.
void MirrorController::Monitor(pid_t pid, uint64_t gen) {
  waitpid(pid, nullptr, 0);  // block until the sender process exits
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (gen_ == gen) {
      pid_ = -1;
      status_ = {};
      changed = true;
    }
  }
  if (changed && on_change_) {
    on_change_();
  }
}

// Assumes lifecycle_mutex_ is held. Signals the child and lets its monitor reap
// it; never calls waitpid itself (avoids a double-reap race with the monitor).
void MirrorController::StopInternal() {
  pid_t pid = -1;
  bool was_active = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ > 0) {
      pid = pid_;
      was_active = status_.active;
      pid_ = -1;
      status_ = {};
      ++gen_;  // invalidate the monitor's match so it won't re-announce
    }
  }
  if (was_active && on_change_) {
    on_change_();  // announce idle promptly
  }
  if (pid > 0) {
    // Signal the whole process group (negative pid) so any helper the sender
    // spawned dies with it; fall back to the bare pid if the group is gone.
    kill(-pid, SIGTERM);
    for (int i = 0; i < 10; ++i) {  // ~500ms graceful window
      if (kill(pid, 0) != 0) {
        break;
      }
      usleep(50 * 1000);
    }
    if (kill(pid, 0) == 0) {
      kill(-pid, SIGKILL);
    }
  }
  if (monitor_.joinable()) {
    monitor_.join();  // the monitor reaps the child and then returns
  }
}

void MirrorController::Stop() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  StopInternal();
}

MirrorController::Status MirrorController::GetStatus() {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

}  // namespace castbridge
