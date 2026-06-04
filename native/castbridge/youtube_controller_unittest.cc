// Unit tests for YouTubeController's no-session paths. The controller runs a
// worker thread for Lounge HTTP work; with no active session no network is
// touched, so these are hermetic. Session behavior is integration-tested.
#include "cast/castbridge/youtube_controller.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"

namespace castbridge {
namespace {

using namespace std::chrono_literals;

class YouTubeControllerTest : public ::testing::Test {
 protected:
  openscreen::FakeClock clock_{openscreen::Clock::now()};
  openscreen::FakeTaskRunner task_runner_{clock_};
  YouTubeController controller_{task_runner_};
};

TEST_F(YouTubeControllerTest, DefaultsToInactive) {
  EXPECT_FALSE(controller_.active());
  EXPECT_TRUE(controller_.title().empty());
}

TEST_F(YouTubeControllerTest, ControlWithNoSessionFails) {
  bool called = false, ok = true;
  std::string err;
  // No active session -> synchronous failure (no task/worker involved).
  controller_.ControlAsync("play", 0, [&](bool o, const std::string& e) {
    called = true;
    ok = o;
    err = e;
  });
  EXPECT_TRUE(called);
  EXPECT_FALSE(ok);
  EXPECT_EQ(err, "no active YouTube session");
}

TEST_F(YouTubeControllerTest, StopWithNoSessionSucceeds) {
  // StopAsync completes from the worker thread; wait for it.
  std::mutex m;
  std::condition_variable cv;
  bool called = false, ok = false;
  controller_.StopAsync([&](bool o, const std::string&) {
    {
      std::lock_guard<std::mutex> lock(m);
      ok = o;
      called = true;
    }
    cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(m);
  ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return called; }));
  EXPECT_TRUE(ok);
}

}  // namespace
}  // namespace castbridge
