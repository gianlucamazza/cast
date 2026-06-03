// YouTube Lounge HTTP client. Given a screenId (from the MDX handshake), gets a
// lounge token, binds a session (SID/gsessionid), and drives the TV's YouTube
// app (setPlaylist + play/pause/seek). HTTPS is done via the `curl` binary as a
// managed subprocess — this avoids linking libcurl(→openssl) alongside
// openscreen's boringssl in the same binary. Blocking; call from a worker thread.
#ifndef CAST_CASTBRIDGE_YOUTUBE_LOUNGE_H_
#define CAST_CASTBRIDGE_YOUTUBE_LOUNGE_H_

#include <string>
#include <vector>

namespace castbridge {

class YouTubeLounge {
 public:
  // Get token + bind + setPlaylist(video_id @ start_time). Returns "" on
  // success or an error message.
  std::string Start(const std::string& screen_id,
                    const std::string& video_id,
                    double start_time);

  // Lounge playback command (e.g. "play", "pause"); "seekTo" uses new_time.
  std::string Command(const std::string& sc, double new_time);

  bool valid() const { return !sid_.empty(); }

 private:
  std::string GetToken(const std::string& screen_id, std::string* error);
  std::string Bind(std::string* error);            // sets sid_/gsessionid_
  std::string SendCommand(const std::vector<std::string>& req_fields,
                          int* http_code,
                          std::string* error);     // bc/bind with SID/gsessionid
  std::string Refresh();  // re-token + re-bind for the saved screen

  std::string token_;
  std::string sid_;
  std::string gsessionid_;
  std::string screen_id_;  // remembered so we can re-auth on token expiry
  int rid_ = 1;
  int ofs_ = 0;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_YOUTUBE_LOUNGE_H_
