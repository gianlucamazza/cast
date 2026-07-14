// Native Cast media client. Connects to a receiver (TLS:8009), launches the
// default media receiver app (CC1AD845), then LOADs a media URL and drives
// playback (play/pause/seek/stop/volume) on the media namespace — the native
// equivalent of what catt did, with no skill-cast dependency.
//
// The Cast-channel plumbing (socket, launch handshake, heartbeat,
// RECEIVER_STATUS, volume, teardown) lives in CastChannelClient; this class
// adds the media namespace. Runs on the openscreen TaskRunner thread.
#ifndef CAST_CASTBRIDGE_MEDIA_RECEIVER_CLIENT_H_
#define CAST_CASTBRIDGE_MEDIA_RECEIVER_CLIENT_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cast/castbridge/cast_channel_client.h"

namespace Json {
class Value;
}

namespace castbridge {

// The Google-defined Default Media Receiver application id — the fallback receiver launched
// when a LoadRequest carries no app_id override (ADR 0013 nstream).
inline constexpr const char* kDefaultMediaReceiverAppId = "CC1AD845";

struct MediaStatus {
  bool active = false;
  std::string state;  // PLAYING | PAUSED | BUFFERING | IDLE
  std::string title;
  double position = 0;
  double duration = 0;
  int media_session_id = 0;
  // Track ids the receiver reports as active (MEDIA_STATUS.activeTrackIds). For a
  // side-loaded caption track (trackId 1) this is the receiver's *confirmation* that
  // it fetched and activated the WebVTT — something the send side cannot know. See
  // ADR 0016 (nstream): surfaces the receiver's real track state.
  std::vector<int> active_track_ids;
  // Non-empty when the receiver reported a failure: an idleReason of ERROR, or a
  // media-namespace LOAD_FAILED / INVALID_REQUEST / ERROR message. Surfaced so a codec
  // or caption rejection is diagnosable instead of reading as a silent success.
  std::string error;
};

struct LoadRequest {
  std::string url;
  std::string content_type;  // e.g. "video/mp4"; empty -> receiver guesses
  std::string title;
  double current_time = 0;
  // Optional now-playing metadata shown on the receiver's media screen (and,
  // via the protocol-level HUD bridge, on the desktop cast widget). When
  // series_title is set the LOAD carries a TvShow metadata block, else when
  // poster/subtitle is set a Movie block, else the bare title (back-compat).
  // poster is a public image URL the receiver fetches.
  std::string poster;
  std::string subtitle;
  std::string series_title;
  int season = 0;
  int episode = 0;
  // Optional side-loaded caption track. When subtitle_url is set the LOAD carries
  // a single TEXT/SUBTITLES track and marks it active, so the Default Media
  // Receiver renders captions natively (WebVTT, fetched by the receiver over the
  // LAN — the server must send CORS headers). subtitle_lang is a BCP-47/ISO code
  // (e.g. "en"), subtitle_name the human label shown in the caption menu.
  std::string subtitle_url;
  std::string subtitle_lang;
  std::string subtitle_name;
  // Optional Cast receiver application id to launch instead of the Default Media Receiver
  // (CC1AD845). Empty → CC1AD845 (unchanged). A registered custom CAF receiver (ADR 0013
  // nstream) can enable AC-3/E-AC-3 passthrough + native track selection; this is the enabling
  // plumbing — dormant until a caller sets it and the receiver is registered + hosted.
  std::string app_id;
};

class MediaReceiverClient final : public CastChannelClient {
 public:
  using StatusCallback = std::function<void(const MediaStatus&)>;
  using ReadyCallback = std::function<void(bool ok, const std::string& error)>;

  MediaReceiverClient(openscreen::TaskRunner& task_runner,
                      std::unique_ptr<openscreen::cast::TrustStore> trust_store,
                      StatusCallback on_status,
                      ClosedCallback on_closed);
  ~MediaReceiverClient() override;

  // Connect, launch CC1AD845, and LOAD the request. on_ready fires once.
  void Connect(const openscreen::IPEndpoint& endpoint,
               LoadRequest request,
               ReadyCallback on_ready);

  // Playback controls (no-ops until a media session exists).
  void Control(const std::string& cmd, double value);  // play|pause|stop|seek

 protected:
  // The launched receiver app: the request's override if set, else the Default Media Receiver.
  // request_ is populated by Connect() before the handshake calls this (ADR 0013 plumbing).
  const char* app_id() const override {
    return request_.app_id.empty() ? kDefaultMediaReceiverAppId
                                   : request_.app_id.c_str();
  }
  const char* app_name() const override { return "media app"; }
  const char* local_id_prefix() const override { return "media-sender"; }
  void OnAppConnectionOpened(bool success) override;
  void OnAppMessage(const std::string& ns,
                    const std::string& type,
                    const Json::Value& body) override;
  void OnConnectError(const std::string& error) override;
  void OnBeforeShutdown() override;  // best-effort STOP before teardown

 private:
  void SendLoad();
  void HandleMediaStatus(const Json::Value& payload);
  void HandleMediaError(const std::string& type, const Json::Value& body);
  void FireReady(bool ok, const std::string& error);

  StatusCallback on_status_;
  ReadyCallback on_ready_;  // one-shot

  LoadRequest request_;
  bool loaded_ = false;
  int media_session_id_ = 0;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_MEDIA_RECEIVER_CLIENT_H_
