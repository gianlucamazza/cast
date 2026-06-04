/* Cast — injects a Cast button into the YouTube player controls.
 *
 * Talks only to the background page (which owns the native port) via
 * {type:"host"} messages. Reuses CastErrors (loaded from common/errors.js in the
 * same content_scripts entry) for localized error text.
 */
(() => {
  const BTN_ID = "opencast-button";
  const POP_ID = "opencast-popover";
  const OV_ID = "opencast-overlay";
  const msg = (k) => browser.i18n.getMessage(k) || k;

  function host(action, args = {}) {
    return browser.runtime.sendMessage({ type: "host", action, args });
  }

  function ctl(cmd, value) {
    return host("media-control", { cmd, value }).catch(() => {});
  }

  let casting = false;
  let currentYt = null;        // last session.youtube payload for THIS tab's video
  let castDeviceName = "";     // friendly device name for the overlay header
  let overlayHidden = false;   // user dismissed the overlay while still casting

  // Scrubber position interpolation (the daemon pushes only on state change).
  let seekDragging = false;
  let posBase = 0;             // seconds at the last reconcile
  let posBaseAt = 0;          // Date.now() at the last reconcile
  let durTotal = 0;
  let ytPlaying = false;
  let ovTicker = null;

  // --- small DOM builder (avoids innerHTML / Trusted Types concerns) --------

  function el(tag, attrs, ...kids) {
    const n = document.createElement(tag);
    for (const k in attrs) {
      if (k === "class") n.className = attrs[k];
      else if (k === "text") n.textContent = attrs[k];
      else n.setAttribute(k, attrs[k]);
    }
    for (const kid of kids) n.appendChild(kid);
    return n;
  }

  function svgCast() {
    const NS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    const p = document.createElementNS(NS, "path");
    p.setAttribute("fill", "#fff");
    p.setAttribute(
      "d",
      "M21 3H3c-1.1 0-2 .9-2 2v3h2V5h18v14h-7v2h7c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zM1 18v3h3c0-1.66-1.34-3-3-3zm0-4v2c2.76 0 5 2.24 5 5h2c0-3.87-3.13-7-7-7zm0-4v2c4.97 0 9 4.03 9 9h2c0-6.08-4.93-11-11-11z"
    );
    svg.appendChild(p);
    return svg;
  }

  // Generic single-path icon (uses currentColor so CSS controls the tint).
  function svgIcon(d) {
    const NS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    const p = document.createElementNS(NS, "path");
    p.setAttribute("fill", "currentColor");
    p.setAttribute("d", d);
    svg.appendChild(p);
    return svg;
  }
  const ICON_PLAY = "M8 5v14l11-7z";
  const ICON_PAUSE = "M6 5h4v14H6zm8 0h4v14h-4z";
  const ICON_VOL =
    "M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02z";

  function fmt(s) {
    s = Math.max(0, Math.floor(s || 0));
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const x = s % 60;
    const mm = h ? String(m).padStart(2, "0") : String(m);
    return (h ? h + ":" : "") + mm + ":" + String(x).padStart(2, "0");
  }

  // --- page data ------------------------------------------------------------

  function videoId() {
    const v = new URLSearchParams(location.search).get("v");
    if (v && /^[A-Za-z0-9_-]{11}$/.test(v)) return v;
    const m = location.pathname.match(/\/(shorts|embed)\/([A-Za-z0-9_-]{11})/);
    return m ? m[2] : null;
  }

  function currentTime() {
    const vid = document.querySelector(".html5-video-player video, video");
    return vid ? Math.floor(vid.currentTime || 0) : 0;
  }

  // --- styles ---------------------------------------------------------------

  function injectStyle() {
    if (document.getElementById("opencast-style")) return;
    const s = document.createElement("style");
    s.id = "opencast-style";
    s.textContent = `
      #${BTN_ID} svg { width: 100%; height: 100%; padding: 12px; box-sizing: border-box; }
      #${BTN_ID}.is-casting svg path { fill: #3ea6ff; }
      #${POP_ID} {
        position: absolute; right: 12px; bottom: 60px;
        min-width: 200px; max-width: 280px;
        background: #1c1f24; color: #e7eaee;
        border: 1px solid #353b44; border-radius: 8px;
        padding: 8px; font: 13px/1.4 system-ui, sans-serif;
        z-index: 2147483647; box-shadow: 0 6px 24px rgba(0,0,0,.5);
      }
      #${POP_ID} .oc-title { font-size: 11px; text-transform: uppercase; color: #9aa3af; margin: 2px 4px 6px; }
      #${POP_ID} .oc-item {
        display: flex; flex-direction: column; gap: 2px;
        padding: 8px 10px; border: 1px solid #353b44; border-radius: 6px;
        margin-bottom: 6px; cursor: pointer;
      }
      #${POP_ID} .oc-item:hover, #${POP_ID} .oc-item:focus-visible { border-color: #4a90d9; outline: none; }
      #${POP_ID} .oc-model { color: #9aa3af; font-size: 11px; }
      #${POP_ID} .oc-msg { color: #9aa3af; padding: 6px 4px; }
      #${OV_ID} {
        position: absolute; inset: 0; display: flex;
        align-items: center; justify-content: center;
        background: rgba(15,17,21,.72); z-index: 2147483647;
        font: 13px/1.4 system-ui, sans-serif; color: #e7eaee;
      }
      #${OV_ID} .oc-ov-panel {
        width: min(440px, 78%); background: #1c1f24;
        border: 1px solid #353b44; border-radius: 12px;
        padding: 16px 18px; box-shadow: 0 10px 40px rgba(0,0,0,.6);
        display: flex; flex-direction: column; gap: 10px; cursor: default;
      }
      #${OV_ID} .oc-ov-head { display: flex; align-items: center; gap: 8px; color: #3ea6ff; }
      #${OV_ID} .oc-ov-head svg { width: 20px; height: 20px; }
      #${OV_ID} .oc-ov-dev { font-weight: 600; color: #e7eaee; }
      #${OV_ID} .oc-ov-title { color: #c4cad2; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
      #${OV_ID} .oc-ov-bar, #${OV_ID} .oc-ov-bar2 { display: flex; align-items: center; gap: 10px; }
      #${OV_ID} .oc-ov-btn {
        background: none; border: none; color: #e7eaee; cursor: pointer;
        padding: 4px; width: 34px; height: 34px; border-radius: 50%;
      }
      #${OV_ID} .oc-ov-btn:hover { background: #2a2f37; }
      #${OV_ID} .oc-ov-btn svg { width: 22px; height: 22px; display: block; margin: auto; }
      #${OV_ID} .oc-ov-btn.is-muted { color: #9aa3af; }
      #${OV_ID} .oc-ov-time { font-variant-numeric: tabular-nums; color: #9aa3af; min-width: 42px; }
      #${OV_ID} .oc-ov-pos { text-align: right; }
      #${OV_ID} .oc-ov-seek { flex: 1; accent-color: #3ea6ff; }
      #${OV_ID} .oc-ov-vol { width: 90px; accent-color: #3ea6ff; }
      #${OV_ID} .oc-ov-stop {
        margin-left: auto; background: #3a1d1d; color: #ff6b6b;
        border: 1px solid #5a2a2a; border-radius: 6px; padding: 6px 12px; cursor: pointer;
      }
      #${OV_ID} .oc-ov-stop:hover { background: #4a2424; }
    `;
    (document.head || document.documentElement).appendChild(s);
  }

  // --- button ---------------------------------------------------------------

  function updateButton(btn) {
    btn = btn || document.getElementById(BTN_ID);
    if (!btn) return;
    btn.classList.toggle("is-casting", casting);
    const label = msg(casting ? "ytStopCasting" : "ytCastToTv");
    btn.title = label;
    btn.setAttribute("aria-label", label);
  }

  function injectButton() {
    const bar = document.querySelector(".ytp-right-controls");
    if (!bar || document.getElementById(BTN_ID)) return;
    injectStyle();
    const btn = el("button", { id: BTN_ID, class: "ytp-button opencast-button" }, svgCast());
    updateButton(btn);
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      e.preventDefault();
      onClick();
    });
    bar.insertBefore(btn, bar.firstChild);
  }

  function onClick() {
    if (casting) {
      // Toggle the controller overlay (Stop lives inside it).
      if (overlayHidden || !document.getElementById(OV_ID)) {
        if (currentYt) showOverlay(currentYt);
      } else {
        overlayHidden = true;
        hideOverlay();
      }
      return;
    }
    togglePopover();
  }

  // --- device picker popover ------------------------------------------------

  function onKey(e) {
    if (e.key === "Escape") {
      e.stopPropagation();
      closePopover();
    }
  }

  function onOutside(e) {
    const p = document.getElementById(POP_ID);
    if (p && !p.contains(e.target) && !e.target.closest(`#${BTN_ID}`)) closePopover();
  }

  function closePopover() {
    const p = document.getElementById(POP_ID);
    if (p) p.remove();
    document.removeEventListener("keydown", onKey, true);
    document.removeEventListener("click", onOutside, true);
  }

  function togglePopover() {
    if (document.getElementById(POP_ID)) {
      closePopover();
      return;
    }
    const player = document.querySelector(".html5-video-player") || document.getElementById("movie_player");
    if (!player) return;
    injectStyle();
    const body = el("div", { class: "oc-body" }, el("div", { class: "oc-msg", text: msg("scanning") }));
    const pop = el("div", { id: POP_ID }, el("div", { class: "oc-title", text: msg("ytPickTitle") }), body);
    pop.addEventListener("click", (e) => e.stopPropagation());
    player.appendChild(pop);
    document.addEventListener("keydown", onKey, true);
    // Defer outside-click binding so the opening click doesn't immediately close.
    setTimeout(() => document.addEventListener("click", onOutside, true), 0);
    loadDevices(body);
  }

  async function loadDevices(body) {
    const r = await host("devices").catch(() => null);
    if (!document.body.contains(body)) return; // popover closed meanwhile
    body.replaceChildren();
    if (!r || !r.ok) {
      const e = (r && r.error) || {};
      body.appendChild(el("div", { class: "oc-msg", text: CastErrors.errorMessage(e.code, e.message) }));
      return;
    }
    const devices = (r.data && r.data.candidates) || [];
    if (!devices.length) {
      body.appendChild(el("div", { class: "oc-msg", text: msg("noDevicesFound") }));
      return;
    }
    for (const d of devices) {
      const item = el("div", { class: "oc-item", role: "button", tabindex: "0" }, el("span", { text: d.name }));
      if (d.model) item.appendChild(el("span", { class: "oc-model", text: d.model }));
      const go = () => castTo(d.id, d.name);
      item.addEventListener("click", go);
      item.addEventListener("keydown", (e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          go();
        }
      });
      body.appendChild(item);
    }
  }

  // Pause the local <video> so audio doesn't play in the tab and on the TV at
  // once (Chrome does the same on cast). pause() is a plain HTMLMediaElement
  // method, reachable from the content script's isolated world.
  function pauseLocalVideo() {
    const v = document.querySelector(".html5-video-player video, video");
    if (v && !v.paused) {
      try { v.pause(); } catch (_) { /* ignore */ }
    }
  }

  async function castTo(deviceId, deviceName) {
    const vid = videoId();
    if (!vid) {
      closePopover();
      return;
    }
    castDeviceName = deviceName || "";
    browser.storage.local.set({ deviceId, deviceName: castDeviceName });
    const r = await host("youtube-load", { deviceId, videoId: vid, currentTime: currentTime() }).catch(() => null);
    if (r && r.ok) {
      casting = true;
      updateButton();
      pauseLocalVideo();
      closePopover();
      return;
    }
    const body = document.querySelector(`#${POP_ID} .oc-body`);
    if (body) {
      const e = (r && r.error) || {};
      body.replaceChildren(el("div", { class: "oc-msg", text: CastErrors.errorMessage(e.code, e.message) }));
    }
  }

  // --- in-page controller overlay ("Playing on <TV>", Chrome-like) ----------

  function paintProgress() {
    const ov = document.getElementById(OV_ID);
    if (!ov || ov.style.display === "none") return;
    let p = posBase;
    if (ytPlaying && !seekDragging) p = posBase + (Date.now() - posBaseAt) / 1000;
    if (durTotal > 0) p = Math.min(p, durTotal);
    const seek = ov.querySelector(".oc-ov-seek");
    if (!seekDragging) {
      seek.max = String(Math.max(1, Math.floor(durTotal)));
      seek.value = String(Math.floor(p));
    }
    ov.querySelector(".oc-ov-pos").textContent = fmt(p);
    ov.querySelector(".oc-ov-dur").textContent = fmt(durTotal);
  }

  function ensureOverlay() {
    let ov = document.getElementById(OV_ID);
    if (ov) return ov;
    const player = document.querySelector(".html5-video-player") || document.getElementById("movie_player");
    if (!player) return null;
    injectStyle();

    const dev = el("span", { class: "oc-ov-dev" });
    const head = el("div", { class: "oc-ov-head" }, svgCast(), dev);
    const title = el("div", { class: "oc-ov-title" });

    const pp = el("button", { class: "oc-ov-btn oc-ov-pp", "aria-label": msg("pause") }, svgIcon(ICON_PAUSE));
    pp.addEventListener("click", (e) => {
      e.stopPropagation();
      ctl(ytPlaying ? "pause" : "play");  // session push reconciles the icon
    });

    const pos = el("span", { class: "oc-ov-time oc-ov-pos", text: "0:00" });
    const dur = el("span", { class: "oc-ov-time oc-ov-dur", text: "0:00" });
    const seek = el("input", {
      type: "range", class: "oc-ov-seek", min: "0", max: "100", value: "0",
      "aria-label": msg("seek"),
    });
    seek.addEventListener("pointerdown", () => { seekDragging = true; });
    seek.addEventListener("input", () => { pos.textContent = fmt(Number(seek.value)); });
    seek.addEventListener("change", () => {
      const v = Number(seek.value);
      seekDragging = false;
      posBase = v;
      posBaseAt = Date.now();
      ctl("seek", v);
    });

    const mute = el("button", { class: "oc-ov-btn oc-ov-mute", "aria-label": msg("mute") }, svgIcon(ICON_VOL));
    let muted = false;
    mute.addEventListener("click", (e) => {
      e.stopPropagation();
      muted = !muted;
      mute.setAttribute("aria-label", msg(muted ? "unmute" : "mute"));
      mute.classList.toggle("is-muted", muted);
      ctl("mute", muted ? 1 : 0);
    });
    const vol = el("input", {
      type: "range", class: "oc-ov-vol", min: "0", max: "100", value: "100",
      "aria-label": msg("volume"),
    });
    vol.addEventListener("input", () => ctl("volume", Number(vol.value) / 100));

    const stop = el("button", { class: "oc-ov-stop", text: msg("ytStopCasting") });
    stop.addEventListener("click", (e) => {
      e.stopPropagation();
      host("stop").catch(() => {});
    });

    const bar = el("div", { class: "oc-ov-bar" }, pp, pos, seek, dur);
    const bar2 = el("div", { class: "oc-ov-bar2" }, mute, vol, stop);
    const panel = el("div", { class: "oc-ov-panel" }, head, title, bar, bar2);
    panel.addEventListener("click", (e) => e.stopPropagation());
    ov = el("div", { id: OV_ID }, panel);
    // A click on the dim backdrop (not the panel) hides the overlay; the
    // in-player Cast button brings it back. Stop is only via the panel button.
    ov.addEventListener("click", () => { overlayHidden = true; hideOverlay(); });
    player.appendChild(ov);
    return ov;
  }

  function renderOverlay(y) {
    const ov = ensureOverlay();
    if (!ov || !y) return;
    const name = castDeviceName || msg("ytThisTv");
    ov.querySelector(".oc-ov-dev").textContent =
      browser.i18n.getMessage("ytCastingTo", [name]) || ("▶ " + name);
    ov.querySelector(".oc-ov-title").textContent = y.title || msg("youtube");
    ytPlaying = y.state === "PLAYING";
    const pp = ov.querySelector(".oc-ov-pp");
    pp.replaceChildren(svgIcon(ytPlaying ? ICON_PAUSE : ICON_PLAY));
    pp.setAttribute("aria-label", msg(ytPlaying ? "pause" : "play"));
    durTotal = y.duration || 0;
    if (!seekDragging) {
      posBase = y.position || 0;
      posBaseAt = Date.now();
    }
    paintProgress();
  }

  function showOverlay(y) {
    overlayHidden = false;
    const ov = ensureOverlay();
    if (!ov) return;
    ov.style.display = "flex";
    renderOverlay(y);
    if (!ovTicker) ovTicker = setInterval(paintProgress, 1000);
  }

  function hideOverlay() {
    if (ovTicker) { clearInterval(ovTicker); ovTicker = null; }
    const ov = document.getElementById(OV_ID);
    if (ov) ov.style.display = "none";
  }

  function removeOverlay() {
    overlayHidden = false;
    if (ovTicker) { clearInterval(ovTicker); ovTicker = null; }
    const ov = document.getElementById(OV_ID);
    if (ov) ov.remove();
  }

  // Re-evaluate overlay presence after SPA navigation / player re-render.
  function syncOverlay() {
    if (casting && currentYt && currentYt.videoId === videoId()) {
      if (!overlayHidden && !document.getElementById(OV_ID)) showOverlay(currentYt);
    } else {
      removeOverlay();
    }
  }

  // --- session state --------------------------------------------------------

  function applySession(s) {
    const wasCasting = casting;
    casting = !!s && s.session === "youtube";
    updateButton();
    const mine = casting && s && s.youtube && s.youtube.videoId === videoId();
    if (mine) {
      currentYt = s.youtube;
      if (!wasCasting) {
        // A cast of THIS tab's video just started (possibly from the popup):
        // pause the local player and resolve the device name for the header.
        pauseLocalVideo();
        if (!castDeviceName) {
          browser.storage.local.get("deviceName").then(({ deviceName }) => {
            castDeviceName = deviceName || "";
            if (!overlayHidden) renderOverlay(currentYt);
          }).catch(() => {});
        }
      }
      if (!overlayHidden) showOverlay(s.youtube);
    } else {
      // Not casting, or a different tab's video — no controller here.
      currentYt = casting ? currentYt : null;
      removeOverlay();
    }
  }

  browser.runtime.onMessage.addListener((m) => {
    if (m && m.type === "cast-event") {
      const ev = m.event;
      if (ev.type === "session") applySession(ev.data);
      else if (ev.type === "session-ended") {
        casting = false;
        currentYt = null;
        updateButton();
        removeOverlay();
      }
    }
  });

  host("status").then((r) => {
    if (r && r.ok) applySession(r.data);
  }).catch(() => {});

  // --- lifecycle (SPA navigation + player re-render) ------------------------

  injectButton();
  const onNavigate = () => { injectButton(); syncOverlay(); };
  window.addEventListener("yt-navigate-finish", onNavigate, true);
  window.addEventListener("yt-page-data-updated", onNavigate, true);
  const mo = new MutationObserver(() => {
    if (!document.getElementById(BTN_ID)) injectButton();
    if (casting) syncOverlay();
  });
  mo.observe(document.documentElement, { childList: true, subtree: true });
})();
