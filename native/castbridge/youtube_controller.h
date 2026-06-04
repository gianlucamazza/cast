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
  using StatusBroadcast = std::function<void(const YouTubeStatus&)>;

  explicit YouTubeController(openscreen::TaskRunner& task_runner);
  ~YouTubeController();

  // Fired whenever the active state changes (start / stop / natural end).
  void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }
  // Fired when the parsed playback state changes (PLAYING <-> PAUSED, etc.).
  void set_on_status(StatusBroadcast cb) { on_status_ = std::move(cb); }

  void LoadAsync(std::string ip, std::string video_id, double start_time, Completion done);
  void ControlAsync(std::string cmd, double value, Completion done);
  void StopAsync(Completion done);

  // Destroy the cast client. MUST be called on the TaskRunner thread. Used for
  // clean shutdown.
  void ResetClient() { client_.reset(); }

  bool active() const { return active_.load(); }
  std::string title() const;

  // Thread-safe snapshot of the last parsed playback status.
  YouTubeStatus Snapshot() const;

 private:
  void Enqueue(std::function<void()> job);
  void WorkerLoop();
  void SetActive(bool active);

  // Event-channel polling (runs on its own thread; see youtube_lounge Poll()).
  void PollLoop();
  void OnStatusUpdate(const YouTubeStatus& status);
  std::shared_ptr<YouTubeLounge> SnapshotLounge() const;

  openscreen::TaskRunner& task_runner_;
  std::function<void()> on_change_;
  StatusBroadcast on_status_;

  std::unique_ptr<YouTubeCastClient> client_;  // TaskRunner thread only
  // The Lounge client is created on the worker and read by the poll thread, so
  // it is a shared_ptr guarded by lounge_mutex_: a poll in flight keeps the
  // object alive even if the worker resets the member.
  mutable std::mutex lounge_mutex_;
  std::shared_ptr<YouTubeLounge> lounge_;

  std::atomic<bool> active_{false};

  mutable std::mutex status_mutex_;
  YouTubeStatus status_;

  // Single-worker job queue for all Lounge HTTP commands.
  std::mutex q_mutex_;
  std::condition_variable q_cv_;
  std::deque<std::function<void()>> queue_;
  bool worker_stop_ = false;
  std::thread worker_;

  // Poll thread + its run/stop signalling.
  std::mutex poll_mutex_;
  std::condition_variable poll_cv_;
  bool poll_active_ = false;  // a session is live; keep polling
  bool poll_stop_ = false;    // shutting down
  std::thread poll_thread_;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_YOUTUBE_CONTROLLER_H_
