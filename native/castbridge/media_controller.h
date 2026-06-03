// Manages the single active media session. Bridges the IPC thread (which calls
// the *Async methods) to the openscreen TaskRunner thread (which owns the
// MediaReceiverClient). Completions and status updates fire on the TaskRunner
// thread; callers route them to the thread-safe IpcServer::Send/Broadcast.
#ifndef CAST_CASTBRIDGE_MEDIA_CONTROLLER_H_
#define CAST_CASTBRIDGE_MEDIA_CONTROLLER_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "cast/castbridge/media_receiver_client.h"
#include "platform/api/task_runner.h"

namespace castbridge {

class MediaController {
 public:
  // ok=false carries an error message.
  using Completion = std::function<void(bool ok, const std::string& error)>;
  using StatusBroadcast = std::function<void(const MediaStatus&)>;

  explicit MediaController(openscreen::TaskRunner& task_runner);
  ~MediaController();

  void set_on_status(StatusBroadcast cb) { on_status_ = std::move(cb); }
  // Fired when the active state changes (start / stop / natural end).
  void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

  void LoadAsync(std::string ip, LoadRequest request, Completion done);
  void ControlAsync(std::string cmd, double value, Completion done);
  void StopAsync(Completion done);

  // Destroy the cast client. MUST be called on the TaskRunner thread (openscreen
  // objects assert thread affinity on destruction). Used for clean shutdown.
  void ResetClient() { client_.reset(); }

  MediaStatus Snapshot() const;

 private:
  void OnStatusUpdate(const MediaStatus& status);
  void OnClosed();

  openscreen::TaskRunner& task_runner_;
  StatusBroadcast on_status_;
  std::function<void()> on_change_;

  // Owned and used only on the TaskRunner thread.
  std::unique_ptr<MediaReceiverClient> client_;

  mutable std::mutex mutex_;
  MediaStatus status_;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_MEDIA_CONTROLLER_H_
