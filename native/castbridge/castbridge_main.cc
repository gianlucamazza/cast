// castbridge — native backend for the Cast browser extension.
//
//   castbridge --daemon     run the discovery/session daemon (IPC over AF_UNIX)
//   castbridge --nm-host     run the native-messaging relay (Firefox stdin/out)
//
// The relay is what Firefox launches; it forwards to (and, if needed, starts)
// the daemon.
#include <cstdio>
#include <cstring>

#include "cast/castbridge/daemon.h"
#include "cast/castbridge/nm_relay.h"

int main(int argc, char* argv[]) {
  const char* mode = argc > 1 ? argv[1] : "";
  if (std::strcmp(mode, "--daemon") == 0) {
    return castbridge::RunDaemon();
  }
  if (std::strcmp(mode, "--nm-host") == 0) {
    return castbridge::RunNmRelay();
  }
  std::fprintf(stderr, "usage: %s --daemon | --nm-host\n", argv[0]);
  return 2;
}
