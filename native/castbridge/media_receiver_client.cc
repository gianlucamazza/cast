#include "cast/castbridge/media_receiver_client.h"

#include <chrono>
#include <utility>

#include "cast/common/channel/message_util.h"
#include "json/value.h"
#include "platform/api/time.h"
#include "platform/api/tls_connection_factory.h"
#include "util/json/json_serialization.h"
#include "util/osp_logging.h"

namespace castbridge {

namespace {

using openscreen::cast::CastSocket;
using openscreen::cast::kBroadcastId;
using openscreen::cast::kHeartbeatNamespace;
using openscreen::cast::kMediaNamespace;
using openscreen::cast::kMessageKeyAppId;
using openscreen::cast::kMessageKeyApplications;
using openscreen::cast::kMessageKeySessionId;
using openscreen::cast::kMessageKeyStatus;
using openscreen::cast::kMessageKeyTransportId;
using openscreen::cast::kPlatformReceiverId;
using openscreen::cast::kPlatformSenderId;
using openscreen::cast::kReceiverNamespace;
using openscreen::cast::MakeSimpleUTF8Message;
using openscreen::cast::MakeUniqueSessionId;
using openscreen::cast::VirtualConnection;

// Default media receiver app id (not defined in the fork's app-id header).
constexpr char kDefaultMediaReceiverAppId[] = "CC1AD845";

std::string Stringify(const Json::Value& v) {
  auto r = openscreen::json::Stringify(v);
  return r.is_value() ? r.value() : std::string();
}

std::string MessageType(const Json::Value& payload) {
  return payload.isObject() ? payload.get("type", "").asString() : std::string();
}

}  // namespace

MediaReceiverClient::MediaReceiverClient(
    openscreen::TaskRunner& task_runner,
    std::unique_ptr<openscreen::cast::TrustStore> trust_store,
    StatusCallback on_status,
    ClosedCallback on_closed)
    : task_runner_(task_runner),
      on_status_(std::move(on_status)),
      on_closed_(std::move(on_closed)),
      connection_handler_(router_, *this),
      socket_factory_(*this,
                      task_runner_,
                      std::move(trust_store),
                      openscreen::cast::CastCRLTrustStore::Create()),
      connection_factory_(openscreen::TlsConnectionFactory::CreateFactory(
          socket_factory_,
          task_runner_)),
      heartbeat_alarm_(&openscreen::Clock::now, task_runner_) {
  router_.AddHandlerForLocalId(std::string(kPlatformSenderId), this);
  socket_factory_.set_factory(connection_factory_.get());
}

MediaReceiverClient::~MediaReceiverClient() {
  Shutdown();
}

void MediaReceiverClient::Connect(const openscreen::IPEndpoint& endpoint,
                                  LoadRequest request,
                                  ReadyCallback on_ready) {
  request_ = std::move(request);
  on_ready_ = std::move(on_ready);
  task_runner_.PostTask([this, endpoint] {
    socket_factory_.Connect(
        endpoint,
        openscreen::cast::SenderSocketFactory::DeviceMediaPolicy::kIncludesVideo,
        &router_);
  });
}

void MediaReceiverClient::OnConnected(
    openscreen::cast::SenderSocketFactory* factory,
    const openscreen::IPEndpoint& endpoint,
    std::unique_ptr<CastSocket> socket) {
  socket_id_ = socket->socket_id();
  router_.TakeSocket(this, std::move(socket));

  OSP_LOG_INFO << "castbridge: launching default media receiver...";
  platform_vc_.emplace(VirtualConnection{std::string(kPlatformSenderId),
                                         std::string(kPlatformReceiverId),
                                         socket_id_});
  connection_handler_.OpenRemoteConnection(
      *platform_vc_, [this](bool success) { OnPlatformOpened(success); });
}

void MediaReceiverClient::OnError(openscreen::cast::SenderSocketFactory* factory,
                                  const openscreen::IPEndpoint& endpoint,
                                  const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: media socket error: " << error;
  FireReady(false, error.ToString());
  if (on_closed_) {
    on_closed_(error.ToString());
  }
}

void MediaReceiverClient::OnClose(CastSocket* socket) {
  if (on_closed_) {
    on_closed_("connection closed");
  }
}

void MediaReceiverClient::OnError(CastSocket* socket,
                                  const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: media cast socket error: " << error;
  FireReady(false, error.ToString());
  if (on_closed_) {
    on_closed_(error.ToString());
  }
}

bool MediaReceiverClient::IsConnectionAllowed(
    const VirtualConnection& virtual_conn) const {
  return true;
}

void MediaReceiverClient::OnPlatformOpened(bool success) {
  if (!success) {
    FireReady(false, "failed to open receiver connection");
    return;
  }
  SendLaunch();
  ScheduleHeartbeat();  // keep the control channel alive
}

void MediaReceiverClient::SendHeartbeat(const char* type) {
  if (!platform_vc_) {
    return;
  }
  std::string json = std::string(R"({"type":")") + type + R"("})";
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kHeartbeatNamespace), json));
}

void MediaReceiverClient::ScheduleHeartbeat() {
  if (!platform_vc_) {
    return;
  }
  SendHeartbeat("PING");
  heartbeat_alarm_.ScheduleFromNow([this] { ScheduleHeartbeat(); },
                                   std::chrono::seconds(5));
}

void MediaReceiverClient::SendLaunch() {
  Json::Value m(Json::objectValue);
  m["type"] = "LAUNCH";
  m["requestId"] = next_request_id_++;
  m["appId"] = kDefaultMediaReceiverAppId;
  m["language"] = "en-US";
  Json::Value types(Json::arrayValue);
  types.append("WEB");
  m["supportedAppTypes"] = types;
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kReceiverNamespace),
                                     Stringify(m)));
}

void MediaReceiverClient::OnMessage(
    openscreen::cast::VirtualConnectionRouter* router,
    CastSocket* socket,
    openscreen::cast::proto::CastMessage message) {
  const std::string& dst = message.destination_id();
  if (dst != kPlatformSenderId && dst != media_local_id_ && dst != kBroadcastId) {
    return;
  }
  if (message.payload_type() !=
      openscreen::cast::proto::CastMessage::STRING) {
    return;
  }
  auto payload = openscreen::json::Parse(openscreen::cast::GetPayload(message));
  if (payload.is_error() || !payload.value().isObject()) {
    return;
  }
  const Json::Value& body = payload.value();
  const std::string type = MessageType(body);
  const std::string& ns = message.namespace_();

  if (ns == kHeartbeatNamespace) {
    if (type == "PING") {
      SendHeartbeat("PONG");
    }
  } else if (ns == kReceiverNamespace) {
    if (type == "RECEIVER_STATUS") {
      HandleReceiverStatus(body);
    } else if (type == "LAUNCH_ERROR") {
      FireReady(false, "receiver refused to launch the media app");
    }
  } else if (ns == kMediaNamespace) {
    if (type == "MEDIA_STATUS") {
      HandleMediaStatus(body);
    }
  }
}

void MediaReceiverClient::HandleReceiverStatus(const Json::Value& payload) {
  const Json::Value& status = payload[kMessageKeyStatus];
  const Json::Value& apps = status[kMessageKeyApplications];
  if (!apps.isArray()) {
    return;
  }
  std::string transport_id, session_id;
  for (const Json::Value& app : apps) {
    if (app.get(kMessageKeyAppId, "").asString() == kDefaultMediaReceiverAppId) {
      transport_id = app.get(kMessageKeyTransportId, "").asString();
      session_id = app.get(kMessageKeySessionId, "").asString();
      break;
    }
  }
  if (transport_id.empty()) {
    return;  // app not (yet) running
  }
  app_session_id_ = session_id;
  if (media_vc_) {
    return;  // already connected to the media app
  }

  media_local_id_ = MakeUniqueSessionId("media-sender");
  router_.AddHandlerForLocalId(media_local_id_, this);
  media_vc_.emplace(VirtualConnection{media_local_id_, transport_id, socket_id_});
  OSP_LOG_INFO << "castbridge: media app ready, opening message routing...";
  connection_handler_.OpenRemoteConnection(
      *media_vc_, [this](bool success) { OnMediaOpened(success); });
}

void MediaReceiverClient::OnMediaOpened(bool success) {
  if (!success) {
    FireReady(false, "failed to open media app connection");
    return;
  }
  SendLoad();
}

void MediaReceiverClient::SendLoad() {
  Json::Value media(Json::objectValue);
  media["contentId"] = request_.url;
  media["streamType"] = "BUFFERED";
  media["contentType"] =
      request_.content_type.empty() ? "video/mp4" : request_.content_type;
  Json::Value meta(Json::objectValue);
  meta["metadataType"] = 0;
  if (!request_.title.empty()) {
    meta["title"] = request_.title;
  }
  media["metadata"] = meta;

  Json::Value m(Json::objectValue);
  m["type"] = "LOAD";
  m["requestId"] = next_request_id_++;
  m["media"] = media;
  m["autoplay"] = true;
  m["currentTime"] = request_.current_time;
  SendToMedia(Stringify(m));
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
  // is cancelled, or errors — that means the session is over, not just starting.
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

void MediaReceiverClient::SendToMedia(const std::string& json) {
  if (!media_vc_) {
    return;
  }
  router_.Send(*media_vc_,
               MakeSimpleUTF8Message(std::string(kMediaNamespace), json));
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
  m["requestId"] = next_request_id_++;
  m["mediaSessionId"] = media_session_id_;
  if (type == "SEEK") {
    m["currentTime"] = value;
  }
  SendToMedia(Stringify(m));
}

void MediaReceiverClient::SetVolume(double level) {
  if (!platform_vc_) {
    return;
  }
  Json::Value vol(Json::objectValue);
  vol["level"] = level;
  Json::Value m(Json::objectValue);
  m["type"] = "SET_VOLUME";
  m["requestId"] = next_request_id_++;
  m["volume"] = vol;
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kReceiverNamespace),
                                     Stringify(m)));
}

void MediaReceiverClient::SetMuted(bool muted) {
  if (!platform_vc_) {
    return;
  }
  Json::Value vol(Json::objectValue);
  vol["muted"] = muted;
  Json::Value m(Json::objectValue);
  m["type"] = "SET_VOLUME";
  m["requestId"] = next_request_id_++;
  m["volume"] = vol;
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kReceiverNamespace),
                                     Stringify(m)));
}

void MediaReceiverClient::FireReady(bool ok, const std::string& error) {
  if (on_ready_) {
    ReadyCallback cb = std::move(on_ready_);
    on_ready_ = nullptr;
    cb(ok, error);
  }
}

void MediaReceiverClient::Shutdown() {
  if (media_vc_) {
    // Best-effort: stop playback before tearing down.
    Json::Value m(Json::objectValue);
    m["type"] = "STOP";
    m["requestId"] = next_request_id_++;
    m["mediaSessionId"] = media_session_id_;
    SendToMedia(Stringify(m));
    const VirtualConnection vc = *media_vc_;
    media_vc_.reset();
    connection_handler_.CloseRemoteConnection(vc);
    router_.RemoveHandlerForLocalId(media_local_id_);
    media_local_id_.clear();
  }
  if (platform_vc_) {
    const VirtualConnection vc = *platform_vc_;
    platform_vc_.reset();
    connection_handler_.CloseRemoteConnection(vc);
  }
  if (socket_id_ != 0) {
    router_.CloseSocket(socket_id_);
    socket_id_ = 0;
  }
}

}  // namespace castbridge
