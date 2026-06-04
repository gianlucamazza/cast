// Shared daemon error-code → localized message map.
// Loaded as a plain script in both the popup and the background page, so it
// exposes itself on the global object rather than using ES modules.
(function (root) {
  // Codes the daemon / relay can return (see native IPC protocol). Each maps to
  // an `err_<code>` key in _locales. Anything else falls back to the raw daemon
  // message, then to a generic string.
  const KNOWN = new Set([
    "ambiguous",
    "no_devices",
    "no_window",
    "no_wm",
    "nohost",
    "timeout",
    "disconnected",
  ]);

  function errorMessage(code, fallback) {
    if (code && KNOWN.has(code)) return browser.i18n.getMessage("err_" + code);
    return fallback || browser.i18n.getMessage("err_unknown");
  }

  root.CastErrors = { errorMessage };
})(typeof self !== "undefined" ? self : this);
