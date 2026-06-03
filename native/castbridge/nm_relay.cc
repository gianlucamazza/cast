#include "cast/castbridge/nm_relay.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace castbridge {

namespace {

constexpr uint32_t kMaxMessage = 16 * 1024 * 1024;  // generous browser->host cap

std::string RuntimeDir() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg && *xdg) {
    return std::string(xdg) + "/castbridge";
  }
  return "/tmp/castbridge-" + std::to_string(getuid());
}

// Read exactly n bytes from fd into buf; false on EOF/error.
bool ReadExact(int fd, void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r > 0) {
      got += static_cast<size_t>(r);
    } else if (r == 0) {
      return false;
    } else if (errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool WriteAll(int fd, const void* buf, size_t n) {
  auto* p = static_cast<const uint8_t*>(buf);
  size_t put = 0;
  while (put < n) {
    ssize_t w = write(fd, p + put, n - put);
    if (w > 0) {
      put += static_cast<size_t>(w);
    } else if (errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

// Send a JSON string to Firefox as a native-messaging frame.
bool WriteNmFrame(const std::string& json) {
  uint32_t len = static_cast<uint32_t>(json.size());
  return WriteAll(STDOUT_FILENO, &len, sizeof(len)) &&
         WriteAll(STDOUT_FILENO, json.data(), json.size());
}

int ConnectSocket(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Spawn `castbridge --daemon` detached from this relay.
void SpawnDaemon() {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) {
    return;
  }
  exe[n] = '\0';
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    // Detach the daemon's stdio from the relay's native-messaging pipes, so it
    // never writes into the browser's stdout (which would corrupt framing) and
    // does not hold the pipe open.
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        close(devnull);
      }
    }
    execl(exe, exe, "--daemon", static_cast<char*>(nullptr));
    _exit(127);
  }
}

// Serialize daemon startup across concurrent relays: hold an exclusive flock
// while spawning so only one relay launches the daemon. Returns the lock fd
// (caller closes to release) or -1 if locking is unavailable.
int AcquireSpawnLock() {
  const std::string dir = RuntimeDir();
  mkdir(dir.c_str(), 0700);  // ok if it already exists
  const std::string lock_path = dir + "/spawn.lock";
  int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    return -1;
  }
  if (flock(fd, LOCK_EX) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int ConnectWithStart(const std::string& path) {
  int fd = ConnectSocket(path);
  if (fd >= 0) {
    return fd;
  }
  // Take the spawn lock, then re-check: another relay may have started the
  // daemon while we waited for the lock.
  int lock = AcquireSpawnLock();
  fd = ConnectSocket(path);
  if (fd < 0) {
    SpawnDaemon();
    for (int i = 0; i < 100; ++i) {  // up to ~5s for discovery startup
      usleep(50 * 1000);
      fd = ConnectSocket(path);
      if (fd >= 0) {
        break;
      }
    }
  }
  if (lock >= 0) {
    close(lock);  // releases the flock
  }
  return fd;
}

}  // namespace

std::string SocketPath() {
  return RuntimeDir() + "/sock";
}

int RunNmRelay() {
  const std::string path = SocketPath();
  int sock = ConnectWithStart(path);
  if (sock < 0) {
    WriteNmFrame(R"({"ok":false,"error":{"code":"daemon","message":"cannot reach castbridge daemon"}})");
    return 1;
  }

  std::string sock_in;  // accumulated bytes from the daemon (split on '\n')
  pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sock, POLLIN, 0}};

  for (;;) {
    int n = poll(fds, 2, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    // Browser -> daemon: read one NM frame, forward as a JSON line.
    if (fds[0].revents & POLLIN) {
      uint32_t len = 0;
      if (!ReadExact(STDIN_FILENO, &len, sizeof(len))) {
        break;  // browser closed the port
      }
      if (len == 0 || len > kMaxMessage) {
        break;
      }
      std::string msg(len, '\0');
      if (!ReadExact(STDIN_FILENO, msg.data(), len)) {
        break;
      }
      msg.push_back('\n');
      if (!WriteAll(sock, msg.data(), msg.size())) {
        break;
      }
    }
    if (fds[0].revents & (POLLHUP | POLLERR)) {
      break;
    }

    // Daemon -> browser: forward each JSON line as an NM frame.
    if (fds[1].revents & POLLIN) {
      char buf[8192];
      ssize_t r = read(sock, buf, sizeof(buf));
      if (r <= 0) {
        break;
      }
      sock_in.append(buf, r);
      for (;;) {
        auto pos = sock_in.find('\n');
        if (pos == std::string::npos) {
          break;
        }
        std::string line = sock_in.substr(0, pos);
        sock_in.erase(0, pos + 1);
        if (!line.empty() && !WriteNmFrame(line)) {
          close(sock);
          return 1;
        }
      }
    }
    if (fds[1].revents & (POLLHUP | POLLERR)) {
      break;
    }
  }

  close(sock);
  return 0;
}

}  // namespace castbridge
