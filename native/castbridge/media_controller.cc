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

    client_.reset();  // tear down any prior session

    const openscreen::ErrorOr<openscreen::IPAddress> addr =
        openscreen::IPAddress::Parse(ip);
    if (!addr.is_value()) {
      finish(false, "invalid device address");
      return;
    }
    const openscreen::IPEndpoint endpoint{addr.value(), 8009};

    client_ = std::make_unique<MediaReceiverClient>(
        task_runner_, openscreen::cast::CastTrustStore::Create(),
        [this](const MediaStatus& s) { OnStatusUpdate(s); },
        [this, finish](const std::string& e) {
          finish(false, e);
          OnClosed();
        });
    client_->Connect(
        endpoint, request,
        [finish](bool ok, const std::string& e) { finish(ok, e); });

    // On timeout, also drop the half-open session so a retry starts clean —
    // but only if this load's client is still the current one.
    MediaReceiverClient* const guarded = client_.get();
    task_runner_.PostTaskWithDelay(
        [this, finish, guarded] {
          if (finish(false, "media load timed out") &&
              client_.get() == guarded) {
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
    if (!client_) {
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
    bool was_active;
    if (client_) {
      client_->Shutdown();
      client_.reset();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      was_active = status_.active;
      status_ = {};
    }
    if (was_active && on_change_) {
      on_change_();
    }
    done(true, "");
  });
}

void MediaController::OnClosed() {
  // Socket closed (TV off / network) after a session was active. The deferred
  // reset must only destroy the client that closed — never a successor a newer
  // LoadAsync created in the meantime.
  bool was_active;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_active = status_.active;
    status_ = {};
  }
  MediaReceiverClient* const dying = client_.get();
  task_runner_.PostTask([this, dying] {
    if (client_.get() == dying) {
      client_.reset();
    }
  });
  if (was_active && on_change_) {
    on_change_();
  }
}

void MediaController::OnStatusUpdate(const MediaStatus& status) {
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

MediaStatus MediaController::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

}  // namespace castbridge
