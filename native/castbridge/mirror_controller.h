// Manages a live screen/window mirror session by running the fork's native
// `cast_sender` binary as a child process (H.264 VAAPI Cast Streaming to the
// receiver). The daemon owns discovery, window resolution, process lifecycle
// and IPC; this is the native fork sender, not a skill-cast wrapper.
//
// (A future refactor can link LoopingFileCastAgent in-process; the IPC contract
// does not change.)
#ifndef CAST_CASTBRIDGE_MIRROR_CONTROLLER_H_
#define CAST_CASTBRIDGE_MIRROR_CONTROLLER_H_

#include <sys/types.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace castbridge {

class MirrorController {
 public:
  struct Status {
    bool active = false;
    std::string mode;    // "window" | "output"
    std::string target;  // window title/class or output name
    std::string device;  // device name
  };

  ~MirrorController();

  // Fired when the active state changes (start / stop / process death).
  void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

  // Start mirroring. Returns an empty string on success, or an error message.
  std::string StartWindow(const std::string& ip,
                          const std::string& address,
                          int audio_pid,
                          const std::string& audio_app,
                          const std::string& label,
                          const std::string& device);
  std::string StartScreen(const std::string& ip,
                          const std::string& output,
                          const std::string& device);

  void Stop();
  // Stop without blocking the caller (safe from the IPC thread). The worker is
  // tracked and joined in the destructor, so it cannot outlive this object.
  void StopAsync();
  Status GetStatus();

 private:
  std::string Launch(const std::string& ip,
                     const std::string& target,
                     int audio_pid,
                     const std::string& audio_app,
                     const std::string& mode,
                     const std::string& label,
                     const std::string& device);
  void StopInternal();
  void Monitor(pid_t pid, uint64_t gen);

  std::function<void()> on_change_;
  std::mutex
      lifecycle_mutex_;  // serializes Start/Stop (held across blocking ops)
  std::mutex mutex_;     // guards pid_/gen_/status_ (fast; used by GetStatus)
  pid_t pid_ = -1;
  uint64_t gen_ = 0;  // bumped on each start/stop to invalidate stale monitors
  Status status_;
  std::thread monitor_;
  std::thread async_stop_;  // in-flight StopAsync worker (joined in dtor)
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_MIRROR_CONTROLLER_H_
