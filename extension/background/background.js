/* Cast — background event page.
 *
 * Owns the single native-messaging port to the castbridge daemon (via the
 * `castbridge --nm-host` relay), the per-tab castability map (fed by the content
 * script), the toolbar badge, and the context menus. Speaks the daemon's action
 * protocol (devices / media-load / media-control / mirror-window / mirror-screen
 * / status / stop) and consumes its push events (media-status, devices-changed).
 *
 * Popup and content scripts talk only to this page; only this page talks to the
 * native host.
 */

const HOST_NAME = "it.gianlucamazza.castbridge";

// --- native messaging port -------------------------------------------------

let port = null;
let nextId = 1;
const pending = new Map(); // id -> resolve

function ensurePort() {
  if (port) return port;
  port = browser.runtime.connectNative(HOST_NAME);
  port.onMessage.addListener((msg) => {
    if (msg && msg.id != null && pending.has(msg.id)) {
      const resolve = pending.get(msg.id);
      pending.delete(msg.id);
      resolve(msg);
      return;
    }
    handleEvent(msg); // unsolicited push event
  });
  port.onDisconnect.addListener(() => {
    const err = browser.runtime.lastError;
    const message = err ? err.message : "native host disconnected";
    for (const resolve of pending.values()) {
      resolve({ ok: false, error: { code: "disconnected", message } });
    }
    pending.clear();
    port = null;
  });
  return port;
}

// A disconnect message that means the host isn't installed / can't launch —
// retrying won't help, so we surface an actionable error instead.
const HOST_MISSING_RE = /no such native|not found|failed to (start|connect|execute|launch)/i;

/** Send one request to the daemon; resolve with its {ok, data, error} reply. */
function callOnce(action, args = {}) {
  return new Promise((resolve) => {
    let p;
    try {
      p = ensurePort();
    } catch (e) {
      resolve({ ok: false, error: { code: "noport", message: String(e) } });
      return;
    }
    const id = String(nextId++);
    pending.set(id, resolve);
    try {
      p.postMessage({ id, action, args });
    } catch (e) {
      pending.delete(id);
      resolve({ ok: false, error: { code: "send", message: String(e) } });
      return;
    }
    setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id);
        resolve({ ok: false, error: { code: "timeout", message: "no response from host" } });
      }
    }, 40000);
  });
}

/**
 * Send a request, retrying once on a transient port drop. The relay auto-starts
 * the daemon, so a first connect can race a cold start; a single backed-off
 * retry recovers it. A genuinely missing host is reclassified as `nohost`.
 */
async function call(action, args = {}, attempt = 0) {
  const reply = await callOnce(action, args);
  if (reply.ok || !reply.error) return reply;

  const transient = reply.error.code === "disconnected" || reply.error.code === "send";
  const missing = HOST_MISSING_RE.test(reply.error.message || "");

  if (transient && !missing && attempt < 1) {
    port = null; // force a fresh connectNative on the retry
    await new Promise((r) => setTimeout(r, 300));
    return call(action, args, attempt + 1);
  }
  if (missing || reply.error.code === "noport") {
    reply.error.code = "nohost";
    reply.error.message = "Native host not installed — run install/install-host.sh.";
  }
  return reply;
}

// --- session + device state ------------------------------------------------

let session = { session: "idle", media: null, mirror: null };
let devices = [];
const castability = new Map(); // tabId -> aggregated {best, count, drm}
const framesByTab = new Map(); // tabId -> Map(frameId -> {best, count, drm})

// Merge per-frame detection into one result. A video often lives in an iframe
// while the top frame reports nothing, so we must not let an empty top frame
// hide a sub-frame's candidate. Prefer the top frame's own best, then any
// frame's best; only report DRM when nothing castable was found anywhere.
function aggregateFrames(frames) {
  let best = null;
  let topBest = null;
  let drm = false;
  let count = 0;
  for (const [fid, r] of frames) {
    count += r.count || 0;
    if (r.drm) drm = true;
    if (r.best) {
      if (fid === 0) topBest = r.best;
      if (!best) best = r.best;
    }
  }
  best = topBest || best;
  return { best, count, drm: best ? false : drm };
}

async function preferredDeviceId() {
  const { deviceId } = await browser.storage.local.get("deviceId");
  return deviceId || "";
}

function handleEvent(msg) {
  if (!msg || !msg.type) return;
  if (msg.type === "session") {
    // Authoritative session state pushed by the daemon (start/stop/natural end).
    session = msg.data || { session: "idle" };
    updateActiveBadge();
  } else if (msg.type === "media-status") {
    // Live position only; update media sub-state without clobbering a
    // mirror/youtube session.
    if (session.session === "media" || (msg.data && session.session === "idle")) {
      session = { session: msg.data ? "media" : "idle", media: msg.data, mirror: null };
      updateActiveBadge();
    } else if (session.session === "media") {
      session.media = msg.data;
    }
  } else if (msg.type === "devices-changed") {
    devices = (msg.data && msg.data.candidates) || [];
  } else if (msg.type === "session-ended") {
    session = { session: "idle", media: null, mirror: null };
    updateActiveBadge();
  }
  // Relay to the popup if it is open (ignored if no receiver).
  browser.runtime.sendMessage({ type: "cast-event", event: msg }).catch(() => {});
}

// --- badge -----------------------------------------------------------------

async function activeTabId() {
  const tabs = await browser.tabs.query({ active: true, currentWindow: true });
  return tabs[0] ? tabs[0].id : null;
}

async function updateActiveBadge() {
  manageReconcileAlarm();
  const tabId = await activeTabId();
  if (tabId != null) updateBadgeForTab(tabId);
}

// While a session is active, run a low-frequency reconcile so the badge
// self-heals after an MV3 event-page suspension (when push events are missed).
// No alarm while idle, so the daemon isn't kept alive needlessly.
function manageReconcileAlarm() {
  if (session.session && session.session !== "idle") {
    browser.alarms.create("cast-reconcile", { periodInMinutes: 1 });
  } else {
    browser.alarms.clear("cast-reconcile");
  }
}

async function reconcile() {
  try {
    const r = await call("status");
    if (r && r.ok && r.data) {
      session = r.data;
      updateActiveBadge();
    }
  } catch (_e) {
    /* daemon unreachable; leave state as-is until next tick */
  }
}

browser.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === "cast-reconcile") reconcile();
});

function updateBadgeForTab(tabId) {
  let text = "";
  let color = "#5b6470";
  if (session.session === "media" || session.session === "mirror" || session.session === "youtube") {
    text = "▶";
    color = "#2e9e5b";
  } else {
    const c = castability.get(tabId);
    if (c && c.best && !c.drm) {
      text = "•";
      color = "#4a90d9";
    }
  }
  browser.action.setBadgeText({ text, tabId });
  browser.action.setBadgeBackgroundColor({ color, tabId });
}

// --- messaging hub (popup + content scripts) -------------------------------

browser.runtime.onMessage.addListener((msg, sender) => {
  if (!msg || !msg.type) return;

  if (msg.type === "castability") {
    const tabId = sender.tab && sender.tab.id;
    if (tabId == null) return;
    const fid = sender.frameId || 0;
    let frames = framesByTab.get(tabId);
    if (!frames) {
      frames = new Map();
      framesByTab.set(tabId, frames);
    }
    // Ignore empty reports from sub-frames we've never seen (ad/tracker noise).
    if (fid !== 0 && !msg.best && !frames.has(fid)) return;
    frames.set(fid, { best: msg.best, count: msg.count, drm: msg.drm });
    castability.set(tabId, aggregateFrames(frames));
    updateBadgeForTab(tabId);
    return;
  }

  if (msg.type === "host") {
    // Proxy a daemon action from the popup.
    return call(msg.action, msg.args).then((reply) => {
      if (reply.ok && msg.action === "devices" && reply.data) {
        devices = reply.data.candidates || [];
      }
      if (reply.ok && msg.action === "status" && reply.data) {
        session = reply.data;
        updateActiveBadge();
      }
      // Reflect stop immediately; the daemon also pushes a `session` event, but
      // updating here keeps the badge correct even if no event arrives.
      if (reply.ok && msg.action === "stop") {
        session = { session: "idle", media: null, mirror: null };
        updateActiveBadge();
      }
      return reply;
    });
  }

  if (msg.type === "get-state") {
    return Promise.resolve({ session, devices, castability: castability.get(msg.tabId) || null });
  }
});

// --- context menus ---------------------------------------------------------

function buildMenus() {
  browser.contextMenus.removeAll().then(() => {
    browser.contextMenus.create({
      id: "cast-media",
      title: "Cast this video/audio to TV",
      contexts: ["video", "audio"],
    });
    browser.contextMenus.create({
      id: "mirror-window",
      title: "Mirror this window to TV",
      contexts: ["page"],
    });
  });
}

browser.runtime.onInstalled.addListener(buildMenus);
browser.runtime.onStartup.addListener(buildMenus);

browser.contextMenus.onClicked.addListener(async (info, tab) => {
  const deviceId = await preferredDeviceId();
  try {
    if (info.menuItemId === "cast-media") {
      const tabId = tab && tab.id;
      const c = tabId != null ? castability.get(tabId) : null;
      const best = c && c.best;
      if (best && best.kind === "youtube") {
        const r = await call("youtube-load", {
          deviceId,
          videoId: best.videoId,
          currentTime: best.startTime || 0,
        });
        reportError(r, "Cast failed");
        return;
      }
      let url = null;
      const title = (tab && tab.title) || "";
      if (info.srcUrl && /^https?:/.test(info.srcUrl)) url = info.srcUrl;
      else if (best && best.castUrl) url = best.castUrl;
      if (!url) {
        notify("Can't cast this", "Protected/streamed media — try mirroring the window.");
        return;
      }
      const r = await call("media-load", { deviceId, url, title });
      reportError(r, "Cast failed");
    } else if (info.menuItemId === "mirror-window") {
      const r = await call("mirror-window", { deviceId, selector: "librewolf" });
      reportError(r, "Mirror failed");
    }
  } catch (e) {
    notify("Cast error", String((e && e.message) || e));
  }
});

function reportError(reply, title) {
  if (reply.ok) return;
  const err = reply.error || {};
  if (err.code === "ambiguous") {
    notify("Pick a TV", "Several Chromecasts found — open the Cast popup to choose one.");
  } else if (err.code === "no_devices") {
    notify("No TV found", "No Chromecast on this network.");
  } else if (err.code === "nohost") {
    notify("Cast host missing", "The native castbridge host isn't installed.");
  } else {
    notify(title, err.message || "unknown error");
  }
}

// --- tab tracking ----------------------------------------------------------

browser.tabs.onActivated.addListener(({ tabId }) => updateBadgeForTab(tabId));
browser.tabs.onUpdated.addListener((tabId, changeInfo) => {
  if (changeInfo.status === "loading") {
    castability.delete(tabId);
    framesByTab.delete(tabId);
    updateBadgeForTab(tabId);
  }
});
browser.tabs.onRemoved.addListener((tabId) => {
  castability.delete(tabId);
  framesByTab.delete(tabId);
});

function notify(title, message) {
  browser.notifications.create({
    type: "basic",
    iconUrl: browser.runtime.getURL("icons/cast-96.png"),
    title,
    message: message || "",
  });
}
