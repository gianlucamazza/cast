#include "cast/castbridge/youtube_cast_client.h"

#include <utility>

#include <chrono>

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
using openscreen::cast::kMessageKeyAppId;
using openscreen::cast::kMessageKeyApplications;
using openscreen::cast::kMessageKeySessionId;
using openscreen::cast::kMessageKeyStatus;
using openscreen::cast::kHeartbeatNamespace;
using openscreen::cast::kMessageKeyTransportId;
using openscreen::cast::kPlatformReceiverId;
using openscreen::cast::kPlatformSenderId;
using openscreen::cast::kReceiverNamespace;
using openscreen::cast::MakeSimpleUTF8Message;
using openscreen::cast::MakeUniqueSessionId;
using openscreen::cast::VirtualConnection;

constexpr char kYouTubeAppId[] = "233637DE";
constexpr char kYouTubeMdxNamespace[] = "urn:x-cast:com.google.youtube.mdx";

std::string Stringify(const Json::Value& v) {
  auto r = openscreen::json::Stringify(v);
  return r.is_value() ? r.value() : std::string();
}

}  // namespace

YouTubeCastClient::YouTubeCastClient(
    openscreen::TaskRunner& task_runner,
    std::unique_ptr<openscreen::cast::TrustStore> trust_store,
    ClosedCallback on_closed)
    : task_runner_(task_runner),
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

YouTubeCastClient::~YouTubeCastClient() {
  Shutdown();
}

void YouTubeCastClient::Connect(const openscreen::IPEndpoint& endpoint,
                                ScreenIdCallback on_screen_id) {
  on_screen_id_ = std::move(on_screen_id);
  task_runner_.PostTask([this, endpoint] {
    socket_factory_.Connect(
        endpoint,
        openscreen::cast::SenderSocketFactory::DeviceMediaPolicy::kIncludesVideo,
        &router_);
  });
}

void YouTubeCastClient::OnConnected(
    openscreen::cast::SenderSocketFactory* factory,
    const openscreen::IPEndpoint& endpoint,
    std::unique_ptr<CastSocket> socket) {
  socket_id_ = socket->socket_id();
  router_.TakeSocket(this, std::move(socket));
  OSP_LOG_INFO << "castbridge: launching YouTube app...";
  platform_vc_.emplace(VirtualConnection{std::string(kPlatformSenderId),
                                         std::string(kPlatformReceiverId),
                                         socket_id_});
  connection_handler_.OpenRemoteConnection(
      *platform_vc_, [this](bool success) { OnPlatformOpened(success); });
}

void YouTubeCastClient::OnError(openscreen::cast::SenderSocketFactory* factory,
                                const openscreen::IPEndpoint& endpoint,
                                const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: youtube socket error: " << error;
  FireScreenId(false, "", error.ToString());
  if (on_closed_) {
    on_closed_(error.ToString());
  }
}

void YouTubeCastClient::OnClose(CastSocket* socket) {
  if (on_closed_) {
    on_closed_("connection closed");
  }
}

void YouTubeCastClient::OnError(CastSocket* socket,
                                const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: youtube cast socket error: " << error;
  FireScreenId(false, "", error.ToString());
  if (on_closed_) {
    on_closed_(error.ToString());
  }
}

bool YouTubeCastClient::IsConnectionAllowed(
    const VirtualConnection& virtual_conn) const {
  return true;
}

void YouTubeCastClient::OnPlatformOpened(bool success) {
  if (!success) {
    FireScreenId(false, "", "failed to open receiver connection");
    return;
  }
  SendLaunch();
  ScheduleHeartbeat();  // keep the control channel alive
}

void YouTubeCastClient::SendHeartbeat(const char* type) {
  if (!platform_vc_) {
    return;
  }
  std::string json = std::string(R"({"type":")") + type + R"("})";
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kHeartbeatNamespace), json));
}

void YouTubeCastClient::ScheduleHeartbeat() {
  if (!platform_vc_) {
    return;
  }
  SendHeartbeat("PING");
  heartbeat_alarm_.ScheduleFromNow([this] { ScheduleHeartbeat(); },
                                   std::chrono::seconds(5));
}

void YouTubeCastClient::SendLaunch() {
  Json::Value m(Json::objectValue);
  m["type"] = "LAUNCH";
  m["requestId"] = next_request_id_++;
  m["appId"] = kYouTubeAppId;
  m["language"] = "en-US";
  Json::Value types(Json::arrayValue);
  types.append("WEB");
  m["supportedAppTypes"] = types;
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kReceiverNamespace),
                                     Stringify(m)));
}

void YouTubeCastClient::OnMessage(
    openscreen::cast::VirtualConnectionRouter* router,
    CastSocket* socket,
    openscreen::cast::proto::CastMessage message) {
  const std::string& dst = message.destination_id();
  if (dst != kPlatformSenderId && dst != app_local_id_ && dst != kBroadcastId) {
    return;
  }
  if (message.payload_type() != openscreen::cast::proto::CastMessage::STRING) {
    return;
  }
  auto payload = openscreen::json::Parse(openscreen::cast::GetPayload(message));
  if (payload.is_error() || !payload.value().isObject()) {
    return;
  }
  const Json::Value& body = payload.value();
  const std::string type = body.get("type", "").asString();
  const std::string& ns = message.namespace_();

  if (ns == kHeartbeatNamespace) {
    if (type == "PING") {
      SendHeartbeat("PONG");
    }
  } else if (ns == kReceiverNamespace) {
    if (type == "RECEIVER_STATUS") {
      HandleReceiverStatus(body);
    } else if (type == "LAUNCH_ERROR") {
      FireScreenId(false, "", "receiver refused to launch the YouTube app");
    }
  } else if (ns == kYouTubeMdxNamespace) {
    if (type == "mdxSessionStatus") {
      const Json::Value& data = body["data"];
      const std::string screen_id = data.get("screenId", "").asString();
      if (!screen_id.empty()) {
        FireScreenId(true, screen_id, "");
      }
    }
  }
}

void YouTubeCastClient::HandleReceiverStatus(const Json::Value& payload) {
  const Json::Value& status = payload[kMessageKeyStatus];
  const Json::Value& apps = status[kMessageKeyApplications];
  if (!apps.isArray()) {
    return;
  }
  std::string transport_id, session_id;
  for (const Json::Value& app : apps) {
    if (app.get(kMessageKeyAppId, "").asString() == kYouTubeAppId) {
      transport_id = app.get(kMessageKeyTransportId, "").asString();
      session_id = app.get(kMessageKeySessionId, "").asString();
      break;
    }
  }
  if (transport_id.empty() || app_vc_) {
    return;
  }
  app_session_id_ = session_id;
  app_local_id_ = MakeUniqueSessionId("yt-sender");
  router_.AddHandlerForLocalId(app_local_id_, this);
  app_vc_.emplace(VirtualConnection{app_local_id_, transport_id, socket_id_});
  OSP_LOG_INFO << "castbridge: YouTube app launched, opening MDX routing...";
  connection_handler_.OpenRemoteConnection(
      *app_vc_, [this](bool success) { OnAppOpened(success); });
}

void YouTubeCastClient::OnAppOpened(bool success) {
  if (!success) {
    FireScreenId(false, "", "failed to open YouTube app connection");
    return;
  }
  Json::Value m(Json::objectValue);
  m["type"] = "getMdxSessionStatus";
  router_.Send(*app_vc_, MakeSimpleUTF8Message(std::string(kYouTubeMdxNamespace),
                                               Stringify(m)));
}

void YouTubeCastClient::SetVolume(double level) {
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

void YouTubeCastClient::SetMuted(bool muted) {
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

void YouTubeCastClient::FireScreenId(bool ok,
                                     const std::string& screen_id,
                                     const std::string& error) {
  if (on_screen_id_) {
    ScreenIdCallback cb = std::move(on_screen_id_);
    on_screen_id_ = nullptr;
    cb(ok, screen_id, error);
  }
}

void YouTubeCastClient::Shutdown() {
  if (app_vc_) {
    const VirtualConnection vc = *app_vc_;
    app_vc_.reset();
    connection_handler_.CloseRemoteConnection(vc);
    router_.RemoveHandlerForLocalId(app_local_id_);
    app_local_id_.clear();
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
