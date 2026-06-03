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
                    std::string* error) {
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
  waitpid(pid, nullptr, 0);

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
  std::vector<std::string> args = BaseArgs();
  args.push_back("-H");
  args.push_back("X-YouTube-LoungeId-Token: " + token_);
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
  if (std::regex_search(resp, m, sid_re)) {
    sid_ = m[1];
  }
  if (std::regex_search(resp, m, g_re)) {
    gsessionid_ = m[1];
  }
  const bool has_screen =
      resp.find("LOUNGE_SCREEN") != std::string::npos ||
      resp.find("loungeStatus") != std::string::npos;
  Log("Bind http=" + std::to_string(code) + " sid=" + sid_ +
      " gsession=" + gsessionid_ + " screen=" + (has_screen ? "yes" : "no"));
  if (sid_.empty() || gsessionid_.empty()) {
    return "bind failed (no SID/gsessionid)";
  }
  return "";
}

std::string YouTubeLounge::SendCommand(
    const std::vector<std::string>& req_fields,
    int* http_code,
    std::string* error) {
  std::vector<std::string> args = BaseArgs();
  args.push_back("-H");
  args.push_back("X-YouTube-LoungeId-Token: " + token_);
  args.push_back("--data");
  args.push_back("count=1");
  args.push_back("--data");
  args.push_back("ofs=" + std::to_string(ofs_++));
  for (const auto& f : req_fields) {
    args.push_back("--data-urlencode");
    args.push_back(f);
  }
  args.push_back(std::string(kBindUrl) + "?RID=" + std::to_string(rid_++) +
                 "&SID=" + sid_ + "&gsessionid=" + gsessionid_ +
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

// Re-fetch the lounge token and re-bind a session for the saved screen. Used
// when the token/session has expired mid-session (~6h lifetime).
std::string YouTubeLounge::Refresh() {
  if (screen_id_.empty()) {
    return "no screen to refresh";
  }
  std::string err;
  token_ = GetToken(screen_id_, &err);
  if (token_.empty()) {
    return err.empty() ? "could not refresh lounge token" : err;
  }
  sid_.clear();
  gsessionid_.clear();
  rid_ = 1;
  ofs_ = 0;
  return Bind(&err);
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
