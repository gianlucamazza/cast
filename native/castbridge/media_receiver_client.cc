#include "cast/castbridge/media_receiver_client.h"

#include <utility>

#include "cast/common/channel/message_util.h"
#include "json/value.h"
#include "util/json/json_serialization.h"
#include "util/osp_logging.h"

namespace castbridge {

namespace {

using openscreen::cast::kMediaNamespace;
using openscreen::cast::kMessageKeyStatus;

std::string Stringify(const Json::Value& v) {
  auto r = openscreen::json::Stringify(v);
  return r.is_value() ? r.value() : std::string();
}

const std::string& EffectiveAppId(const std::string& app_id) {
  static const std::string kDefault(kDefaultMediaReceiverAppId);
  return app_id.empty() ? kDefault : app_id;
}

}  // namespace

bool CanReuseSession(bool app_running,
                     bool loaded,
                     const openscreen::IPEndpoint& current,
                     const std::string& current_app_id,
                     const openscreen::IPEndpoint& target,
                     const std::string& target_app_id) {
  return app_running && loaded && current == target &&
         EffectiveAppId(current_app_id) == EffectiveAppId(target_app_id);
}

MediaReceiverClient::MediaReceiverClient(
    openscreen::TaskRunner& task_runner,
    std::unique_ptr<openscreen::cast::TrustStore> trust_store,
    StatusCallback on_status,
    ClosedCallback on_closed)
    : CastChannelClient(task_runner,
                        std::move(trust_store),
                        std::move(on_closed)),
      on_status_(std::move(on_status)) {}

MediaReceiverClient::~MediaReceiverClient() {
  Shutdown();
}

void MediaReceiverClient::Connect(const openscreen::IPEndpoint& endpoint,
                                  LoadRequest request,
                                  ReadyCallback on_ready) {
  request_ = std::move(request);
  on_ready_ = std::move(on_ready);
  ConnectInternal(endpoint);
}

bool MediaReceiverClient::CanReload(const openscreen::IPEndpoint& endpoint,
                                    const LoadRequest& request) const {
  return CanReuseSession(app_running(), loaded_ && on_ready_ == nullptr,
                         this->endpoint(), request_.app_id, endpoint,
                         request.app_id);
}

void MediaReceiverClient::Reload(LoadRequest request, ReadyCallback on_ready) {
  superseded_session_id_ = media_session_id_;
  request_ = std::move(request);
  on_ready_ = std::move(on_ready);
  loaded_ = false;
  SendLoad();
}

void MediaReceiverClient::OnAppConnectionOpened(bool success) {
  if (!success) {
    FireReady(false, "failed to open media app connection");
    return;
  }
  SendLoad();
}

void MediaReceiverClient::OnAppMessage(const std::string& ns,
                                       const std::string& type,
                                       const Json::Value& body) {
  if (ns != kMediaNamespace) {
    return;
  }
  if (type == "MEDIA_STATUS") {
    HandleMediaStatus(body);
  } else if (type == "LOAD_FAILED" || type == "INVALID_REQUEST" ||
             type == "ERROR") {
    // The receiver rejected the LOAD (bad codec, unreachable/invalid media, a
    // caption it couldn't fetch): surface it instead of hanging until timeout.
    HandleMediaError(type, body);
  }
}

void MediaReceiverClient::OnConnectError(const std::string& error) {
  FireReady(false, error);
}

void MediaReceiverClient::OnBeforeShutdown() {
  if (!has_app_connection()) {
    return;
  }
  // Best-effort: stop playback before tearing down.
  Json::Value m(Json::objectValue);
  m["type"] = "STOP";
  m["requestId"] = NextRequestId();
  m["mediaSessionId"] = media_session_id_;
  SendToApp(std::string(kMediaNamespace), Stringify(m));
}

void MediaReceiverClient::SendLoad() {
  Json::Value media(Json::objectValue);
  media["contentId"] = request_.url;
  media["streamType"] = "BUFFERED";
  media["contentType"] =
      request_.content_type.empty() ? "video/mp4" : request_.content_type;
  // Metadata type per Google Cast: 0 Generic, 1 Movie, 2 TvShow. Choose the
  // richest block the request supports so the receiver shows a real now-playing
  // card (title + poster), and the HUD bridge reads a real title.
  Json::Value meta(Json::objectValue);
  if (!request_.series_title.empty()) {
    meta["metadataType"] = 2;  // TvShowMediaMetadata
    meta["seriesTitle"] = request_.series_title;
    if (request_.season > 0) {
      meta["season"] = request_.season;
    }
    if (request_.episode > 0) {
      meta["episode"] = request_.episode;
    }
    if (!request_.title.empty()) {
      meta["title"] = request_.title;  // episode title
    }
  } else if (!request_.poster.empty() || !request_.subtitle.empty()) {
    meta["metadataType"] = 1;  // MovieMediaMetadata
    if (!request_.title.empty()) {
      meta["title"] = request_.title;
    }
    if (!request_.subtitle.empty()) {
      meta["subtitle"] = request_.subtitle;
    }
  } else {
    meta["metadataType"] = 0;  // GenericMediaMetadata (title only)
    if (!request_.title.empty()) {
      meta["title"] = request_.title;
    }
  }
  if (!request_.poster.empty()) {
    Json::Value image(Json::objectValue);
    image["url"] = request_.poster;
    Json::Value images(Json::arrayValue);
    images.append(image);
    meta["images"] = images;
  }
  media["metadata"] = meta;

  // Side-loaded caption track: a single TEXT/SUBTITLES track the Default Media
  // Receiver fetches (WebVTT) and renders. Declared on the media, activated via
  // the LOAD's activeTrackIds so captions show without a manual menu toggle.
  Json::Value m(Json::objectValue);
  if (!request_.subtitle_url.empty()) {
    Json::Value track(Json::objectValue);
    track["trackId"] = 1;
    track["type"] = "TEXT";
    track["trackContentId"] = request_.subtitle_url;
    track["trackContentType"] = "text/vtt";
    track["subtype"] = "SUBTITLES";
    if (!request_.subtitle_lang.empty()) {
      track["language"] = request_.subtitle_lang;
    }
    track["name"] =
        request_.subtitle_name.empty() ? "Subtitles" : request_.subtitle_name;
    Json::Value tracks(Json::arrayValue);
    tracks.append(track);
    media["tracks"] = tracks;
    Json::Value active(Json::arrayValue);
    active.append(1);
    m["activeTrackIds"] = active;
  }

  m["type"] = "LOAD";
  m["requestId"] = NextRequestId();
  m["media"] = media;
  m["autoplay"] = true;
  m["currentTime"] = request_.current_time;
  SendToApp(std::string(kMediaNamespace), Stringify(m));
}

void MediaReceiverClient::HandleMediaStatus(const Json::Value& payload) {
  const Json::Value& arr = payload[kMessageKeyStatus];
  if (!arr.isArray() || arr.empty()) {
    return;
  }
  const Json::Value& s = arr[0];
  MediaStatus st;
  st.media_session_id = s.get("mediaSessionId", 0).asInt();
  if (superseded_session_id_ != 0 &&
      st.media_session_id == superseded_session_id_) {
    return;  // the media a Reload() replaced: its INTERRUPTED end is expected
  }
  media_session_id_ = st.media_session_id;
  st.state = s.get("playerState", "").asString();
  // The receiver reports IDLE with an idleReason once playback ends (FINISHED),
  // is cancelled, or errors — that means the session is over, not just
  // starting.
  const std::string idle_reason = s.get("idleReason", "").asString();
  if (!idle_reason.empty()) {
    // ERROR/CANCELLED/INTERRUPTED point at delivery/codec problems on the
    // receiver; surfacing the reason is the only way to diagnose them.
    OSP_LOG_WARN << "castbridge: media went IDLE (" << idle_reason << ")";
  }
  // Only ERROR is a genuine fault; FINISHED/CANCELLED/INTERRUPTED are normal
  // ends (a natural finish or a new LOAD replacing this one) and must not read
  // as errors.
  if (idle_reason == "ERROR") {
    st.error = idle_reason;
  }
  st.active = !(st.state == "IDLE" && !idle_reason.empty());
  st.position = s.get("currentTime", 0.0).asDouble();
  const Json::Value& media = s["media"];
  st.duration = media.get("duration", 0.0).asDouble();
  st.title = media["metadata"].get("title", "").asString();
  // Which tracks the receiver actually has active — for a side-loaded caption
  // track this confirms the WebVTT was fetched + activated (ADR 0016). Absent
  // on receivers that don't echo it → empty, treated as "unknown" downstream,
  // never a downgrade.
  const Json::Value& active_ids = s["activeTrackIds"];
  if (active_ids.isArray()) {
    for (const Json::Value& id : active_ids) {
      if (id.isInt()) {
        st.active_track_ids.push_back(id.asInt());
      }
    }
  }

  if (on_status_) {
    on_status_(st);
  }
  if (!loaded_) {
    loaded_ = true;
    if (!request_.text_language.empty()) {
      SendTextLanguage();
    }
    FireReady(true, "");
  }
}

void MediaReceiverClient::SendTextLanguage() {
  Json::Value m(Json::objectValue);
  m["type"] = "EDIT_TRACKS_INFO";
  m["requestId"] = NextRequestId();
  m["mediaSessionId"] = media_session_id_;
  m["language"] = request_.text_language;
  m["enableTextTracks"] = true;
  SendToApp(std::string(kMediaNamespace), Stringify(m));
}

void MediaReceiverClient::HandleMediaError(const std::string& type,
                                           const Json::Value& body) {
  const std::string reason = body.get("reason", "").asString();
  const std::string detail = reason.empty() ? type : (type + ": " + reason);
  OSP_LOG_WARN << "castbridge: media error (" << detail << ")";
  // Push it as a status so the client's event stream carries the failure; an
  // inactive status with a non-empty error is serialized by the daemon (it
  // would otherwise be dropped as an idle session).
  MediaStatus st;
  st.error = detail;
  st.media_session_id = media_session_id_;
  if (on_status_) {
    on_status_(st);
  }
  // A LOAD that fails before the first MEDIA_STATUS must resolve the one-shot
  // ready as a failure, so the caller falls back (to catt) instead of hanging
  // until the timeout.
  if (!loaded_) {
    loaded_ = true;
    FireReady(false, detail);
  }
}

void MediaReceiverClient::Control(const std::string& cmd, double value) {
  std::string type;
  if (cmd == "play") {
    type = "PLAY";
  } else if (cmd == "pause") {
    type = "PAUSE";
  } else if (cmd == "stop") {
    type = "STOP";
  } else if (cmd == "seek") {
    type = "SEEK";
  } else {
    return;
  }
  Json::Value m(Json::objectValue);
  m["type"] = type;
  m["requestId"] = NextRequestId();
  m["mediaSessionId"] = media_session_id_;
  if (type == "SEEK") {
    m["currentTime"] = value;
  }
  SendToApp(std::string(kMediaNamespace), Stringify(m));
}

void MediaReceiverClient::FireReady(bool ok, const std::string& error) {
  if (on_ready_) {
    ReadyCallback cb = std::move(on_ready_);
    on_ready_ = nullptr;
    cb(ok, error);
  }
}

}  // namespace castbridge
