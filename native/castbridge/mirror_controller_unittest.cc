// Unit tests for MirrorController. The controller spawns the `cast_sender`
// binary; we stub it via CASTBRIDGE_SENDER_BIN with a tiny script that reports
// its outcome on CASTBRIDGE_STATUS_FD, so these tests are hermetic (no real
// streaming, no network, no openscreen objects).
#include "cast/castbridge/mirror_controller.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

#include "gtest/gtest.h"

namespace castbridge {
namespace {

using namespace std::chrono_literals;

std::string TempDir() {
  static std::atomic<int> counter{0};
  std::string d = "/tmp/cb_mirror_test_" + std::to_string(getpid()) + "_" +
                  std::to_string(counter.fetch_add(1));
  mkdir(d.c_str(), 0700);
  return d;
}

class MirrorControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = TempDir();
    // Keep mirror.log out of ~/.local/state.
    setenv("XDG_STATE_HOME", dir_.c_str(), 1);
    WriteStub();
  }

  void TearDown() override {
    controller_.Stop();
    unsetenv("CASTBRIDGE_SENDER_BIN");
    unsetenv("CB_TEST_MODE");
    unsetenv("XDG_STATE_HOME");
  }

  // A fake cast_sender: branches on CB_TEST_MODE and reports on the inherited
  // status fd (CASTBRIDGE_STATUS_FD), mirroring what LoopingFileCastAgent does.
  void WriteStub() {
    stub_ = dir_ + "/cast_sender";
    FILE* f = fopen(stub_.c_str(), "w");
    ASSERT_NE(f, nullptr);
    fputs(
        "#!/bin/bash\n"
        "case \"$CB_TEST_MODE\" in\n"
        "  streaming-stay) eval \"echo streaming >&$CASTBRIDGE_STATUS_FD\"; "
        "sleep 1000 ;;\n"
        "  streaming-exit) eval \"echo streaming >&$CASTBRIDGE_STATUS_FD\"; "
        "sleep 0.3; exit 0 ;;\n"
        "  failed) eval \"echo failed >&$CASTBRIDGE_STATUS_FD\"; exit 1 ;;\n"
        "esac\n",
        f);
    fclose(f);
    chmod(stub_.c_str(), 0755);
    setenv("CASTBRIDGE_SENDER_BIN", stub_.c_str(), 1);
  }

  void SetMode(const char* mode) { setenv("CB_TEST_MODE", mode, 1); }

  MirrorControllerTest() {
    controller_.set_on_change([this] {
      std::lock_guard<std::mutex> lock(mu_);
      ++changes_;
      cv_.notify_all();
    });
  }

  bool WaitChanges(int n, int timeout_ms = 2000) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [&] { return changes_ >= n; });
  }

  std::string dir_;
  std::string stub_;
  MirrorController controller_;

  std::mutex mu_;
  std::condition_variable cv_;
  int changes_ = 0;
};

TEST_F(MirrorControllerTest, StartScreenSucceedsWhenSenderStreams) {
  SetMode("streaming-stay");
  const std::string err = controller_.StartScreen("127.0.0.1", "", "TV");
  EXPECT_EQ(err, "");
  const auto s = controller_.GetStatus();
  EXPECT_TRUE(s.active);
  EXPECT_EQ(s.mode, "output");
  EXPECT_EQ(s.device, "TV");
  EXPECT_TRUE(WaitChanges(1)) << "on_change not fired on start";
}

TEST_F(MirrorControllerTest, StartWindowRejectsEmptyAddress) {
  SetMode("streaming-stay");
  const std::string err =
      controller_.StartWindow("127.0.0.1", "", 0, "firefox", "Firefox", "TV");
  EXPECT_NE(err, "");
  EXPECT_FALSE(controller_.GetStatus().active);
}

TEST_F(MirrorControllerTest, StopEndsTheSession) {
  SetMode("streaming-stay");
  ASSERT_EQ(controller_.StartScreen("127.0.0.1", "", "TV"), "");
  ASSERT_TRUE(controller_.GetStatus().active);
  controller_.Stop();
  EXPECT_FALSE(controller_.GetStatus().active);
  EXPECT_TRUE(WaitChanges(2)) << "on_change not fired on stop";
}

TEST_F(MirrorControllerTest, NaturalSenderExitIsDetected) {
  SetMode("streaming-exit");  // streams, then exits ~300ms later
  ASSERT_EQ(controller_.StartScreen("127.0.0.1", "", "TV"), "");
  EXPECT_TRUE(controller_.GetStatus().active);
  // start change + death change.
  EXPECT_TRUE(WaitChanges(2, 3000)) << "sender death not detected";
  EXPECT_FALSE(controller_.GetStatus().active);
}

TEST_F(MirrorControllerTest, StartFailsWhenSenderNeverStreams) {
  SetMode("failed");  // reports failure on both attempts
  const std::string err = controller_.StartScreen("127.0.0.1", "", "TV");
  EXPECT_NE(err, "");
  EXPECT_FALSE(controller_.GetStatus().active);
}

TEST_F(MirrorControllerTest, MissingSenderBinaryReported) {
  setenv("CASTBRIDGE_SENDER_BIN", "/nonexistent/cast_sender", 1);
  const std::string err = controller_.StartScreen("127.0.0.1", "", "TV");
  EXPECT_NE(err.find("native sender binary not found"), std::string::npos);
  EXPECT_FALSE(controller_.GetStatus().active);
}

}  // namespace
}  // namespace castbridge
