// Unit tests for MediaController's no-session paths, driven deterministically
// with a FakeTaskRunner (no network, no real Cast receiver). Session/state
// behavior that needs a live receiver is integration-tested elsewhere.
#include "cast/castbridge/media_controller.h"

#include <string>

#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"

namespace castbridge {
namespace {

class MediaControllerTest : public ::testing::Test {
 protected:
  openscreen::FakeClock clock_{openscreen::Clock::now()};
  openscreen::FakeTaskRunner task_runner_{clock_};
  MediaController controller_{task_runner_};
};

TEST_F(MediaControllerTest, ControlWithNoSessionFails) {
  bool called = false, ok = true;
  std::string err;
  controller_.ControlAsync("play", 0, [&](bool o, const std::string& e) {
    called = true;
    ok = o;
    err = e;
  });
  task_runner_.RunTasksUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_FALSE(ok);
  EXPECT_EQ(err, "no active media session");
}

TEST_F(MediaControllerTest, StopWithNoSessionSucceedsQuietly) {
  int changes = 0;
  controller_.set_on_change([&] { ++changes; });
  bool ok = false;
  std::string err = "unset";
  controller_.StopAsync([&](bool o, const std::string& e) {
    ok = o;
    err = e;
  });
  task_runner_.RunTasksUntilIdle();
  EXPECT_TRUE(ok);
  EXPECT_EQ(err, "");
  EXPECT_EQ(changes, 0) << "no session was active, so no change should fire";
}

TEST_F(MediaControllerTest, SnapshotDefaultsToInactive) {
  EXPECT_FALSE(controller_.Snapshot().active);
}

}  // namespace
}  // namespace castbridge
