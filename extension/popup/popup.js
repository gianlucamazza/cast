/* Cast — popup UI. Talks to the background page (which owns the native port). */

const $ = (id) => document.getElementById(id);

let devices = [];
let selectedId = "";
let activeTab = null;
let castability = null;
let session = { session: "idle", media: null, mirror: null };
let seeking = false;
let ytPaused = false;

function host(action, args = {}) {
  return browser.runtime.sendMessage({ type: "host", action, args });
}

function setStatus(msg) {
  $("status-line").textContent = msg || "";
}

function fmt(t) {
  t = Math.max(0, Math.floor(t || 0));
  const m = Math.floor(t / 60);
  const s = t % 60;
  return `${m}:${String(s).padStart(2, "0")}`;
}

async function init() {
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

  // Refresh device list from the daemon (live mDNS).
  host("devices").then((r) => {
    if (r && r.ok) {
      devices = r.data.candidates || [];
      ensureSelection();
      renderDevices();
    }
  });
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

function ensureSelection() {
  if (!devices.length) return;
  if (!selectedId || !devices.some((d) => d.id === selectedId)) {
    selectedId = devices[0].id;
    browser.storage.local.set({ deviceId: selectedId });
  }
}

function selectedName() {
  const d = devices.find((x) => x.id === selectedId);
  return d ? d.name : "No TV";
}

function renderDevices() {
  $("device-chip").textContent = devices.length ? selectedName() : "No TV";
  const ul = $("device-list");
  ul.replaceChildren();
  for (const d of devices) {
    const li = document.createElement("li");
    if (d.id === selectedId) li.className = "selected";
    const name = document.createElement("span");
    name.textContent = d.name;
    const model = document.createElement("span");
    model.className = "model";
    model.textContent = d.model || "";
    li.append(name, model);
    li.addEventListener("click", () => {
      selectedId = d.id;
      browser.storage.local.set({ deviceId: selectedId });
      renderDevices();
      $("devices").classList.add("hidden");
    });
    ul.appendChild(li);
  }
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
    renderYouTube(session.youtube || { title: "YouTube" });
  } else if (session.session === "mirror" && session.mirror) {
    const m = session.mirror;
    $("mirror-target").textContent = `${m.mode === "window" ? "window" : "screen"}: ${m.target || ""} → ${m.device || ""}`;
  } else {
    renderIdle();
  }
}

function renderYouTube(y) {
  $("media-title").textContent = y.title || "YouTube";
  // The Lounge API doesn't give us reliable position, so hide the scrubber.
  $("seek").style.display = "none";
  document.querySelector(".times").style.display = "none";
  $("playpause").textContent = "⏯";
}

function renderIdle() {
  const c = castability;
  const btn = $("cast-video");
  if (c && c.best && !c.drm) {
    btn.disabled = false;
    btn.textContent = c.best.kind === "site" ? "Cast this page's video" : "Cast this video";
    $("hint").textContent = "";
  } else {
    btn.disabled = true;
    btn.textContent = "Cast this video";
    $("hint").textContent = c && c.drm ? "Protected media (DRM) — mirror the window instead." : "No castable video detected on this tab.";
  }
}

function renderMedia(m) {
  $("seek").style.display = "";
  document.querySelector(".times").style.display = "";
  $("media-title").textContent = m.title || "Playing";
  const dur = m.duration || 0;
  const pos = m.position || 0;
  if (!seeking) {
    $("seek").max = Math.max(1, Math.floor(dur));
    $("seek").value = Math.floor(pos);
  }
  $("pos").textContent = fmt(pos);
  $("dur").textContent = fmt(dur);
  $("playpause").textContent = m.state === "PAUSED" ? "▶" : "⏸";
}

function show(id, on) {
  $(id).classList.toggle("hidden", !on);
}

function reply(r, okMsg) {
  if (r && r.ok) {
    setStatus(okMsg || "");
  } else {
    const e = (r && r.error) || {};
    if (e.code === "ambiguous") setStatus("Several TVs — pick one above.");
    else if (e.code === "no_devices") setStatus("No Chromecast on this network.");
    else if (e.code === "nohost") setStatus("Native host not installed.");
    else setStatus(e.message || "Error");
  }
}

function bind() {
  $("device-chip").addEventListener("click", () => $("devices").classList.toggle("hidden"));

  $("cast-video").addEventListener("click", async () => {
    if (!castability || !castability.best) return;
    const best = castability.best;
    setStatus("Casting…");
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
    reply(r, "Casting.");
    if (r && r.ok) refreshStatus();
  });

  $("mirror-window").addEventListener("click", async () => {
    setStatus("Starting mirror…");
    const r = await host("mirror-window", { deviceId: selectedId, selector: "librewolf" });
    reply(r, "Mirroring window.");
    if (r && r.ok) refreshStatus();
  });

  $("mirror-screen").addEventListener("click", async () => {
    setStatus("Starting mirror…");
    const r = await host("mirror-screen", { deviceId: selectedId });
    reply(r, "Mirroring screen.");
    if (r && r.ok) refreshStatus();
  });

  $("playpause").addEventListener("click", async () => {
    if (session.session === "youtube") {
      // Lounge gives us no playback state, so this toggle is optimistic: we
      // assume "playing" after load and flip on each press. Roll back on error.
      const was = ytPaused;
      ytPaused = !ytPaused;
      $("playpause").textContent = ytPaused ? "▶" : "⏸";
      const r = await host("media-control", { cmd: ytPaused ? "pause" : "play" });
      if (!r || !r.ok) {
        ytPaused = was;
        $("playpause").textContent = "⏯";
        reply(r);
      }
      return;
    }
    const paused = session.media && session.media.state === "PAUSED";
    const r = await host("media-control", { cmd: paused ? "play" : "pause" });
    if (!r || !r.ok) reply(r);
  });

  const stopOptimistic = () => {
    // Reflect idle immediately; fire the stop and reconcile shortly after.
    session = { session: "idle", media: null, mirror: null };
    ytPaused = false;
    render();
    setStatus("Stopped.");
    host("stop").catch(() => {});
    setTimeout(refreshStatus, 1200);
  };
  $("stop-media").addEventListener("click", stopOptimistic);
  $("stop-mirror").addEventListener("click", stopOptimistic);

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
    const r = await host("media-control", { cmd: "volume", value: Number(e.target.value) });
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
browser.runtime.onMessage.addListener((msg) => {
  if (msg && msg.type === "cast-event") {
    const ev = msg.event;
    if (ev.type === "media-status") {
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
      ensureSelection();
      renderDevices();
    }
  }
});

init();
