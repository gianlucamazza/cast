// Orchestrates native YouTube casting: resolves the screenId over the Cast
// channel (YouTubeCastClient, on the TaskRunner thread), then drives playback
// via the Lounge HTTP API (YouTubeLounge). All Lounge calls run on ONE dedicated
// worker thread (a job queue) so they never block the IPC/TaskRunner threads and
// the session state (rid/ofs/sid) stays monotonic and single-threaded. One
// active session at a time.
#ifndef CAST_CASTBRIDGE_YOUTUBE_CONTROLLER_H_
#define CAST_CASTBRIDGE_YOUTUBE_CONTROLLER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cast/castbridge/youtube_cast_client.h"
#include "cast/castbridge/youtube_lounge.h"
#include "platform/api/task_runner.h"

namespace castbridge {

class YouTubeController {
 public:
  using Completion = std::function<void(bool ok, const std::string& error)>;

  explicit YouTubeController(openscreen::TaskRunner& task_runner);
  ~YouTubeController();

  // Fired whenever the active state changes (start / stop / natural end).
  void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

  void LoadAsync(std::string ip, std::string video_id, double start_time, Completion done);
  void ControlAsync(std::string cmd, double value, Completion done);
  void StopAsync(Completion done);

  // Destroy the cast client. MUST be called on the TaskRunner thread. Used for
  // clean shutdown.
  void ResetClient() { client_.reset(); }

  bool active() const { return active_.load(); }
  std::string title() const;

 private:
  void Enqueue(std::function<void()> job);
  void WorkerLoop();
  void SetActive(bool active);

  openscreen::TaskRunner& task_runner_;
  std::function<void()> on_change_;

  std::unique_ptr<YouTubeCastClient> client_;  // TaskRunner thread only
  std::unique_ptr<YouTubeLounge> lounge_;      // worker thread only

  std::atomic<bool> active_{false};

  // Single-worker job queue for all Lounge HTTP work.
  std::mutex q_mutex_;
  std::condition_variable q_cv_;
  std::deque<std::function<void()>> queue_;
  bool worker_stop_ = false;
  std::thread worker_;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_YOUTUBE_CONTROLLER_H_
