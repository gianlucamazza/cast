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

openscreen::IPEndpoint Device(const char* ip) {
  return {openscreen::IPAddress::Parse(ip).value(), 8009};
}

TEST(CanReuseSessionTest, SameDeviceAndRunningAppReuses) {
  const auto tv = Device("192.168.1.228");
  EXPECT_TRUE(CanReuseSession(true, true, tv, "", tv, ""));
  // An explicit Default Media Receiver id is the same app as the empty default.
  EXPECT_TRUE(CanReuseSession(true, true, tv, "", tv, kDefaultMediaReceiverAppId));
}

TEST(CanReuseSessionTest, AnythingElseReconnects) {
  const auto tv = Device("192.168.1.228");
  const auto other = Device("192.168.1.229");
  EXPECT_FALSE(CanReuseSession(false, true, tv, "", tv, ""))
      << "app gone or replaced on the receiver";
  EXPECT_FALSE(CanReuseSession(true, false, tv, "", tv, ""))
      << "the first LOAD is still pending";
  EXPECT_FALSE(CanReuseSession(true, true, tv, "", other, ""));
  EXPECT_FALSE(CanReuseSession(true, true, tv, "", tv, "07841171"))
      << "a different receiver app needs its own LAUNCH";
}

}  // namespace
}  // namespace castbridge
