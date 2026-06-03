// The castbridge daemon: owns an openscreen TaskRunner (on a worker thread),
// runs headless Cast discovery, and serves an AF_UNIX IPC socket (on the main
// thread) speaking newline-delimited JSON. Phase A handles discovery/status;
// later phases add media and mirror sessions.
#ifndef CAST_CASTBRIDGE_DAEMON_H_
#define CAST_CASTBRIDGE_DAEMON_H_

namespace castbridge {

// Run the daemon until SIGINT/SIGTERM. Returns a process exit code.
int RunDaemon();

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_DAEMON_H_
