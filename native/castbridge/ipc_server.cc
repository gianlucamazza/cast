#include "cast/castbridge/ipc_server.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace castbridge {

namespace {

void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags != -1) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

}  // namespace

IpcServer::IpcServer(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

IpcServer::~IpcServer() {
  if (wake_fd_ != -1) {
    close(wake_fd_);
  }
  if (listen_fd_ != -1) {
    close(listen_fd_);
  }
  unlink(socket_path_.c_str());
}

bool IpcServer::Run() {
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    perror("castbridge: socket");
    return false;
  }

  // Create the runtime dir (0700) and remove any stale socket.
  std::string dir = socket_path_.substr(0, socket_path_.find_last_of('/'));
  mkdir(dir.c_str(), 0700);
  unlink(socket_path_.c_str());

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    fprintf(stderr, "castbridge: socket path too long: %s\n",
            socket_path_.c_str());
    return false;
  }
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("castbridge: bind");
    return false;
  }
  if (listen(listen_fd_, 8) < 0) {
    perror("castbridge: listen");
    return false;
  }
  SetNonBlocking(listen_fd_);

  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    perror("castbridge: eventfd");
    return false;
  }

  running_ = true;
  while (running_) {
    std::vector<pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});
    fds.push_back({wake_fd_, POLLIN, 0});
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [fd, conn] : conns_) {
        short events = POLLIN;
        if (!conn.out.empty()) {
          events |= POLLOUT;
        }
        fds.push_back({fd, events, 0});
      }
    }

    int n = poll(fds.data(), fds.size(), -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("castbridge: poll");
      break;
    }

    if (fds[1].revents & POLLIN) {
      DrainWake();
    }

    if (fds[0].revents & POLLIN) {
      for (;;) {
        int cfd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
          break;
        }
        SetNonBlocking(cfd);
        std::lock_guard<std::mutex> lock(mutex_);
        conns_[cfd];
      }
    }

    // Handle per-connection events (skip the two fixed fds).
    for (size_t i = 2; i < fds.size(); ++i) {
      const int fd = fds[i].fd;
      const short re = fds[i].revents;
      if (re & (POLLHUP | POLLERR)) {
        CloseConn(fd);
        continue;
      }
      if (re & POLLOUT) {
        bool closed = false;
        for (;;) {
          std::string frame;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = conns_.find(fd);
            if (it == conns_.end() || it->second.out.empty()) {
              break;
            }
            frame = it->second.out.front();
          }
          ssize_t w = write(fd, frame.data(), frame.size());
          if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            CloseConn(fd);
            closed = true;
            break;
          }
          std::lock_guard<std::mutex> lock(mutex_);
          auto it = conns_.find(fd);
          if (it == conns_.end()) {
            break;
          }
          if (static_cast<size_t>(w) < frame.size()) {
            it->second.out.front().erase(0, w);
            break;
          }
          it->second.out.pop_front();
        }
        if (closed) {
          continue;
        }
      }
      if (re & POLLIN) {
        char buf[4096];
        bool closed = false;
        for (;;) {
          ssize_t r = read(fd, buf, sizeof(buf));
          if (r > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = conns_.find(fd);
            if (it != conns_.end()) {
              it->second.in.append(buf, r);
            }
          } else if (r == 0) {
            CloseConn(fd);
            closed = true;
            break;
          } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            CloseConn(fd);
            closed = true;
            break;
          }
        }
        if (closed) {
          continue;
        }
        // Extract complete lines and dispatch.
        for (;;) {
          std::string line;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = conns_.find(fd);
            if (it == conns_.end()) {
              break;
            }
            auto pos = it->second.in.find('\n');
            if (pos == std::string::npos) {
              break;
            }
            line = it->second.in.substr(0, pos);
            it->second.in.erase(0, pos + 1);
          }
          if (!line.empty() && handler_) {
            handler_(fd, line);
          }
        }
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [fd, conn] : conns_) {
    close(fd);
  }
  conns_.clear();
  return true;
}

void IpcServer::QueueLocked(int fd, const std::string& json) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) {
    return;
  }
  it->second.out.push_back(json + "\n");
}

void IpcServer::Send(int conn_id, const std::string& json) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    QueueLocked(conn_id, json);
  }
  Wake();
}

void IpcServer::Broadcast(const std::string& json) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [fd, conn] : conns_) {
      conn.out.push_back(json + "\n");
    }
  }
  Wake();
}

void IpcServer::Stop() {
  running_ = false;
  Wake();
}

void IpcServer::Wake() {
  if (wake_fd_ != -1) {
    uint64_t one = 1;
    ssize_t ignored = write(wake_fd_, &one, sizeof(one));
    (void)ignored;
  }
}

void IpcServer::DrainWake() {
  uint64_t v;
  while (read(wake_fd_, &v, sizeof(v)) > 0) {
  }
}

void IpcServer::CloseConn(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = conns_.find(fd);
  if (it != conns_.end()) {
    close(fd);
    conns_.erase(it);
  }
}

}  // namespace castbridge
