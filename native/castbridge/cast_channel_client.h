// Shared Cast-channel plumbing for castbridge's sender-side clients.
// Encapsulates the SenderSocketFactory + VirtualConnectionRouter +
// ConnectionNamespaceHandler
// + TlsConnectionFactory setup, the platform-connection / app-launch /
// heartbeat handshake, RECEIVER_STATUS handling, volume control, and teardown
// that media_receiver_client and youtube_cast_client share. Subclasses provide
// the target app id and handle app-specific messages.
//
// Runs on the openscreen TaskRunner thread.
#ifndef CAST_CASTBRIDGE_CAST_CHANNEL_CLIENT_H_
#define CAST_CASTBRIDGE_CAST_CHANNEL_CLIENT_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "cast/common/channel/cast_message_handler.h"
#include "cast/common/channel/connection_namespace_handler.h"
#include "cast/common/channel/virtual_connection.h"
#include "cast/common/channel/virtual_connection_router.h"
#include "cast/common/public/cast_socket.h"
#include "cast/common/public/trust_store.h"
#include "cast/sender/public/sender_socket_factory.h"
#include "platform/api/task_runner.h"
#include "platform/api/time.h"
#include "platform/api/tls_connection_factory.h"
#include "platform/base/ip_address.h"
#include "util/alarm.h"

namespace Json {
class Value;
}

namespace castbridge {

class CastChannelClient
    : public openscreen::cast::SenderSocketFactory::Client,
      public openscreen::cast::VirtualConnectionRouter::SocketErrorHandler,
      public openscreen::cast::ConnectionNamespaceHandler::
          VirtualConnectionPolicy,
      public openscreen::cast::CastMessageHandler {
 public:
  using ClosedCallback = std::function<void(const std::string& error)>;

  CastChannelClient(openscreen::TaskRunner& task_runner,
                    std::unique_ptr<openscreen::cast::TrustStore> trust_store,
                    ClosedCallback on_closed);
  ~CastChannelClient() override;

  // Volume is app-independent (receiver namespace).
  void SetVolume(double level);  // 0.0 .. 1.0
  void SetMuted(bool muted);

  // Tear down app/platform connections and the socket. Idempotent; subclass
  // destructors call this. OnBeforeShutdown() runs first, while the subclass
  // vtable is still live.
  void Shutdown();

 protected:
  // Connect the TLS socket and start the launch handshake. A subclass Connect()
  // stores its own one-shot callback first, then calls this.
  void ConnectInternal(const openscreen::IPEndpoint& endpoint);

  // Send a message on the platform (receiver) or app virtual connection. No-op
  // if the respective connection is not open.
  void SendToPlatform(const std::string& ns, const std::string& json);
  void SendToApp(const std::string& ns, const std::string& json);

  int NextRequestId() { return next_request_id_++; }
  bool has_app_connection() const { return app_vc_.has_value(); }
  const std::string& app_session_id() const { return app_session_id_; }

  // ---- Subclass hooks ----
  // Receiver app id to launch and match in RECEIVER_STATUS (e.g. "CC1AD845").
  virtual const char* app_id() const = 0;
  // Human-readable app name for log/error messages (e.g. "media app").
  virtual const char* app_name() const = 0;
  // Prefix for the app virtual-connection local id (e.g. "media-sender").
  virtual const char* local_id_prefix() const = 0;
  // Called once the app's virtual connection is (or fails to be) opened.
  virtual void OnAppConnectionOpened(bool success) = 0;
  // App-namespace messages the base does not handle (media / MDX / etc.).
  virtual void OnAppMessage(const std::string& ns,
                            const std::string& type,
                            const Json::Value& body) = 0;
  // A connection / launch failure occurred; subclass fires its one-shot.
  virtual void OnConnectError(const std::string& error) = 0;
  // Optional: runs at the start of Shutdown() (e.g. send STOP).
  virtual void OnBeforeShutdown() {}

 private:
  // SenderSocketFactory::Client.
  void OnConnected(openscreen::cast::SenderSocketFactory* factory,
                   const openscreen::IPEndpoint& endpoint,
                   std::unique_ptr<openscreen::cast::CastSocket> socket) final;
  void OnError(openscreen::cast::SenderSocketFactory* factory,
               const openscreen::IPEndpoint& endpoint,
               const openscreen::Error& error) final;

  // VirtualConnectionRouter::SocketErrorHandler.
  void OnClose(openscreen::cast::CastSocket* socket) final;
  void OnError(openscreen::cast::CastSocket* socket,
               const openscreen::Error& error) final;

  // ConnectionNamespaceHandler::VirtualConnectionPolicy.
  bool IsConnectionAllowed(
      const openscreen::cast::VirtualConnection& virtual_conn) const final;

  // CastMessageHandler.
  void OnMessage(openscreen::cast::VirtualConnectionRouter* router,
                 openscreen::cast::CastSocket* socket,
                 openscreen::cast::proto::CastMessage message) final;

  void OnPlatformOpened(bool success);
  void SendLaunch();
  void HandleReceiverStatus(const Json::Value& payload);
  void ScheduleHeartbeat();
  void SendHeartbeat(const char* type);  // "PING" | "PONG"

  openscreen::TaskRunner& task_runner_;
  ClosedCallback on_closed_;

  openscreen::cast::VirtualConnectionRouter router_;
  openscreen::cast::ConnectionNamespaceHandler connection_handler_;
  openscreen::cast::SenderSocketFactory socket_factory_;
  std::unique_ptr<openscreen::TlsConnectionFactory> connection_factory_;

  int socket_id_ = 0;
  int next_request_id_ = 1;
  std::optional<openscreen::cast::VirtualConnection> platform_vc_;
  std::optional<openscreen::cast::VirtualConnection> app_vc_;
  std::string app_local_id_;
  std::string app_session_id_;
  openscreen::Alarm heartbeat_alarm_;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_CAST_CHANNEL_CLIENT_H_
