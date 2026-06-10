// Minimal AF_UNIX stream IPC server with a poll() loop. Messages are
// newline-delimited JSON. The request handler is invoked on the server thread
// (the one that called Run()); Send()/Broadcast()/Stop() are thread-safe and
// wake the loop via an eventfd, so the TaskRunner thread can push responses and
// events without blocking.
#ifndef CAST_CASTBRIDGE_IPC_SERVER_H_
#define CAST_CASTBRIDGE_IPC_SERVER_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace castbridge {

class IpcServer {
 public:
  // conn_id identifies the client connection; line is one JSON message. The id
  // is monotonic (never an fd), so an async reply can never be misdelivered to
  // a newer client that happened to reuse the same file descriptor.
  using RequestHandler =
      std::function<void(uint64_t conn_id, const std::string& line)>;

  explicit IpcServer(std::string socket_path);
  ~IpcServer();

  void set_request_handler(RequestHandler handler) {
    handler_ = std::move(handler);
  }

  // Bind+listen, then run the poll loop until Stop(). Returns false if the
  // socket could not be created. Runs on the calling thread.
  bool Run();

  // Thread-safe. Queue a JSON message (a newline is appended) to one client or
  // to all clients respectively. Sending to a connection that has since closed
  // is a no-op.
  void Send(uint64_t conn_id, const std::string& json);
  void Broadcast(const std::string& json);

  // Thread-safe. Unblock Run() and shut the server down.
  void Stop();

 private:
  struct Conn {
    uint64_t id = 0;  // stable handle handed to the request handler
    std::string in;   // accumulated inbound bytes (split on '\n')
    std::deque<std::string>
        out;  // pending outbound frames (newline-terminated)
    // The peer half-closed its write side (shutdown(SHUT_WR)) but may still be
    // reading: keep the connection open to deliver the response, but stop
    // polling it for input. Set after an EOF on read.
    bool read_closed = false;
  };

  void Wake();
  void DrainWake();
  void CloseConn(int fd);
  void QueueLocked(int fd, const std::string& json);

  // Per-connection I/O steps used by the Run() loop, all on the server thread.
  bool FlushWrites(int fd);    // drain the out-queue; true if closed on error
  bool DrainReadable(int fd);  // read into Conn::in; true on peer EOF/error
  void DispatchLines(int fd);  // dispatch complete '\n'-terminated requests

  const std::string socket_path_;
  RequestHandler handler_;

  int listen_fd_ = -1;
  int wake_fd_ = -1;  // eventfd

  std::mutex mutex_;
  std::map<int, Conn> conns_;         // fd -> connection state (guarded)
  std::map<uint64_t, int> id_to_fd_;  // conn id -> live fd (guarded)
  uint64_t next_conn_id_ = 0;         // guarded by mutex_
  std::atomic<bool> running_{false};
};

}  // namespace castbridge

#endif  // CAST_CASTBRIDGE_IPC_SERVER_H_
