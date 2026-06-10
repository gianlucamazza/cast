#include "cast/castbridge/cast_channel_client.h"

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

std::string Stringify(const Json::Value& v) {
  auto r = openscreen::json::Stringify(v);
  return r.is_value() ? r.value() : std::string();
}

}  // namespace

CastChannelClient::CastChannelClient(
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
      connection_factory_(
          openscreen::TlsConnectionFactory::CreateFactory(socket_factory_,
                                                          task_runner_)),
      heartbeat_alarm_(&openscreen::Clock::now, task_runner_) {
  router_.AddHandlerForLocalId(std::string(kPlatformSenderId), this);
  socket_factory_.set_factory(connection_factory_.get());
}

CastChannelClient::~CastChannelClient() = default;

void CastChannelClient::ConnectInternal(
    const openscreen::IPEndpoint& endpoint) {
  task_runner_.PostTask([this, endpoint] {
    socket_factory_.Connect(endpoint,
                            openscreen::cast::SenderSocketFactory::
                                DeviceMediaPolicy::kIncludesVideo,
                            &router_);
  });
}

void CastChannelClient::OnConnected(
    openscreen::cast::SenderSocketFactory* factory,
    const openscreen::IPEndpoint& endpoint,
    std::unique_ptr<CastSocket> socket) {
  socket_id_ = socket->socket_id();
  router_.TakeSocket(this, std::move(socket));

  OSP_LOG_INFO << "castbridge: launching " << app_name() << "...";
  platform_vc_.emplace(VirtualConnection{std::string(kPlatformSenderId),
                                         std::string(kPlatformReceiverId),
                                         socket_id_});
  connection_handler_.OpenRemoteConnection(
      *platform_vc_, [this](bool success) { OnPlatformOpened(success); });
}

void CastChannelClient::OnError(openscreen::cast::SenderSocketFactory* factory,
                                const openscreen::IPEndpoint& endpoint,
                                const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: " << app_name() << " socket error: " << error;
  OnConnectError(error.ToString());
  if (on_closed_) {
    on_closed_(error.ToString());
  }
}

void CastChannelClient::OnClose(CastSocket* socket) {
  if (on_closed_) {
    on_closed_("connection closed");
  }
}

void CastChannelClient::OnError(CastSocket* socket,
                                const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: " << app_name()
                << " cast socket error: " << error;
  OnConnectError(error.ToString());
  if (on_closed_) {
    on_closed_(error.ToString());
  }
}

bool CastChannelClient::IsConnectionAllowed(
    const VirtualConnection& virtual_conn) const {
  return true;
}

void CastChannelClient::OnPlatformOpened(bool success) {
  if (!success) {
    OnConnectError("failed to open receiver connection");
    return;
  }
  SendLaunch();
  ScheduleHeartbeat();  // keep the control channel alive
}

void CastChannelClient::SendHeartbeat(const char* type) {
  if (!platform_vc_) {
    return;
  }
  std::string json = std::string(R"({"type":")") + type + R"("})";
  router_.Send(*platform_vc_,
               MakeSimpleUTF8Message(std::string(kHeartbeatNamespace), json));
}

void CastChannelClient::ScheduleHeartbeat() {
  if (!platform_vc_) {
    return;
  }
  SendHeartbeat("PING");
  heartbeat_alarm_.ScheduleFromNow([this] { ScheduleHeartbeat(); },
                                   std::chrono::seconds(5));
}

void CastChannelClient::SendLaunch() {
  Json::Value m(Json::objectValue);
  m["type"] = "LAUNCH";
  m["requestId"] = next_request_id_++;
  m["appId"] = app_id();
  m["language"] = "en-US";
  Json::Value types(Json::arrayValue);
  types.append("WEB");
  m["supportedAppTypes"] = types;
  router_.Send(
      *platform_vc_,
      MakeSimpleUTF8Message(std::string(kReceiverNamespace), Stringify(m)));
}

void CastChannelClient::OnMessage(
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
      OnConnectError(std::string("receiver refused to launch the ") +
                     app_name());
    }
  } else {
    OnAppMessage(ns, type, body);
  }
}

void CastChannelClient::HandleReceiverStatus(const Json::Value& payload) {
  const Json::Value& status = payload[kMessageKeyStatus];
  const Json::Value& apps = status[kMessageKeyApplications];
  if (!apps.isArray()) {
    return;
  }
  std::string transport_id, session_id;
  for (const Json::Value& app : apps) {
    if (app.get(kMessageKeyAppId, "").asString() == app_id()) {
      transport_id = app.get(kMessageKeyTransportId, "").asString();
      session_id = app.get(kMessageKeySessionId, "").asString();
      break;
    }
  }
  if (transport_id.empty() || app_vc_) {
    return;  // app not (yet) running, or already connected
  }
  app_session_id_ = session_id;
  app_local_id_ = MakeUniqueSessionId(local_id_prefix());
  router_.AddHandlerForLocalId(app_local_id_, this);
  app_vc_.emplace(VirtualConnection{app_local_id_, transport_id, socket_id_});
  OSP_LOG_INFO << "castbridge: " << app_name()
               << " ready, opening message routing...";
  // The CONNECT to the app transport is fire-and-forget: Google receivers ack
  // it, but third-party Cast stacks (e.g. Philips TVs) never do, so gating the
  // session on the ack hangs forever. Send it and proceed immediately — the
  // same strategy every production sender uses.
  connection_handler_.OpenRemoteConnection(*app_vc_, [](bool) {});
  OnAppConnectionOpened(true);
}

void CastChannelClient::SendToPlatform(const std::string& ns,
                                       const std::string& json) {
  if (!platform_vc_) {
    return;
  }
  router_.Send(*platform_vc_, MakeSimpleUTF8Message(ns, json));
}

void CastChannelClient::SendToApp(const std::string& ns,
                                  const std::string& json) {
  if (!app_vc_) {
    return;
  }
  router_.Send(*app_vc_, MakeSimpleUTF8Message(ns, json));
}

void CastChannelClient::SetVolume(double level) {
  Json::Value vol(Json::objectValue);
  vol["level"] = level;
  Json::Value m(Json::objectValue);
  m["type"] = "SET_VOLUME";
  m["requestId"] = next_request_id_++;
  m["volume"] = vol;
  SendToPlatform(std::string(kReceiverNamespace), Stringify(m));
}

void CastChannelClient::SetMuted(bool muted) {
  Json::Value vol(Json::objectValue);
  vol["muted"] = muted;
  Json::Value m(Json::objectValue);
  m["type"] = "SET_VOLUME";
  m["requestId"] = next_request_id_++;
  m["volume"] = vol;
  SendToPlatform(std::string(kReceiverNamespace), Stringify(m));
}

void CastChannelClient::Shutdown() {
  // Explicit teardown must never announce itself as a remote close: the
  // on_closed_ chain belongs to the session being torn down, and firing it
  // here can destroy a successor client the controller just created.
  on_closed_ = nullptr;
  OnBeforeShutdown();
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
