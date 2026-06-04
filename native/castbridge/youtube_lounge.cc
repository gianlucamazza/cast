#include "cast/castbridge/youtube_lounge.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <regex>
#include <string>

#include "json/reader.h"
#include "json/value.h"

namespace castbridge {

namespace {

constexpr char kTokenUrl[] =
    "https://www.youtube.com/api/lounge/pairing/get_lounge_token_batch";
constexpr char kBindUrl[] = "https://www.youtube.com/api/lounge/bc/bind";

void Log(const std::string& msg) {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  const char* home = std::getenv("HOME");
  std::string base;
  if (xdg && *xdg) {
    base = xdg;
  } else {
    base = std::string(home ? home : "/tmp") + "/.local/state";
  }
  std::string dir = base + "/castbridge";
  mkdir(dir.c_str(), 0700);
  std::ofstream f(dir + "/youtube.log", std::ios::app);
  if (f) {
    f << msg << "\n";
  }
}

// Run curl (no shell). Captures stdout; the trailing "HTTPSTATUS:<n>" line
// (added via -w) is stripped and returned via *http_code.
std::string RunCurl(std::vector<std::string> args,
                    int* http_code,
                    std::string* error,
                    int* curl_exit = nullptr) {
  args.push_back("-w");
  args.push_back("\nHTTPSTATUS:%{http_code}");

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    if (error) *error = "pipe failed";
    return "";
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    if (error) *error = "fork failed";
    return "";
  }
  if (pid == 0) {
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>("curl"));
    for (const auto& a : args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    execvp("curl", argv.data());
    _exit(127);
  }
  close(pipefd[1]);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    out.append(buf, n);
  }
  close(pipefd[0]);
  int wstatus = 0;
  waitpid(pid, &wstatus, 0);
  if (curl_exit) {
    *curl_exit = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
  }

  if (http_code) {
    *http_code = 0;
    auto pos = out.rfind("\nHTTPSTATUS:");
    if (pos != std::string::npos) {
      *http_code = std::atoi(out.c_str() + pos + 12);
      out.erase(pos);
    }
  }
  return out;
}

std::vector<std::string> BaseArgs() {
  return {"-s", "--max-time", "15", "-X", "POST", "-H",
          "Origin: https://www.youtube.com"};
}

// Whether raw poll frames should be dumped to the log for protocol discovery.
bool RawLogEnabled() {
  const char* v = std::getenv("CASTBRIDGE_YT_RAW_LOG");
  return v && *v && std::string(v) != "0";
}

// Map the numeric Lounge playerState to the URL-receiver vocabulary so the
// extension treats YouTube and the default receiver identically.
//   -1 unstarted, 0 ended, 1 playing, 2 paused, 3 buffering, 5 cued
// 1081 was observed on a real TV during ad playback (content is playing) — treat
// it as PLAYING so the UI doesn't get stuck on BUFFERING through the pre-roll.
std::string MapState(const std::string& code) {
  if (code == "1" || code == "1081") return "PLAYING";
  if (code == "2") return "PAUSED";
  if (code == "3") return "BUFFERING";
  if (code == "0") return "IDLE";
  if (code == "-1" || code == "5") return "BUFFERING";  // cued/unstarted
  return "";
}

// A Lounge response is a sequence of length-prefixed chunks:
//   <decimal length>\n<json-array>
// where each array is [[index,[eventName, payload]], ...]. The length counts
// the characters of the JSON that follows. Split the buffer into JSON chunks.
std::vector<std::string> SplitChunks(const std::string& body) {
  std::vector<std::string> chunks;
  size_t i = 0;
  const size_t n = body.size();
  while (i < n) {
    // Read the decimal length run.
    size_t j = i;
    while (j < n && body[j] >= '0' && body[j] <= '9') {
      ++j;
    }
    if (j == i || j >= n || body[j] != '\n') {
      break;  // not a length-prefixed frame; stop (tolerant)
    }
    const long len = std::atol(body.substr(i, j - i).c_str());
    const size_t start = j + 1;
    if (len <= 0 || start + static_cast<size_t>(len) > n) {
      // Length runs past the buffer: take the remainder and stop.
      if (start < n) {
        chunks.push_back(body.substr(start));
      }
      break;
    }
    chunks.push_back(body.substr(start, len));
    i = start + len;
  }
  return chunks;
}

}  // namespace

std::string YouTubeLounge::GetToken(const std::string& screen_id,
                                    std::string* error) {
  std::vector<std::string> args = BaseArgs();
  args.push_back("-d");
  args.push_back("screen_ids=" + screen_id);
  args.push_back(kTokenUrl);
  int code = 0;
  const std::string resp = RunCurl(args, &code, error);
  Log("GetToken http=" + std::to_string(code));

  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errs;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(resp.data(), resp.data() + resp.size(), &root, &errs)) {
    if (error) *error = "token parse failed";
    return "";
  }
  const Json::Value& screens = root["screens"];
  if (!screens.isArray() || screens.empty()) {
    if (error) *error = "no screens in token response";
    return "";
  }
  return screens[0].get("loungeToken", "").asString();
}

std::string YouTubeLounge::Bind(std::string* error) {
  std::string token;
  {
    std::lock_guard<std::mutex> lock(session_mutex_);
    token = token_;
  }
  std::vector<std::string> args = BaseArgs();
  args.push_back("-H");
  args.push_back("X-YouTube-LoungeId-Token: " + token);
  for (const char* kv : {"app=android-phone-13.14.55", "mdx-version=3",
                         "name=castbridge", "id=castbridgecastbridgecast",
                         "device=REMOTE_CONTROL", "pairing_type=cast"}) {
    args.push_back("--data-urlencode");
    args.push_back(kv);
  }
  args.push_back("--data");
  args.push_back("count=0");
  args.push_back(std::string(kBindUrl) + "?RID=" + std::to_string(rid_++) +
                 "&VER=8&CVER=1");
  int code = 0;
  const std::string resp = RunCurl(args, &code, error);

  std::smatch m;
  const std::regex sid_re(R"re(\["c","([^"]+)")re");
  const std::regex g_re(R"re(\["S","([^"]+)"\])re");
  std::string sid, gsession;
  if (std::regex_search(resp, m, sid_re)) {
    sid = m[1];
  }
  if (std::regex_search(resp, m, g_re)) {
    gsession = m[1];
  }
  const bool has_screen =
      resp.find("LOUNGE_SCREEN") != std::string::npos ||
      resp.find("loungeStatus") != std::string::npos;
  Log("Bind http=" + std::to_string(code) + " sid=" + sid +
      " gsession=" + gsession + " screen=" + (has_screen ? "yes" : "no"));
  if (sid.empty() || gsession.empty()) {
    return "bind failed (no SID/gsessionid)";
  }
  {
    std::lock_guard<std::mutex> lock(session_mutex_);
    sid_ = sid;
    gsessionid_ = gsession;
  }
  aid_ = 0;  // fresh session: poll from the start of the event stream
  return "";
}

std::string YouTubeLounge::SendCommand(
    const std::vector<std::string>& req_fields,
    int* http_code,
    std::string* error) {
  std::string token, sid, gsession;
  {
    std::lock_guard<std::mutex> lock(session_mutex_);
    token = token_;
    sid = sid_;
    gsession = gsessionid_;
  }
  std::vector<std::string> args = BaseArgs();
  args.push_back("-H");
  args.push_back("X-YouTube-LoungeId-Token: " + token);
  args.push_back("--data");
  args.push_back("count=1");
  args.push_back("--data");
  args.push_back("ofs=" + std::to_string(ofs_++));
  for (const auto& f : req_fields) {
    args.push_back("--data-urlencode");
    args.push_back(f);
  }
  args.push_back(std::string(kBindUrl) + "?RID=" + std::to_string(rid_++) +
                 "&SID=" + sid + "&gsessionid=" + gsession +
                 "&VER=8&CVER=1");
  int code = 0;
  const std::string resp = RunCurl(args, &code, error);
  Log("SendCommand http=" + std::to_string(code) + " body=" + resp.substr(0, 120));
  if (http_code) {
    *http_code = code;
  }
  if (code < 200 || code >= 300) {
    return "command HTTP " + std::to_string(code);
  }
  return "";
}

PollResult YouTubeLounge::Poll(YouTubeStatus* out, std::string* error) {
  std::string token, sid, gsession;
  {
    std::lock_guard<std::mutex> lock(session_mutex_);
    token = token_;
    sid = sid_;
    gsession = gsessionid_;
  }
  if (sid.empty() || gsession.empty()) {
    if (error) *error = "no lounge session";
    return PollResult::kError;
  }

  // GET the event channel. RID=rpc marks a server->client read; AID is the last
  // event index we have acknowledged, so we only get newer frames.
  const int aid = aid_.load();
  std::vector<std::string> args = {
      "-s", "--max-time", "30", "-H", "Origin: https://www.youtube.com",
      "-H", "X-YouTube-LoungeId-Token: " + token,
      std::string(kBindUrl) + "?RID=rpc&SID=" + sid + "&gsessionid=" + gsession +
          "&AID=" + std::to_string(aid) +
          "&CI=0&TYPE=xmlhttp&VER=8&CVER=1&zx=castbridge"};
  int code = 0;
  int curl_exit = 0;
  std::string err;
  const std::string resp = RunCurl(args, &code, &err, &curl_exit);
  if (RawLogEnabled()) {
    Log("Poll http=" + std::to_string(code) + " exit=" +
        std::to_string(curl_exit) + " aid=" + std::to_string(aid) +
        " raw=" + resp);
  }
  if (code == 400 || code == 401 || code == 403) {
    if (error) *error = "lounge session expired (HTTP " + std::to_string(code) + ")";
    return PollResult::kNeedRefresh;
  }
  // Parse the body BEFORE inspecting the exit code: the Lounge long-poll
  // normally holds the connection open and streams a batch of events, then curl
  // hits --max-time (exit 28) — so the events arrive precisely on a "timeout".
  // Treating exit 28 as no-data here would discard the whole event batch.

  bool got = false;
  YouTubeStatus st;
  st.active = true;
  int max_index = aid;
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

  for (const std::string& chunk : SplitChunks(resp)) {
    Json::Value arr;
    std::string perr;
    if (!reader->parse(chunk.data(), chunk.data() + chunk.size(), &arr, &perr) ||
        !arr.isArray()) {
      continue;
    }
    for (const Json::Value& entry : arr) {
      // entry = [index, [eventName, payload?]]
      if (!entry.isArray() || entry.size() < 2) {
        continue;
      }
      const int index = entry[0].asInt();
      if (index > max_index) {
        max_index = index;
      }
      const Json::Value& ev = entry[1];
      if (!ev.isArray() || ev.empty()) {
        continue;
      }
      const std::string name = ev[0].asString();
      if ((name != "onStateChange" && name != "nowPlaying") || ev.size() < 2) {
        continue;
      }
      const Json::Value& p = ev[1];
      if (!p.isObject()) {
        continue;
      }
      // Lounge sends numbers as strings; accept either.
      auto num = [](const Json::Value& v) -> double {
        if (v.isNumeric()) return v.asDouble();
        if (v.isString()) return std::atof(v.asCString());
        return 0.0;
      };
      auto str = [](const Json::Value& v) -> std::string {
        return v.isString() ? v.asString()
                            : (v.isNumeric() ? std::to_string(v.asInt()) : "");
      };
      const std::string mapped = MapState(str(p["state"]));
      if (!mapped.empty()) {
        st.state = mapped;
        got = true;  // only a real, mapped state counts as a usable update
      }
      if (p.isMember("currentTime")) st.position = num(p["currentTime"]);
      if (p.isMember("duration")) st.duration = num(p["duration"]);
      if (p.isMember("videoId")) st.video_id = str(p["videoId"]);
    }
  }

  if (max_index > aid) {
    aid_.store(max_index);
  }
  if (got && out) {
    *out = st;
    return PollResult::kStatus;
  }
  // No usable playback event. A timeout (exit 28) is the normal idle long-poll;
  // a non-2xx with no body is a real error worth backing off on.
  if (curl_exit == 28) {
    return PollResult::kNoChange;
  }
  if (code < 200 || code >= 300) {
    if (error) *error = err.empty() ? ("poll HTTP " + std::to_string(code)) : err;
    return PollResult::kError;
  }
  return PollResult::kNoChange;
}

// Re-fetch the lounge token and re-bind a session for the saved screen. Used
// when the token/session has expired mid-session (~6h lifetime).
std::string YouTubeLounge::Refresh() {
  if (screen_id_.empty()) {
    return "no screen to refresh";
  }
  std::string err;
  const std::string token = GetToken(screen_id_, &err);
  if (token.empty()) {
    return err.empty() ? "could not refresh lounge token" : err;
  }
  {
    std::lock_guard<std::mutex> lock(session_mutex_);
    token_ = token;
    sid_.clear();
    gsessionid_.clear();
  }
  rid_ = 1;
  ofs_ = 0;
  return Bind(&err);  // resets aid_ and sets sid_/gsessionid_ under the lock
}

std::string YouTubeLounge::Start(const std::string& screen_id,
                                 const std::string& video_id,
                                 double start_time) {
  std::string err;
  screen_id_ = screen_id;
  token_ = GetToken(screen_id, &err);
  if (token_.empty()) {
    return err.empty() ? "could not get lounge token" : err;
  }
  err = Bind(&err);
  if (!err.empty()) {
    return err;
  }
  // Param keys must match the YouTube Lounge exactly (from casttube): only the
  // action key "__sc" has a double underscore; videoId/currentTime/etc. have a
  // single one. A double underscore on videoId is silently ignored → the app
  // stays on "Ready to cast".
  const std::vector<std::string> fields = {
      "req0__sc=setPlaylist",
      "req0_videoId=" + video_id,
      "req0_currentTime=" + std::to_string(static_cast<long>(start_time)),
      "req0_currentIndex=-1",
      "req0_audioOnly=false",
  };
  // Cold-start race: a freshly-launched YouTube app may not be registered on
  // the Lounge backend yet, so the first setPlaylist is a no-op ("Ready to
  // cast"). setPlaylist is idempotent — send it a few times, spaced out, until
  // it takes.
  std::string last = "setPlaylist not sent";
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (attempt > 0) {
      usleep(1300 * 1000);
    }
    last = SendCommand(fields, nullptr, &err);
    if (last.empty()) {
      // HTTP ok; resend once more after a beat on the first success to cover
      // the registration race, then consider it done.
      if (attempt == 0) {
        continue;
      }
      return "";
    }
  }
  return last.empty() ? "" : last;
}

std::string YouTubeLounge::Command(const std::string& sc, double new_time) {
  if (!valid()) {
    return "no lounge session";
  }
  std::string err;
  std::vector<std::string> fields = {"req0__sc=" + sc};
  if (sc == "seekTo") {
    fields.push_back("req0_newTime=" +
                     std::to_string(static_cast<long>(new_time)));
  }
  int code = 0;
  std::string res = SendCommand(fields, &code, &err);
  if (res.empty()) {
    return "";
  }
  // Expired session/token: the Lounge answers 400 ("Unknown SID") or 401/403.
  // Re-authenticate once and retry the command.
  if (code == 400 || code == 401 || code == 403) {
    Log("Command http=" + std::to_string(code) + " — refreshing lounge session");
    const std::string re = Refresh();
    if (!re.empty()) {
      return re;
    }
    res = SendCommand(fields, &code, &err);
  }
  return res;
}

}  // namespace castbridge
