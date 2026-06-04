#include "cast/castbridge/youtube_controller.h"

#include <atomic>
#include <chrono>
#include <utility>

#include "cast/common/public/trust_store.h"
#include "platform/base/ip_address.h"

namespace castbridge {

YouTubeController::YouTubeController(openscreen::TaskRunner& task_runner)
    : task_runner_(task_runner),
      worker_(&YouTubeController::WorkerLoop, this),
      poll_thread_(&YouTubeController::PollLoop, this) {}

YouTubeController::~YouTubeController() {
  {
    std::lock_guard<std::mutex> lock(q_mutex_);
    worker_stop_ = true;
  }
  q_cv_.notify_all();
  {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    poll_stop_ = true;
  }
  poll_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  if (poll_thread_.joinable()) {
    poll_thread_.join();
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

std::shared_ptr<YouTubeLounge> YouTubeController::SnapshotLounge() const {
  std::lock_guard<std::mutex> lock(lounge_mutex_);
  return lounge_;
}

void YouTubeController::SetActive(bool active) {
  const bool was = active_.exchange(active);
  if (was == active) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(poll_mutex_);
    poll_active_ = active;
  }
  poll_cv_.notify_all();
  if (on_change_) {
    on_change_();
  }
}

std::string YouTubeController::title() const {
  if (!active_.load()) {
    return "";
  }
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_.title.empty() ? "YouTube" : status_.title;
}

YouTubeStatus YouTubeController::Snapshot() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_;
}

// Poll the Lounge event channel while a session is live and translate parsed
// frames into status updates. Runs on its own thread so the ~30s long-poll GET
// never blocks command dispatch on the worker thread.
void YouTubeController::PollLoop() {
  using namespace std::chrono_literals;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(poll_mutex_);
      poll_cv_.wait(lock, [this] { return poll_stop_ || poll_active_; });
      if (poll_stop_) {
        return;
      }
    }
    std::shared_ptr<YouTubeLounge> lg = SnapshotLounge();
    if (!lg) {
      std::unique_lock<std::mutex> lock(poll_mutex_);
      poll_cv_.wait_for(lock, 200ms,
                        [this] { return poll_stop_ || !poll_active_; });
      continue;
    }

    YouTubeStatus st;
    std::string err;
    const PollResult r = lg->Poll(&st, &err);

    // A stop may have raced with the in-flight poll; drop late updates.
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      if (!poll_active_ || poll_stop_) {
        continue;
      }
    }

    switch (r) {
      case PollResult::kStatus:
        OnStatusUpdate(st);
        break;
      case PollResult::kNeedRefresh:
        // Re-auth on the worker thread (owns rid/ofs); back off a beat.
        Enqueue([this] {
          std::shared_ptr<YouTubeLounge> l = SnapshotLounge();
          if (l) {
            l->Refresh();
          }
        });
        {
          std::unique_lock<std::mutex> lock(poll_mutex_);
          poll_cv_.wait_for(lock, 1s,
                            [this] { return poll_stop_ || !poll_active_; });
        }
        break;
      case PollResult::kError: {
        std::unique_lock<std::mutex> lock(poll_mutex_);
        poll_cv_.wait_for(lock, 2s,
                          [this] { return poll_stop_ || !poll_active_; });
        break;
      }
      case PollResult::kNoChange:
        break;
    }
  }
}

void YouTubeController::OnStatusUpdate(const YouTubeStatus& status) {
  bool state_changed;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    state_changed = status_.state != status.state;
    status_ = status;
  }
  // A natural end (IDLE) ends the session even if the Cast channel is still up.
  if (status.state == "IDLE" && active_.load()) {
    SetActive(false);  // fires on_change_ → session broadcast
    Enqueue([this] {
      std::lock_guard<std::mutex> lock(lounge_mutex_);
      lounge_.reset();
    });
    return;
  }
  // Coalesce: only push on a real state transition, not on every position tick.
  if (state_changed && on_status_) {
    on_status_(status);
  }
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
            Enqueue([this] {
              std::lock_guard<std::mutex> lock(lounge_mutex_);
              lounge_.reset();
            });
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
        auto lounge = std::make_shared<YouTubeLounge>();
        {
          std::lock_guard<std::mutex> lock(lounge_mutex_);
          lounge_ = lounge;
        }
        const std::string e = lounge->Start(screen_id, video_id, start_time);
        if (e.empty()) {
          {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_ = YouTubeStatus{};
            status_.active = true;
            status_.state = "PLAYING";  // optimistic until the first poll frame
            status_.video_id = video_id;
            status_.position = start_time;
          }
          SetActive(true);  // also starts polling
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
    std::shared_ptr<YouTubeLounge> lg = SnapshotLounge();
    if (!lg) {
      done(false, "no active YouTube session");
      return;
    }
    const std::string e = lg->Command(sc, value);
    done(e.empty(), e);
  });
}

void YouTubeController::StopAsync(Completion done) {
  // Tell the TV's YouTube app to stop (clearPlaylist) on the worker thread, then
  // close the Cast channel on the TaskRunner thread.
  Enqueue([this, done] {
    std::shared_ptr<YouTubeLounge> lg = SnapshotLounge();
    if (lg) {
      lg->Command("clearPlaylist", 0);
    }
    {
      std::lock_guard<std::mutex> lock(lounge_mutex_);
      lounge_.reset();
    }
    SetActive(false);  // also stops polling
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
