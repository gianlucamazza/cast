// Unit tests for MediaController's no-session paths, driven deterministically
// with a FakeTaskRunner (no network, no real Cast receiver). Session/state
// behavior that needs a live receiver is integration-tested elsewhere.
#include "cast/castbridge/media_controller.h"

#include <chrono>
#include <string>

#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"

namespace castbridge {

class MediaControllerRecoveryTest : public ::testing::Test {
 protected:
  openscreen::FakeClock clock_{openscreen::Clock::now()};
  openscreen::FakeTaskRunner runner_{clock_};
  MediaController controller_{runner_};
  void Seed() {
    controller_.had_session_ = true;
    controller_.status_.active = true;
    controller_.status_.position = 219;
    controller_.status_.duration = 6000;
    controller_.status_.media_session_id = 42;
    controller_.epoch_ = 7;
  }
  void Drop(uint64_t epoch) { controller_.OnClosed(epoch); }
  void Finish() {
    auto status = controller_.Snapshot();
    status.active = false;
    status.end_reason = "finished";
    controller_.OnStatusUpdate(status);
  }
  bool recovering() const { return controller_.recovering_; }
  bool has_client() const { return controller_.client_ != nullptr; }
};

TEST_F(MediaControllerRecoveryTest,
       TransportLossPreservesPositionAndAnnouncesDisconnect) {
  Seed();
  int changes = 0;
  controller_.set_on_change([&] { ++changes; });
  Drop(7);
  runner_.RunTasksUntilIdle();
  auto status = controller_.Snapshot();
  EXPECT_FALSE(status.active);
  EXPECT_EQ(status.end_reason, "disconnected");
  EXPECT_EQ(status.position, 219);
  EXPECT_EQ(status.duration, 6000);
  EXPECT_EQ(status.media_session_id, 42);
  EXPECT_TRUE(recovering());
  EXPECT_EQ(changes, 1);
}

TEST_F(MediaControllerRecoveryTest,
       SocketCloseAfterFinishDoesNotEraseKnownFinish) {
  Seed();
  Finish();
  Drop(7);
  runner_.RunTasksUntilIdle();
  EXPECT_EQ(controller_.Snapshot().end_reason, "finished");
  EXPECT_FALSE(recovering());
}

TEST_F(MediaControllerRecoveryTest, StopCancelsQueuedReconnect) {
  Seed();
  Drop(7);
  runner_.RunTasksUntilIdle();
  controller_.StopAsync([](bool ok, const std::string&) { EXPECT_TRUE(ok); });
  runner_.RunTasksUntilIdle();
  clock_.Advance(std::chrono::seconds(20));
  runner_.RunTasksUntilIdle();
  EXPECT_FALSE(recovering());
  EXPECT_FALSE(has_client());
  EXPECT_EQ(controller_.Snapshot().end_reason, "stopped");
}

TEST_F(MediaControllerRecoveryTest, StaleSocketCannotEraseSuccessor) {
  Seed();
  Drop(6);
  runner_.RunTasksUntilIdle();
  EXPECT_TRUE(controller_.Snapshot().active);
  EXPECT_TRUE(controller_.Snapshot().end_reason.empty());
}

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

TEST_F(MediaControllerTest, StopBeforeQueuedConnectDoesNotUseDestroyedClient) {
  LoadRequest request;
  request.url = "http://media/one";
  controller_.LoadAsync("192.0.2.10", request, [](bool, const std::string&) {});
  controller_.StopAsync([](bool ok, const std::string&) { EXPECT_TRUE(ok); });
  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(controller_.Snapshot().active);
}

TEST_F(MediaControllerTest, SnapshotDefaultsToInactive) {
  EXPECT_FALSE(controller_.Snapshot().active);
}

openscreen::IPEndpoint Device(const char* ip) {
  return {openscreen::IPAddress::Parse(ip).value(), 8009};
}

TEST(ReattachIdentityTest, OnlyTheExactExistingMediaCanBeAttached) {
  EXPECT_TRUE(CanReattachMedia(42, "http://media/one", 42, "http://media/one"));
  EXPECT_FALSE(
      CanReattachMedia(42, "http://media/one", 43, "http://media/one"));
  EXPECT_FALSE(
      CanReattachMedia(42, "http://media/one", 42, "http://media/two"));
  EXPECT_FALSE(CanReattachMedia(0, "", 0, ""));
}

TEST(CanReuseSessionTest, SameDeviceAndRunningAppReuses) {
  const auto tv = Device("192.0.2.10");
  EXPECT_TRUE(CanReuseSession(true, true, tv, "", tv, ""));
  // An explicit Default Media Receiver id is the same app as the empty default.
  EXPECT_TRUE(
      CanReuseSession(true, true, tv, "", tv, kDefaultMediaReceiverAppId));
}

TEST(CanReuseSessionTest, AnythingElseReconnects) {
  const auto tv = Device("192.0.2.10");
  const auto other = Device("192.0.2.11");
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
