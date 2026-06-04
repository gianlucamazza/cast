#include "cast/castbridge/youtube_cast_client.h"

#include <utility>

#include "cast/common/channel/message_util.h"
#include "json/value.h"
#include "util/json/json_serialization.h"
#include "util/osp_logging.h"

namespace castbridge {

namespace {

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
    : CastChannelClient(task_runner,
                        std::move(trust_store),
                        std::move(on_closed)) {}

YouTubeCastClient::~YouTubeCastClient() {
  Shutdown();
}

void YouTubeCastClient::Connect(const openscreen::IPEndpoint& endpoint,
                                ScreenIdCallback on_screen_id) {
  on_screen_id_ = std::move(on_screen_id);
  ConnectInternal(endpoint);
}

void YouTubeCastClient::OnAppConnectionOpened(bool success) {
  if (!success) {
    FireScreenId(false, "", "failed to open YouTube app connection");
    return;
  }
  Json::Value m(Json::objectValue);
  m["type"] = "getMdxSessionStatus";
  SendToApp(std::string(kYouTubeMdxNamespace), Stringify(m));
}

void YouTubeCastClient::OnAppMessage(const std::string& ns,
                                     const std::string& type,
                                     const Json::Value& body) {
  if (ns == kYouTubeMdxNamespace && type == "mdxSessionStatus") {
    const Json::Value& data = body["data"];
    const std::string screen_id = data.get("screenId", "").asString();
    if (!screen_id.empty()) {
      FireScreenId(true, screen_id, "");
    }
  }
}

void YouTubeCastClient::OnConnectError(const std::string& error) {
  FireScreenId(false, "", error);
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

}  // namespace castbridge
