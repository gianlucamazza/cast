// Native-messaging relay. Bridges Firefox's stdin/stdout (uint32 length prefix
// in native byte order + JSON) to the daemon's AF_UNIX socket (newline-
// delimited JSON). Forwards both replies and unsolicited push events back to
// the browser. Auto-spawns the daemon if the socket is absent.
#ifndef CAST_CASTBRIDGE_NM_RELAY_H_
#define CAST_CASTBRIDGE_NM_RELAY_H_

#include <string>

namespace castbridge {

// Returns the daemon socket path ($XDG_RUNTIME_DIR/castbridge/sock, with a
// /tmp fallback).
std::string SocketPath();

// Run the native-messaging relay loop. Returns a process exit code.
int RunNmRelay();

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_NM_RELAY_H_
