/* Cast — popup UI. Talks to the background page (which owns the native port). */

const $ = (id) => document.getElementById(id);
const msg = (k) => browser.i18n.getMessage(k) || k;

let devices = [];
let devicesScanned = false;
let selectedId = "";
let activeTab = null;
let castability = null;
let session = { session: "idle", media: null, mirror: null };
let seeking = false;
let muted = false;
let lastVolume = 50;

function host(action, args = {}) {
  return browser.runtime.sendMessage({ type: "host", action, args });
}

function setStatus(text) {
  $("status-line").textContent = text || "";
}

function fmt(t) {
  t = Math.max(0, Math.floor(t || 0));
  const m = Math.floor(t / 60);
  const s = t % 60;
  return `${m}:${String(s).padStart(2, "0")}`;
}

// Replace static strings in markup with their localized text.
function applyI18n() {
  for (const el of document.querySelectorAll("[data-i18n]")) {
    el.textContent = msg(el.dataset.i18n);
  }
  for (const el of document.querySelectorAll("[data-i18n-label]")) {
    el.setAttribute("aria-label", msg(el.dataset.i18nLabel));
  }
  for (const el of document.querySelectorAll("[data-i18n-title]")) {
    el.title = msg(el.dataset.i18nTitle);
  }
}

// Disable a button and show a spinner while an async action is in flight.
async function withBusy(btn, fn) {
  btn.classList.add("busy");
  btn.setAttribute("aria-busy", "true");
  try {
    return await fn();
  } finally {
    btn.classList.remove("busy");
    btn.removeAttribute("aria-busy");
  }
}

async function init() {
  applyI18n();

  const tabs = await browser.tabs.query({ active: true, currentWindow: true });
  activeTab = tabs[0] || null;

  const stored = await browser.storage.local.get("deviceId");
  selectedId = stored.deviceId || "";

  const state = await browser.runtime.sendMessage({
    type: "get-state",
    tabId: activeTab ? activeTab.id : null,
  });
  session = state.session || session;
  devices = state.devices || [];
  castability = state.castability || null;
  devicesScanned = devices.length > 0;

  // Refresh device list from the daemon (live mDNS).
  loadDevices();
  host("status").then((r) => {
    if (r && r.ok) {
      session = r.data;
      render();
    }
  });

  ensureSelection();
  renderDevices();
  render();
  bind();
}

function loadDevices() {
  devicesScanned = false;
  renderDevices();
  return host("devices").then((r) => {
    if (r && r.ok) {
      devices = r.data.candidates || [];
      ensureSelection();
    }
    devicesScanned = true;
    renderDevices();
  });
}

function ensureSelection() {
  if (!devices.length) return;
  if (!selectedId || !devices.some((d) => d.id === selectedId)) {
    selectedId = devices[0].id;
    persistSelection();
  }
}

// Persist id + friendly name so the in-page YouTube overlay can label the
// device ("Playing on <name>") even when the cast is launched from the popup.
function persistSelection() {
  browser.storage.local.set({ deviceId: selectedId, deviceName: selectedName() });
}

function selectedName() {
  const d = devices.find((x) => x.id === selectedId);
  return d ? d.name : msg("noTv");
}

function selectDevice(id) {
  selectedId = id;
  persistSelection();
  renderDevices();
  toggleDevices(false);
}

function toggleDevices(open) {
  const panel = $("devices");
  const willOpen = open === undefined ? panel.classList.contains("hidden") : open;
  panel.classList.toggle("hidden", !willOpen);
  $("device-chip").setAttribute("aria-expanded", String(willOpen));
  if (willOpen) {
    const sel = $("device-list").querySelector('[aria-selected="true"]') || $("device-list").firstElementChild;
    if (sel) sel.focus();
  }
}

function renderDevices() {
  $("device-chip").textContent = devices.length ? selectedName() : msg("noTv");
  const ul = $("device-list");
  ul.replaceChildren();
  for (const d of devices) {
    const li = document.createElement("li");
    li.setAttribute("role", "option");
    li.tabIndex = 0;
    const selected = d.id === selectedId;
    li.setAttribute("aria-selected", String(selected));
    if (selected) li.className = "selected";
    const name = document.createElement("span");
    name.textContent = d.name;
    const model = document.createElement("span");
    model.className = "model";
    model.textContent = d.model || "";
    li.append(name, model);
    li.addEventListener("click", () => selectDevice(d.id));
    li.addEventListener("keydown", (e) => {
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        selectDevice(d.id);
      }
    });
    ul.appendChild(li);
  }
  const empty = devices.length === 0;
  $("devices-scanning").classList.toggle("hidden", devicesScanned);
  $("devices-empty").classList.toggle("hidden", !(devicesScanned && empty));
  ul.classList.toggle("hidden", empty);
}

function render() {
  const chip = $("device-chip");
  chip.classList.toggle("active", session.session !== "idle");

  const mediaLike = session.session === "media" || session.session === "youtube";
  show("idle", session.session === "idle");
  show("media", mediaLike);
  show("mirror", session.session === "mirror");

  if (session.session === "media" && session.media) {
    renderMedia(session.media);
  } else if (session.session === "youtube") {
    renderYouTube(session.youtube || { title: msg("youtube") });
  } else if (session.session === "mirror" && session.mirror) {
    const m = session.mirror;
    $("mirror-target").textContent = `${m.mode === "window" ? "window" : "screen"}: ${m.target || ""} → ${m.device || ""}`;
  } else {
    renderIdle();
  }
}

function setPlayPause(paused) {
  const btn = $("playpause");
  btn.classList.toggle("is-paused", paused);
  btn.setAttribute("aria-label", msg(paused ? "play" : "pause"));
}

function renderYouTube(y) {
  $("media-title").textContent = y.title || msg("youtube");
  // No interactive scrubber for YouTube, but the play/pause state is real now
  // (pushed from the Lounge event channel).
  $("seek").style.display = "none";
  document.querySelector(".times").style.display = "none";
  setPlayPause(y.state === "PAUSED");
}

function renderIdle() {
  const c = castability;
  const btn = $("cast-video");
  if (c && c.best && !c.drm) {
    btn.disabled = false;
    btn.textContent = c.best.kind === "site" ? msg("castPageVideo") : msg("castVideo");
    $("hint").textContent = "";
  } else {
    btn.disabled = true;
    btn.textContent = msg("castVideo");
    $("hint").textContent = c && c.drm ? msg("hintDrm") : msg("hintNoMedia");
  }
}

function renderMedia(m) {
  $("seek").style.display = "";
  document.querySelector(".times").style.display = "";
  $("media-title").textContent = m.title || msg("playing");
  const dur = m.duration || 0;
  const pos = m.position || 0;
  if (!seeking) {
    $("seek").max = Math.max(1, Math.floor(dur));
    $("seek").value = Math.floor(pos);
  }
  $("pos").textContent = fmt(pos);
  $("dur").textContent = fmt(dur);
  setPlayPause(m.state === "PAUSED");
}

function show(id, on) {
  $(id).classList.toggle("hidden", !on);
}

function reply(r, okMsg) {
  if (r && r.ok) {
    setStatus(okMsg || "");
  } else {
    const e = (r && r.error) || {};
    setStatus(CastErrors.errorMessage(e.code, e.message));
  }
}

function setMuteUi() {
  const btn = $("mute");
  btn.classList.toggle("is-muted", muted);
  btn.setAttribute("aria-pressed", String(muted));
  btn.setAttribute("aria-label", msg(muted ? "unmute" : "mute"));
}

function bind() {
  $("device-chip").addEventListener("click", () => toggleDevices());
  $("refresh-devices").addEventListener("click", () => loadDevices());

  $("cast-video").addEventListener("click", (e) => {
    if (!castability || !castability.best) return;
    const best = castability.best;
    return withBusy(e.currentTarget, async () => {
      setStatus(msg("statusCasting"));
      let r;
      if (best.kind === "youtube") {
        r = await host("youtube-load", {
          deviceId: selectedId,
          videoId: best.videoId,
          currentTime: best.startTime || 0,
        });
      } else {
        r = await host("media-load", {
          deviceId: selectedId,
          url: best.castUrl,
          title: best.title || (activeTab && activeTab.title) || "",
        });
      }
      reply(r, msg("statusCast"));
      if (r && r.ok) refreshStatus();
    });
  });

  $("mirror-window").addEventListener("click", (e) =>
    withBusy(e.currentTarget, async () => {
      setStatus(msg("statusMirrorStarting"));
      const r = await host("mirror-window", { deviceId: selectedId, selector: "librewolf" });
      reply(r, msg("statusMirroringWindow"));
      if (r && r.ok) refreshStatus();
    })
  );

  $("mirror-screen").addEventListener("click", (e) =>
    withBusy(e.currentTarget, async () => {
      setStatus(msg("statusMirrorStarting"));
      const r = await host("mirror-screen", { deviceId: selectedId });
      reply(r, msg("statusMirroringScreen"));
      if (r && r.ok) refreshStatus();
    })
  );

  $("playpause").addEventListener("click", async () => {
    // Real state for both receivers now: YouTube from the Lounge event channel,
    // the URL receiver from media-status. The daemon push reconciles the button.
    const sub = session.session === "youtube" ? session.youtube : session.media;
    const paused = sub && sub.state === "PAUSED";
    const r = await host("media-control", { cmd: paused ? "play" : "pause" });
    if (!r || !r.ok) reply(r);
  });

  const stopOptimistic = () => {
    // Reflect idle immediately; fire the stop and reconcile shortly after.
    session = { session: "idle", media: null, mirror: null };
    render();
    setStatus(msg("statusStopped"));
    host("stop").catch(() => {});
    setTimeout(refreshStatus, 1200);
  };
  $("stop-media").addEventListener("click", stopOptimistic);
  $("stop-mirror").addEventListener("click", stopOptimistic);

  $("mute").addEventListener("click", async () => {
    muted = !muted;
    setMuteUi();
    const vol = muted ? 0 : lastVolume;
    $("volume").value = vol;
    const r = await host("media-control", { cmd: "volume", value: vol });
    if (!r || !r.ok) reply(r);
  });

  const seek = $("seek");
  seek.addEventListener("input", () => {
    seeking = true;
  });
  seek.addEventListener("change", async () => {
    const r = await host("media-control", { cmd: "seek", value: Number(seek.value) });
    seeking = false;
    // On failure, surface it and pull the slider back to the real position.
    if (!r || !r.ok) {
      reply(r);
      refreshStatus();
    }
  });

  $("volume").addEventListener("change", async (e) => {
    const v = Number(e.target.value);
    if (v > 0) lastVolume = v;
    muted = v === 0;
    setMuteUi();
    const r = await host("media-control", { cmd: "volume", value: v });
    if (!r || !r.ok) reply(r);
  });
}

async function refreshStatus() {
  const r = await host("status");
  if (r && r.ok) {
    session = r.data;
    render();
  }
}

// Live updates pushed by the daemon (relayed by background).
browser.runtime.onMessage.addListener((m) => {
  if (m && m.type === "cast-event") {
    const ev = m.event;
    if (ev.type === "session") {
      // Authoritative session state (incl. real YouTube play/pause) pushed by
      // the daemon; render it directly.
      session = ev.data || { session: "idle", media: null, mirror: null };
      render();
    } else if (ev.type === "media-status") {
      // media-status applies to the URL receiver. Don't clobber a YouTube
      // session (which has no status push) on a null/idle frame.
      if (ev.data) {
        session = { session: "media", media: ev.data, mirror: null };
        render();
      } else if (session.session !== "youtube") {
        session = { session: "idle", media: null, mirror: null };
        render();
      }
    } else if (ev.type === "devices-changed") {
      devices = (ev.data && ev.data.candidates) || [];
      devicesScanned = true;
      ensureSelection();
      renderDevices();
    }
  }
});

init();
