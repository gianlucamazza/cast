// YouTube Lounge HTTP client. Given a screenId (from the MDX handshake), gets a
// lounge token, binds a session (SID/gsessionid), and drives the TV's YouTube
// app (setPlaylist + play/pause/seek). HTTPS is done via the `curl` binary as a
// managed subprocess — this avoids linking libcurl(→openssl) alongside
// openscreen's boringssl in the same binary. Blocking; call from a worker
// thread.
#ifndef CAST_CASTBRIDGE_YOUTUBE_LOUNGE_H_
#define CAST_CASTBRIDGE_YOUTUBE_LOUNGE_H_

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace castbridge {

// Snapshot of the TV's YouTube playback, parsed from the Lounge event stream.
// `state` uses the same vocabulary as the URL receiver (MediaStatus) so the
// extension treats both alike: PLAYING | PAUSED | BUFFERING | IDLE.
struct YouTubeStatus {
  bool active = false;
  std::string state;
  std::string title;
  std::string video_id;
  double position = 0;
  double duration = 0;
};

// Result of a single Poll(): whether a usable status was parsed, plus a hint to
// the caller when the session must be re-authenticated (on the command thread).
enum class PollResult {
  kNoChange,     // poll returned, nothing new (timeout / non-status frames)
  kStatus,       // *out was populated with a fresh status
  kNeedRefresh,  // session expired (HTTP 4xx); caller must Refresh() then retry
  kError,        // transient error; back off and retry
};

class YouTubeLounge {
 public:
  // Get token + bind + setPlaylist(video_id @ start_time). Returns "" on
  // success or an error message.
  std::string Start(const std::string& screen_id,
                    const std::string& video_id,
                    double start_time);

  // Lounge playback command (e.g. "play", "pause"); "seekTo" uses new_time.
  std::string Command(const std::string& sc, double new_time);

  // One bounded long-poll GET on the event channel. Parses Comet frames and, on
  // a playback event, fills *out. Safe to call from a thread other than the one
  // running Command()/Start() (reads sid_/gsessionid_ under a lock). Blocks up
  // to ~30s. `error` gets a human-readable message on kError/kNeedRefresh.
  PollResult Poll(YouTubeStatus* out, std::string* error);

  // Re-fetch token + re-bind for the saved screen. Call from the command
  // thread.
  std::string Refresh();

  bool valid() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return !sid_.empty();
  }

 private:
  std::string GetToken(const std::string& screen_id, std::string* error);
  std::string Bind(std::string* error);  // sets sid_/gsessionid_
  std::string SendCommand(const std::vector<std::string>& req_fields,
                          int* http_code,
                          std::string* error);  // bc/bind with SID/gsessionid

  // Guards sid_/gsessionid_/token_: written by Bind()/Refresh() on the command
  // thread, read by Poll() on the poll thread.
  mutable std::mutex session_mutex_;
  std::string token_;
  std::string sid_;
  std::string gsessionid_;

  std::string screen_id_;  // remembered so we can re-auth on token expiry
  int rid_ = 1;
  int ofs_ = 0;
  // Last acknowledged event index, sent on the next poll GET. Written by both
  // the command thread (Bind resets to 0) and the poll thread, hence atomic.
  std::atomic<int> aid_{0};
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_YOUTUBE_LOUNGE_H_
