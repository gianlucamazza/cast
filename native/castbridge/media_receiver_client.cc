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

}  // namespace

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
  if (ns == kMediaNamespace && type == "MEDIA_STATUS") {
    HandleMediaStatus(body);
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

  Json::Value m(Json::objectValue);
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
  media_session_id_ = st.media_session_id;
  st.state = s.get("playerState", "").asString();
  // The receiver reports IDLE with an idleReason once playback ends (FINISHED),
  // is cancelled, or errors — that means the session is over, not just
  // starting.
  const std::string idle_reason = s.get("idleReason", "").asString();
  st.active = !(st.state == "IDLE" && !idle_reason.empty());
  st.position = s.get("currentTime", 0.0).asDouble();
  const Json::Value& media = s["media"];
  st.duration = media.get("duration", 0.0).asDouble();
  st.title = media["metadata"].get("title", "").asString();

  if (on_status_) {
    on_status_(st);
  }
  if (!loaded_) {
    loaded_ = true;
    FireReady(true, "");
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
