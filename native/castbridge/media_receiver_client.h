// Native Cast media client. Connects to a receiver (TLS:8009), launches the
// default media receiver app (CC1AD845), then LOADs a media URL and drives
// playback (play/pause/seek/stop/volume) on the media namespace — the native
// equivalent of what catt did, with no skill-cast dependency.
//
// Modelled on cast/standalone_sender/looping_file_cast_agent.{h,cc}, minus the
// streaming pieces (SenderSession/Environment/CastSocketMessagePort): a media
// client only needs the channel layer. Runs on the openscreen TaskRunner thread.
#ifndef CAST_CASTBRIDGE_MEDIA_RECEIVER_CLIENT_H_
#define CAST_CASTBRIDGE_MEDIA_RECEIVER_CLIENT_H_

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

struct MediaStatus {
  bool active = false;
  std::string state;  // PLAYING | PAUSED | BUFFERING | IDLE
  std::string title;
  double position = 0;
  double duration = 0;
  int media_session_id = 0;
};

struct LoadRequest {
  std::string url;
  std::string content_type;  // e.g. "video/mp4"; empty -> receiver guesses
  std::string title;
  double current_time = 0;
};

class MediaReceiverClient final
    : public openscreen::cast::SenderSocketFactory::Client,
      public openscreen::cast::VirtualConnectionRouter::SocketErrorHandler,
      public openscreen::cast::ConnectionNamespaceHandler::VirtualConnectionPolicy,
      public openscreen::cast::CastMessageHandler {
 public:
  using StatusCallback = std::function<void(const MediaStatus&)>;
  using ReadyCallback = std::function<void(bool ok, const std::string& error)>;
  using ClosedCallback = std::function<void(const std::string& error)>;

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
  void SetVolume(double level);                         // 0.0 .. 1.0
  void SetMuted(bool muted);

  void Shutdown();

 private:
  // SenderSocketFactory::Client.
  void OnConnected(openscreen::cast::SenderSocketFactory* factory,
                   const openscreen::IPEndpoint& endpoint,
                   std::unique_ptr<openscreen::cast::CastSocket> socket) override;
  void OnError(openscreen::cast::SenderSocketFactory* factory,
               const openscreen::IPEndpoint& endpoint,
               const openscreen::Error& error) override;

  // VirtualConnectionRouter::SocketErrorHandler.
  void OnClose(openscreen::cast::CastSocket* socket) override;
  void OnError(openscreen::cast::CastSocket* socket,
               const openscreen::Error& error) override;

  // ConnectionNamespaceHandler::VirtualConnectionPolicy.
  bool IsConnectionAllowed(
      const openscreen::cast::VirtualConnection& virtual_conn) const override;

  // CastMessageHandler.
  void OnMessage(openscreen::cast::VirtualConnectionRouter* router,
                 openscreen::cast::CastSocket* socket,
                 openscreen::cast::proto::CastMessage message) override;

  void SendLaunch();
  void OnPlatformOpened(bool success);
  void HandleReceiverStatus(const Json::Value& payload);
  void OnMediaOpened(bool success);
  void SendLoad();
  void HandleMediaStatus(const Json::Value& payload);
  void SendToMedia(const std::string& json);
  void FireReady(bool ok, const std::string& error);
  void ScheduleHeartbeat();
  void SendHeartbeat(const char* type);

  openscreen::TaskRunner& task_runner_;
  StatusCallback on_status_;
  ClosedCallback on_closed_;
  ReadyCallback on_ready_;  // one-shot

  openscreen::cast::VirtualConnectionRouter router_;
  openscreen::cast::ConnectionNamespaceHandler connection_handler_;
  openscreen::cast::SenderSocketFactory socket_factory_;
  std::unique_ptr<openscreen::TlsConnectionFactory> connection_factory_;

  int socket_id_ = 0;
  int next_request_id_ = 1;
  std::optional<openscreen::cast::VirtualConnection> platform_vc_;
  std::optional<openscreen::cast::VirtualConnection> media_vc_;
  std::string media_local_id_;
  std::string app_session_id_;
  int media_session_id_ = 0;

  LoadRequest request_;
  bool loaded_ = false;
  openscreen::Alarm heartbeat_alarm_;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_MEDIA_RECEIVER_CLIENT_H_
