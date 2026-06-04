// Cast-channel half of native YouTube casting. Launches the TV's YouTube app
// (233637DE) and performs the MDX handshake (getMdxSessionStatus) to obtain the
// receiver's screenId, which the Lounge HTTP layer then uses to drive playback.
//
// The Cast-channel plumbing is shared via CastChannelClient; this class adds
// the MDX namespace. Runs on the openscreen TaskRunner thread.
#ifndef CAST_CASTBRIDGE_YOUTUBE_CAST_CLIENT_H_
#define CAST_CASTBRIDGE_YOUTUBE_CAST_CLIENT_H_

#include <functional>
#include <memory>
#include <string>

#include "cast/castbridge/cast_channel_client.h"

namespace Json {
class Value;
}

namespace castbridge {

class YouTubeCastClient final : public CastChannelClient {
 public:
  using ScreenIdCallback = std::function<
      void(bool ok, const std::string& screen_id, const std::string& error)>;

  YouTubeCastClient(openscreen::TaskRunner& task_runner,
                    std::unique_ptr<openscreen::cast::TrustStore> trust_store,
                    ClosedCallback on_closed);
  ~YouTubeCastClient() override;

  // Connect, launch the YouTube app, and resolve the screenId (fires once).
  void Connect(const openscreen::IPEndpoint& endpoint,
               ScreenIdCallback on_screen_id);

 protected:
  const char* app_id() const override { return "233637DE"; }
  const char* app_name() const override { return "YouTube app"; }
  const char* local_id_prefix() const override { return "yt-sender"; }
  void OnAppConnectionOpened(bool success) override;
  void OnAppMessage(const std::string& ns,
                    const std::string& type,
                    const Json::Value& body) override;
  void OnConnectError(const std::string& error) override;

 private:
  void FireScreenId(bool ok,
                    const std::string& screen_id,
                    const std::string& error);

  ScreenIdCallback on_screen_id_;  // one-shot
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_YOUTUBE_CAST_CLIENT_H_
