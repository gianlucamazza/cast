// Headless Cast device discovery: wraps openscreen's DnsSdServiceWatcher and
// keeps a thread-safe snapshot of the receivers seen on the LAN. This replaces
// the interactive ReceiverChooser menu with a queryable device list for the IPC
// layer. All openscreen objects are created/touched on the TaskRunner thread;
// Snapshot() is safe to call from any thread.
#ifndef CAST_CASTBRIDGE_DEVICE_LISTER_H_
#define CAST_CASTBRIDGE_DEVICE_LISTER_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cast/common/public/receiver_info.h"
#include "discovery/common/reporting_client.h"
#include "discovery/public/dns_sd_service_factory.h"
#include "discovery/public/dns_sd_service_watcher.h"
#include "platform/api/task_runner.h"
#include "platform/base/interface_info.h"

namespace castbridge {

struct Device {
  std::string id;     // stable instance id (from the TXT 'id' record)
  std::string name;   // friendly name
  std::string model;  // model name
  std::string ip;     // IPv4 (preferred) or IPv6 literal
  uint16_t port = 0;
  bool busy = false;  // receiver is hosting an activity (mDNS 'st' == 1)
};

class DeviceLister final : public openscreen::discovery::ReportingClient {
 public:
  using ChangeCallback = std::function<void()>;

  explicit DeviceLister(openscreen::TaskRunner& task_runner);
  ~DeviceLister() override;

  // Must run on the TaskRunner thread.
  void Start(const openscreen::InterfaceInfo& interface);
  // Tear down the watcher/service; must run on the TaskRunner thread.
  void Shutdown();

  // Thread-safe snapshot of the currently-known devices.
  std::vector<Device> Snapshot() const;

  // Invoked (on the TaskRunner thread) whenever the device set changes.
  void set_on_change(ChangeCallback cb) { on_change_ = std::move(cb); }

 private:
  // discovery::ReportingClient.
  void OnFatalError(const openscreen::Error& error) override;
  void OnRecoverableError(const openscreen::Error& error) override;

  void OnUpdate(
      std::vector<std::reference_wrapper<const openscreen::cast::ReceiverInfo>>
          all);

  openscreen::TaskRunner& task_runner_;
  openscreen::discovery::DnsSdServicePtr service_;
  std::unique_ptr<
      openscreen::discovery::DnsSdServiceWatcher<openscreen::cast::ReceiverInfo>>
      watcher_;

  mutable std::mutex mutex_;
  std::vector<Device> devices_;
  ChangeCallback on_change_;
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_DEVICE_LISTER_H_
