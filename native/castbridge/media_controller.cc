#include "cast/castbridge/media_controller.h"

#include <atomic>
#include <chrono>
#include <utility>

#include "cast/common/public/trust_store.h"
#include "platform/base/ip_address.h"

namespace castbridge {

MediaController::MediaController(openscreen::TaskRunner& task_runner)
    : task_runner_(task_runner) {}

MediaController::~MediaController() = default;

void MediaController::LoadAsync(std::string ip,
                                LoadRequest request,
                                Completion done) {
  task_runner_.PostTask([this, ip, request, done] {
    // Single-shot completion guard (success via on_ready, failure via on_closed
    // or timeout — whichever fires first wins). Returns true to the winner.
    auto guard = std::make_shared<std::atomic<bool>>(false);
    auto finish = [guard, done](bool ok, const std::string& err) {
      if (!guard->exchange(true)) {
        done(ok, err);
        return true;
      }
      return false;
    };

    const openscreen::ErrorOr<openscreen::IPAddress> addr =
        openscreen::IPAddress::Parse(ip);
    if (!addr.is_value()) {
      finish(false, "invalid device address");
      return;
    }
    const openscreen::IPEndpoint endpoint{addr.value(), 8009};

    if (client_ && client_->CanReload(endpoint, request)) {
      last_request_ = request;
      // Same device and app still running: LOAD on the live session. A
      // reconnect would relaunch the app and INTERRUPT the playing media.
      client_->Reload(
          request, [finish](bool ok, const std::string& e) { finish(ok, e); });
    } else {
      const uint64_t epoch = ++epoch_;
      if (client_ && had_session_ && !recovering_ && !closing_)
        client_->Shutdown();
      recovering_ = false;
      closing_ = false;
      reconnect_attempt_ = 0;
      had_session_ = false;
      last_request_ = request;
      last_endpoint_ = endpoint;
      client_.reset();
      client_ = std::make_unique<MediaReceiverClient>(
          task_runner_, openscreen::cast::CastTrustStore::Create(),
          [this, epoch](const MediaStatus& s) {
            if (epoch == epoch_)
              OnStatusUpdate(s);
          },
          [this, finish, epoch](const std::string& e) {
            finish(false, e);
            OnClosed(epoch);
          });
      client_->Connect(
          endpoint, request,
          [finish](bool ok, const std::string& e) { finish(ok, e); });
    }

    // On timeout, also drop the half-open session so a retry starts clean —
    // but only if this load's client is still the current one.
    const uint64_t guarded = epoch_;
    task_runner_.PostTaskWithDelay(
        [this, finish, guarded] {
          if (finish(false, "media load timed out") && epoch_ == guarded) {
            ++epoch_;
            client_.reset();
          }
        },
        std::chrono::seconds(12));
  });
}

void MediaController::ControlAsync(std::string cmd,
                                   double value,
                                   Completion done) {
  task_runner_.PostTask([this, cmd, value, done] {
    if (!client_ || recovering_) {
      done(false, "no active media session");
      return;
    }
    if (cmd == "volume") {
      client_->SetVolume(value);
    } else if (cmd == "mute") {
      client_->SetMuted(value > 0.5);
    } else {
      client_->Control(cmd, value);
    }
    done(true, "");
  });
}

void MediaController::StopAsync(Completion done) {
  task_runner_.PostTask([this, done] {
    ++epoch_;  // cancel every queued reconnect and timeout
    const bool verified = had_session_ && !recovering_ && !closing_;
    recovering_ = false;
    had_session_ = false;
    if (client_) {
      client_->Shutdown(verified);  // explicit stop is the only transport
                                    // teardown that sends STOP
      client_.reset();
    }
    bool announce;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      announce = status_.active || !status_.end_reason.empty();
      status_.active = false;
      status_.state = "IDLE";
      status_.end_reason = announce ? "stopped" : "";
      status_.error.clear();
    }
    if (announce && on_change_)
      on_change_();
    done(true, "");
  });
}

void MediaController::OnClosed(uint64_t epoch) {
  if (epoch != epoch_ || closing_)
    return;
  closing_ = true;
  const bool retry = had_session_;
  if (retry) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.active = false;
      status_.state = "UNKNOWN";
      status_.end_reason = "disconnected";
    }
    if (on_status_)
      on_status_(Snapshot());
    if (on_change_)
      on_change_();
  }
  // Never destroy the client while executing its own socket callback.
  task_runner_.PostTask([this, epoch, retry] {
    if (epoch != epoch_)
      return;
    client_.reset();  // destructor closes transport without sending STOP
    closing_ = false;
    recovering_ = retry;
    if (retry)
      ScheduleReconnect(epoch);
  });
}

void MediaController::ScheduleReconnect(uint64_t epoch) {
  if (!recovering_ || epoch != epoch_ || reconnect_attempt_ >= 3)
    return;
  const int delay = 1 << reconnect_attempt_++;
  task_runner_.PostTaskWithDelay(
      [this, epoch] {
        if (epoch != epoch_ || !recovering_)
          return;
        const uint64_t attempt = ++epoch_;
        client_ = std::make_unique<MediaReceiverClient>(
            task_runner_, openscreen::cast::CastTrustStore::Create(),
            [this, attempt](const MediaStatus& s) {
              if (attempt == epoch_)
                OnStatusUpdate(s);
            },
            [this, attempt](const std::string&) { OnClosed(attempt); });
        client_->Attach(last_endpoint_, last_request_, last_media_session_,
                        last_app_session_,
                        [this, attempt](bool ok, const std::string&) {
                          if (attempt != epoch_)
                            return;
                          if (!ok) {
                            OnClosed(attempt);
                          } else {
                            recovering_ = false;
                            reconnect_attempt_ = 0;
                          }
                        });
        task_runner_.PostTaskWithDelay(
            [this, attempt] {
              if (attempt == epoch_ && recovering_)
                OnClosed(attempt);
            },
            std::chrono::seconds(12));
      },
      std::chrono::seconds(delay));
}

void MediaController::OnStatusUpdate(const MediaStatus& status) {
  if (status.active) {
    had_session_ = true;
    last_media_session_ = status.media_session_id;
    if (client_)
      last_app_session_ = client_->receiver_session_id();
  } else if (!status.end_reason.empty() || !status.error.empty()) {
    had_session_ = false;
    recovering_ = false;
  }
  bool was_active;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_active = status_.active;
    status_ = status;
  }
  if (on_status_) {
    on_status_(status);
  }
  // Playback ended (IDLE/FINISHED) → announce the session change once.
  if (was_active && !status.active && on_change_) {
    on_change_();
  }
}

void MediaController::ResetClient() {
  ++epoch_;
  recovering_ = false;
  had_session_ = false;
  client_.reset();
}

MediaStatus MediaController::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

}  // namespace castbridge
