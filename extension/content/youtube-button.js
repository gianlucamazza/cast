/* Cast — injects a Cast button into the YouTube player controls.
 *
 * Talks only to the background page (which owns the native port) via
 * {type:"host"} messages. Reuses CastErrors (loaded from common/errors.js in the
 * same content_scripts entry) for localized error text.
 */
(() => {
  const BTN_ID = "opencast-button";
  const POP_ID = "opencast-popover";
  const msg = (k) => browser.i18n.getMessage(k) || k;

  function host(action, args = {}) {
    return browser.runtime.sendMessage({ type: "host", action, args });
  }

  let casting = false;

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
      host("stop").catch(() => {});
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
      const go = () => castTo(d.id);
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

  async function castTo(deviceId) {
    const vid = videoId();
    if (!vid) {
      closePopover();
      return;
    }
    browser.storage.local.set({ deviceId });
    const r = await host("youtube-load", { deviceId, videoId: vid, currentTime: currentTime() }).catch(() => null);
    if (r && r.ok) {
      casting = true;
      updateButton();
      closePopover();
      return;
    }
    const body = document.querySelector(`#${POP_ID} .oc-body`);
    if (body) {
      const e = (r && r.error) || {};
      body.replaceChildren(el("div", { class: "oc-msg", text: CastErrors.errorMessage(e.code, e.message) }));
    }
  }

  // --- session state --------------------------------------------------------

  function applySession(s) {
    casting = !!s && s.session === "youtube";
    updateButton();
  }

  browser.runtime.onMessage.addListener((m) => {
    if (m && m.type === "cast-event") {
      const ev = m.event;
      if (ev.type === "session") applySession(ev.data);
      else if (ev.type === "session-ended") {
        casting = false;
        updateButton();
      }
    }
  });

  host("status").then((r) => {
    if (r && r.ok) applySession(r.data);
  }).catch(() => {});

  // --- lifecycle (SPA navigation + player re-render) ------------------------

  injectButton();
  window.addEventListener("yt-navigate-finish", injectButton, true);
  window.addEventListener("yt-page-data-updated", injectButton, true);
  const mo = new MutationObserver(() => {
    if (!document.getElementById(BTN_ID)) injectButton();
  });
  mo.observe(document.documentElement, { childList: true, subtree: true });
})();
