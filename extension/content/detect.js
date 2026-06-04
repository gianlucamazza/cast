/* Cast — media detection content script.
 *
 * Classifies the page's castability and reports the best candidate to the
 * background page. The daemon's media path (CC1AD845 LOAD) plays direct media
 * URLs; DRM/MSE (blob: sources) cannot be cast and fall back to mirroring.
 */

(() => {
  const DRM_HOSTS = [
    /(^|\.)netflix\.com$/,
    /(^|\.)disneyplus\.com$/,
    /(^|\.)primevideo\.com$/,
    /(^|\.)hbomax\.com$/,
    /(^|\.)max\.com$/,
    /(^|\.)spotify\.com$/,
  ];
  // Extensions the default media receiver can play from a direct URL, including
  // adaptive manifests (HLS .m3u8 / DASH .mpd) when exposed as a plain link.
  const MEDIA_EXT = /\.(mp4|m4v|webm|ogg|ogv|mp3|m4a|aac|flac|wav|mov|m3u8|mpd)(\?|#|$)/i;

  function matches(list) {
    return list.some((re) => re.test(location.hostname));
  }

  function effectiveSrc(el) {
    const s = el.currentSrc || el.src || "";
    if (s) return s;
    const source = el.querySelector("source[src]");
    return source ? source.src : "";
  }

  function area(el) {
    const r = el.getBoundingClientRect();
    return r.width * r.height;
  }

  function youtubeVideoId() {
    const h = location.hostname;
    if (/(^|\.)youtu\.be$/.test(h)) {
      const id = location.pathname.slice(1).split(/[/?#]/)[0];
      return /^[A-Za-z0-9_-]{11}$/.test(id) ? id : null;
    }
    if (/(^|\.)youtube\.com$/.test(h)) {
      const v = new URLSearchParams(location.search).get("v");
      if (v && /^[A-Za-z0-9_-]{11}$/.test(v)) return v;
      const m = location.pathname.match(/\/(shorts|embed)\/([A-Za-z0-9_-]{11})/);
      if (m) return m[2];
    }
    return null;
  }

  // Parse a YouTube time param: "90", "90s", or "1h2m3s" -> seconds.
  function parseTime(t) {
    t = String(t).trim();
    if (!t) return 0;
    if (/^\d+s?$/.test(t)) return parseInt(t, 10);
    const h = t.match(/(\d+)h/);
    const m = t.match(/(\d+)m/);
    const s = t.match(/(\d+)s/);
    if (h || m || s) {
      return (h ? +h[1] * 3600 : 0) + (m ? +m[1] * 60 : 0) + (s ? +s[1] : 0);
    }
    const n = t.match(/\d+/);
    return n ? parseInt(n[0], 10) : 0;
  }

  function youtubeStart() {
    const sp = new URLSearchParams(location.search);
    return parseTime(sp.get("t") || sp.get("start") || "");
  }

  function classify() {
    const drm = matches(DRM_HOSTS);

    // 1) YouTube: cast natively to the TV's YouTube app (videoId via Lounge).
    const ytId = youtubeVideoId();
    if (ytId) {
      return {
        best: { kind: "youtube", videoId: ytId, startTime: youtubeStart(), title: document.title },
        count: 1,
        drm: false,
      };
    }

    // 2) Direct media elements with a fetchable http(s) source.
    const els = [...document.querySelectorAll("video, audio")];
    let drmSeen = drm;
    const candidates = [];
    for (const el of els) {
      const src = effectiveSrc(el);
      if (!src) continue;
      if (src.startsWith("blob:") || src.startsWith("mediasource:")) {
        drmSeen = true; // MSE/EME — not castable via a plain URL
        continue;
      }
      if (!/^https?:/.test(src)) continue;
      if (el.tagName === "VIDEO" && !MEDIA_EXT.test(src)) {
        // Allow extension-less only if clearly a media element with metadata.
        if (!el.duration) continue;
      }
      candidates.push({ el, src });
    }
    if (candidates.length) {
      candidates.sort((a, b) => {
        const pa = a.el.paused ? 0 : 1;
        const pb = b.el.paused ? 0 : 1;
        if (pa !== pb) return pb - pa;
        return area(b.el) - area(a.el);
      });
      const top = candidates[0];
      return {
        best: { kind: "media", castUrl: top.src, title: document.title },
        count: candidates.length,
        drm: drmSeen,
      };
    }

    // 3) The page itself is a bare media file.
    if (MEDIA_EXT.test(location.pathname)) {
      return { best: { kind: "media", castUrl: location.href, title: document.title }, count: 1, drm };
    }

    return { best: null, count: 0, drm: drmSeen };
  }

  let lastJson = "";
  function report() {
    const result = classify();
    const json = JSON.stringify(result);
    if (json === lastJson) return;
    lastJson = json;
    browser.runtime
      .sendMessage({ type: "castability", best: result.best, count: result.count, drm: result.drm })
      .catch(() => {});
  }

  let timer = null;
  function scheduleReport() {
    clearTimeout(timer);
    timer = setTimeout(report, 500);
  }

  report();
  const mo = new MutationObserver(scheduleReport);
  mo.observe(document.documentElement, { childList: true, subtree: true });
  document.addEventListener("play", scheduleReport, true);
  document.addEventListener("loadedmetadata", scheduleReport, true);
  // YouTube navigates client-side (related video, shorts↔watch) without a reload;
  // its own SPA events are the reliable signal to re-classify, instead of relying
  // on a DOM mutation happening to fire. The lastJson dedup drops no-op reports.
  window.addEventListener("yt-navigate-finish", scheduleReport, true);
  window.addEventListener("yt-page-data-updated", scheduleReport, true);
})();
