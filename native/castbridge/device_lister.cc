#include "cast/castbridge/device_lister.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "discovery/common/config.h"
#include "platform/base/ip_address.h"
#include "util/osp_logging.h"

namespace castbridge {

namespace {
namespace discovery = openscreen::discovery;

std::string AddressToString(const openscreen::IPAddress& addr) {
  std::ostringstream oss;
  oss << addr;
  return oss.str();
}
}  // namespace

DeviceLister::DeviceLister(openscreen::TaskRunner& task_runner)
    : task_runner_(task_runner) {}

DeviceLister::~DeviceLister() = default;

void DeviceLister::Start(const openscreen::InterfaceInfo& interface) {
  discovery::Config config{.network_info = {interface},
                           .enable_publication = false,
                           .enable_querying = true};
  service_ =
      discovery::CreateDnsSdService(task_runner_, *this, std::move(config));

  watcher_ = std::make_unique<
      discovery::DnsSdServiceWatcher<openscreen::cast::ReceiverInfo>>(
      service_.get(), openscreen::cast::kCastV2ServiceId,
      openscreen::cast::DnsSdInstanceEndpointToReceiverInfo,
      [this](std::vector<
             std::reference_wrapper<const openscreen::cast::ReceiverInfo>>
                 all) { OnUpdate(std::move(all)); });

  OSP_LOG_INFO << "castbridge: starting Cast discovery on " << interface.name;
  watcher_->StartDiscovery();
}

void DeviceLister::Shutdown() {
  watcher_.reset();
  service_.reset();
}

std::vector<Device> DeviceLister::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_;
}

void DeviceLister::OnFatalError(const openscreen::Error& error) {
  OSP_LOG_ERROR << "castbridge: discovery fatal error: " << error;
}

void DeviceLister::OnRecoverableError(const openscreen::Error& error) {
  OSP_VLOG << "castbridge: discovery recoverable error: " << error;
}

void DeviceLister::OnUpdate(
    std::vector<std::reference_wrapper<const openscreen::cast::ReceiverInfo>>
        all) {
  std::vector<Device> next;
  next.reserve(all.size());
  for (const openscreen::cast::ReceiverInfo& info : all) {
    if (!info.IsValid() || (!info.v4_address && !info.v6_address)) {
      continue;
    }
    Device d;
    d.id = info.GetInstanceId();
    d.name = info.friendly_name;
    d.model = info.model_name;
    d.ip = info.v4_address ? AddressToString(info.v4_address)
                           : AddressToString(info.v6_address);
    d.port = info.port;
    // 'st' == kBusy: the receiver is already hosting an activity (a session,
    // possibly from another sender). Surfaced so the UI can flag it.
    d.busy = info.status == openscreen::cast::ReceiverStatus::kBusy;
    next.push_back(std::move(d));
  }
  std::sort(next.begin(), next.end(),
            [](const Device& a, const Device& b) { return a.name < b.name; });

  bool changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    changed = next.size() != devices_.size() ||
              !std::equal(next.begin(), next.end(), devices_.begin(),
                          [](const Device& a, const Device& b) {
                            return a.id == b.id && a.ip == b.ip &&
                                   a.port == b.port && a.name == b.name;
                          });
    devices_ = std::move(next);
  }
  if (changed && on_change_) {
    on_change_();
  }
}

}  // namespace castbridge
