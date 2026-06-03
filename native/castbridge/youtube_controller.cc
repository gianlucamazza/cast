#include "cast/castbridge/youtube_controller.h"

#include <atomic>
#include <chrono>
#include <utility>

#include "cast/common/public/trust_store.h"
#include "platform/base/ip_address.h"

namespace castbridge {

YouTubeController::YouTubeController(openscreen::TaskRunner& task_runner)
    : task_runner_(task_runner), worker_(&YouTubeController::WorkerLoop, this) {}

YouTubeController::~YouTubeController() {
  {
    std::lock_guard<std::mutex> lock(q_mutex_);
    worker_stop_ = true;
  }
  q_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void YouTubeController::Enqueue(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(q_mutex_);
    queue_.push_back(std::move(job));
  }
  q_cv_.notify_one();
}

void YouTubeController::WorkerLoop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(q_mutex_);
      q_cv_.wait(lock, [this] { return worker_stop_ || !queue_.empty(); });
      if (worker_stop_ && queue_.empty()) {
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    job();  // Lounge HTTP (blocking curl) — off the IPC/TaskRunner threads.
  }
}

void YouTubeController::SetActive(bool active) {
  const bool was = active_.exchange(active);
  if (was != active && on_change_) {
    on_change_();
  }
}

std::string YouTubeController::title() const {
  return active_.load() ? "YouTube" : "";
}

void YouTubeController::LoadAsync(std::string ip,
                                  std::string video_id,
                                  double start_time,
                                  Completion done) {
  auto guard = std::make_shared<std::atomic<bool>>(false);
  auto finish = [guard, done](bool ok, const std::string& err) {
    if (!guard->exchange(true)) {
      done(ok, err);
    }
  };

  task_runner_.PostTask([this, ip, video_id, start_time, finish] {
    client_.reset();
    const openscreen::ErrorOr<openscreen::IPAddress> addr =
        openscreen::IPAddress::Parse(ip);
    if (!addr.is_value()) {
      finish(false, "invalid device address");
      return;
    }
    const openscreen::IPEndpoint endpoint{addr.value(), 8009};

    client_ = std::make_unique<YouTubeCastClient>(
        task_runner_, openscreen::cast::CastTrustStore::Create(),
        [this, finish](const std::string& e) {
          // Channel closed: fail a pending load, else treat as natural end.
          finish(false, e);
          if (active_.load()) {
            SetActive(false);
            Enqueue([this] { lounge_.reset(); });
          }
        });

    client_->Connect(endpoint, [this, video_id, start_time, finish](
                                   bool ok, const std::string& screen_id,
                                   const std::string& err) {
      if (!ok) {
        finish(false, err);
        return;
      }
      Enqueue([this, screen_id, video_id, start_time, finish] {
        lounge_ = std::make_unique<YouTubeLounge>();
        const std::string e = lounge_->Start(screen_id, video_id, start_time);
        if (e.empty()) {
          SetActive(true);
        }
        finish(e.empty(), e);
      });
    });

    task_runner_.PostTaskWithDelay(
        [finish] { finish(false, "youtube load timed out"); },
        std::chrono::seconds(15));
  });
}

void YouTubeController::ControlAsync(std::string cmd,
                                     double value,
                                     Completion done) {
  if (!active_.load()) {
    done(false, "no active YouTube session");
    return;
  }
  if (cmd == "volume" || cmd == "mute") {
    task_runner_.PostTask([this, cmd, value, done] {
      if (client_) {
        if (cmd == "volume") {
          client_->SetVolume(value);
        } else {
          client_->SetMuted(value > 0.5);
        }
      }
      done(true, "");
    });
    return;
  }
  std::string sc;
  if (cmd == "play") {
    sc = "play";
  } else if (cmd == "pause") {
    sc = "pause";
  } else if (cmd == "seek") {
    sc = "seekTo";
  } else {
    done(false, "unsupported command");
    return;
  }
  Enqueue([this, sc, value, done] {
    if (!lounge_) {
      done(false, "no active YouTube session");
      return;
    }
    const std::string e = lounge_->Command(sc, value);
    done(e.empty(), e);
  });
}

void YouTubeController::StopAsync(Completion done) {
  // Tell the TV's YouTube app to stop (clearPlaylist) on the worker thread, then
  // close the Cast channel on the TaskRunner thread.
  Enqueue([this, done] {
    if (lounge_) {
      lounge_->Command("clearPlaylist", 0);
      lounge_.reset();
    }
    SetActive(false);
    task_runner_.PostTask([this] {
      if (client_) {
        client_->Shutdown();
        client_.reset();
      }
    });
    done(true, "");
  });
}

}  // namespace castbridge
