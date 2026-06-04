// Unit tests for IpcServer, focused on request dispatch and response delivery
// around client disconnects. These are regression tests for the bug where a
// one-shot client (write a request, then close/half-close immediately) could
// have its request dropped or its response lost.
#include "cast/castbridge/ipc_server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace castbridge {
namespace {

using namespace std::chrono_literals;

std::string UniqueSocketPath() {
  static std::atomic<int> counter{0};
  return "/tmp/cb_ipc_test_" + std::to_string(getpid()) + "_" +
         std::to_string(counter.fetch_add(1)) + ".sock";
}

// Connect a client to the server socket. The server binds asynchronously on its
// own thread, so retry connect() until it is listening.
int ConnectClient(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  for (int i = 0; i < 400; ++i) {
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      return fd;
    }
    std::this_thread::sleep_for(5ms);
  }
  close(fd);
  return -1;
}

// Read one '\n'-terminated line (without the newline) with a timeout. Returns
// "" on timeout or EOF before a newline.
std::string ReadLine(int fd, int timeout_ms = 2000) {
  std::string out;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) {
      char c;
      ssize_t r = read(fd, &c, 1);
      if (r <= 0) {
        break;  // EOF or error
      }
      if (c == '\n') {
        return out;
      }
      out.push_back(c);
    }
  }
  return out;
}

class IpcServerTest : public ::testing::Test {
 protected:
  void StartServer(IpcServer::RequestHandler handler) {
    server_ = std::make_unique<IpcServer>(path_);
    server_->set_request_handler(std::move(handler));
    thread_ = std::thread([this] { server_->Run(); });
  }

  void TearDown() override {
    if (server_) {
      server_->Stop();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    unlink(path_.c_str());
  }

  // A handler that records every dispatched line and notifies waiters.
  IpcServer::RequestHandler RecordingHandler() {
    return [this](int conn, const std::string& line) {
      std::lock_guard<std::mutex> lock(mu_);
      last_conn_ = conn;
      lines_.push_back(line);
      cv_.notify_all();
    };
  }

  bool WaitForLines(size_t n, int timeout_ms = 2000) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [&] { return lines_.size() >= n; });
  }

  std::string path_ = UniqueSocketPath();
  std::unique_ptr<IpcServer> server_;
  std::thread thread_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::string> lines_;
  int last_conn_ = -1;
};

// Baseline: a client that stays connected gets its response.
TEST_F(IpcServerTest, SynchronousRequestGetsResponse) {
  StartServer([this](int conn, const std::string& line) {
    server_->Send(conn, R"({"echo":")" + line + "\"}");
  });
  const int c = ConnectClient(path_);
  ASSERT_GE(c, 0);
  const std::string req = "{\"id\":1}\n";
  ASSERT_EQ(write(c, req.data(), req.size()),
            static_cast<ssize_t>(req.size()));
  const std::string resp = ReadLine(c);
  EXPECT_NE(resp.find("\"echo\""), std::string::npos);
  close(c);
}

// Regression: a one-shot client half-closes (shutdown(SHUT_WR)) right after
// writing, then waits for the reply. The response is produced LATER (as an async
// controller completion would be). It must still reach the client.
TEST_F(IpcServerTest, OneShotHalfCloseReceivesLateResponse) {
  StartServer(RecordingHandler());
  const int c = ConnectClient(path_);
  ASSERT_GE(c, 0);
  const std::string req = "{\"do\":\"x\"}\n";
  ASSERT_EQ(write(c, req.data(), req.size()),
            static_cast<ssize_t>(req.size()));
  shutdown(c, SHUT_WR);  // signal "request done", keep the read side open

  ASSERT_TRUE(WaitForLines(1)) << "request was not dispatched";
  // Reply only after the half-close has surely been observed by the server,
  // reproducing the timing where the disconnect is seen before the response.
  std::this_thread::sleep_for(60ms);
  int conn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    conn = last_conn_;
  }
  server_->Send(conn, R"({"ok":true})");

  const std::string resp = ReadLine(c, 3000);
  EXPECT_NE(resp.find("\"ok\""), std::string::npos)
      << "response lost after half-close (the drain bug)";
  close(c);
}

// Regression: a client that writes a request and immediately closes the whole
// socket must still have its request dispatched (side effects must run), even
// though the response is then undeliverable.
TEST_F(IpcServerTest, RequestDispatchedEvenIfPeerClosesImmediately) {
  StartServer(RecordingHandler());
  const int c = ConnectClient(path_);
  ASSERT_GE(c, 0);
  const std::string req = "{\"fire\":1}\n";
  ASSERT_EQ(write(c, req.data(), req.size()),
            static_cast<ssize_t>(req.size()));
  close(c);  // full close right after the write

  ASSERT_TRUE(WaitForLines(1))
      << "request dropped when the peer closed immediately (the drain bug)";
  std::lock_guard<std::mutex> lock(mu_);
  EXPECT_EQ(lines_[0], "{\"fire\":1}");
}

// Two newline-delimited requests in a single write are both dispatched, in order.
TEST_F(IpcServerTest, PipelinedRequestsBothDispatched) {
  StartServer(RecordingHandler());
  const int c = ConnectClient(path_);
  ASSERT_GE(c, 0);
  const std::string req = "a\nb\n";
  ASSERT_EQ(write(c, req.data(), req.size()),
            static_cast<ssize_t>(req.size()));
  ASSERT_TRUE(WaitForLines(2));
  std::lock_guard<std::mutex> lock(mu_);
  EXPECT_EQ(lines_[0], "a");
  EXPECT_EQ(lines_[1], "b");
  close(c);
}

// A request split across two writes is dispatched once, only after the newline.
TEST_F(IpcServerTest, FragmentedFrameDispatchedOnceOnNewline) {
  StartServer(RecordingHandler());
  const int c = ConnectClient(path_);
  ASSERT_GE(c, 0);
  ASSERT_EQ(write(c, "par", 3), 3);
  std::this_thread::sleep_for(60ms);
  {
    std::lock_guard<std::mutex> lock(mu_);
    EXPECT_TRUE(lines_.empty()) << "dispatched before the frame was complete";
  }
  ASSERT_EQ(write(c, "t\n", 2), 2);
  ASSERT_TRUE(WaitForLines(1));
  std::lock_guard<std::mutex> lock(mu_);
  EXPECT_EQ(lines_[0], "part");
  close(c);
}

}  // namespace
}  // namespace castbridge
