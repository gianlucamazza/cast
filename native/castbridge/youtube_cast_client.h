// Cast-channel half of native YouTube casting. Launches the TV's YouTube app
// (233637DE) and performs the MDX handshake (getMdxSessionStatus) to obtain the
// receiver's screenId, which the Lounge HTTP layer then uses to drive playback.
// Models media_receiver_client.{h,cc}; the streaming pieces are not needed.
#ifndef CAST_CASTBRIDGE_YOUTUBE_CAST_CLIENT_H_
#define CAST_CASTBRIDGE_YOUTUBE_CAST_CLIENT_H_

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

class YouTubeCastClient final
    : public openscreen::cast::SenderSocketFactory::Client,
      public openscreen::cast::VirtualConnectionRouter::SocketErrorHandler,
      public openscreen::cast::ConnectionNamespaceHandler::VirtualConnectionPolicy,
      public openscreen::cast::CastMessageHandler {
 public:
  using ScreenIdCallback = std::function<void(bool ok, const std::string& screen_id, const std::string& error)>;
  using ClosedCallback = std::function<void(const std::string& error)>;

  YouTubeCastClient(openscreen::TaskRunner& task_runner,
                    std::unique_ptr<openscreen::cast::TrustStore> trust_store,
                    ClosedCallback on_closed);
  ~YouTubeCastClient() override;

  // Connect, launch the YouTube app, and resolve the screenId (fires once).
  void Connect(const openscreen::IPEndpoint& endpoint, ScreenIdCallback on_screen_id);

  // Volume is independent of the app (receiver namespace).
  void SetVolume(double level);
  void SetMuted(bool muted);

  void Shutdown();

 private:
  void OnConnected(openscreen::cast::SenderSocketFactory* factory,
                   const openscreen::IPEndpoint& endpoint,
                   std::unique_ptr<openscreen::cast::CastSocket> socket) override;
  void OnError(openscreen::cast::SenderSocketFactory* factory,
               const openscreen::IPEndpoint& endpoint,
               const openscreen::Error& error) override;
  void OnClose(openscreen::cast::CastSocket* socket) override;
  void OnError(openscreen::cast::CastSocket* socket,
               const openscreen::Error& error) override;
  bool IsConnectionAllowed(
      const openscreen::cast::VirtualConnection& virtual_conn) const override;
  void OnMessage(openscreen::cast::VirtualConnectionRouter* router,
                 openscreen::cast::CastSocket* socket,
                 openscreen::cast::proto::CastMessage message) override;

  void SendLaunch();
  void OnPlatformOpened(bool success);
  void HandleReceiverStatus(const Json::Value& payload);
  void OnAppOpened(bool success);
  void FireScreenId(bool ok, const std::string& screen_id, const std::string& error);
  void ScheduleHeartbeat();  // periodic PING to keep the control channel alive
  void SendHeartbeat(const char* type);  // "PING" | "PONG"

  openscreen::TaskRunner& task_runner_;
  ClosedCallback on_closed_;
  ScreenIdCallback on_screen_id_;  // one-shot

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

#endif  // CAST_CASTBRIDGE_YOUTUBE_CAST_CLIENT_H_
