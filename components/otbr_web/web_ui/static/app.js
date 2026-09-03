/*
 * Shared helpers for the thredge border router UI.
 * SPDX-License-Identifier: Apache-2.0
 */

/* --- tiny helpers ------------------------------------------------------ */

function $(id) { return document.getElementById(id); }

function el(tag, cls, text) {
  var n = document.createElement(tag);
  if (cls) { n.className = cls; }
  if (text !== undefined) { n.textContent = text; }
  return n;
}

/* Always sets textContent, never innerHTML: values here come from the network
 * and from nearby access points, i.e. from outside this device. */
function setText(id, value, fallback) {
  var n = $(id);
  if (n) { n.textContent = (value === undefined || value === null || value === '') ? (fallback || '--') : value; }
}

function getJSON(url) {
  return fetch(url, {headers : {'Accept' : 'application/json'}})
      .then(function(r) { return r.json().then(function(body) { return checkBody(r, body); }); });
}

/* Two response shapes are in play. This project's own endpoints answer
 * {"error": "text"} on failure, while the border router's REST API answers
 * {"error": <otError number>, "result": ..., "message": "text"} where 0 means
 * success -- so a numeric error must be compared against zero, not merely
 * tested for truthiness, and its "message" carries the readable reason. */
function checkBody(r, body) {
  if (body && typeof body.error === 'number') {
    if (body.error !== 0) {
      throw new Error(body.message || ('error ' + body.error));
    }
    return body;
  }
  if (!r.ok || (body && body.error)) {
    throw new Error((body && (body.error || body.message)) || ('HTTP ' + r.status));
  }
  return body;
}

function postJSON(url, payload) {
  return fetch(url, {
           method : 'POST',
           headers : {'Content-Type' : 'application/json'},
           body : JSON.stringify(payload)
         })
      .then(function(r) { return r.json().then(function(body) { return checkBody(r, body); }); });
}

/* Some border router endpoints answer 200 with an empty body (PUT
 * /node/state), so the response must not be parsed as JSON unconditionally. */
function putRaw(url, body, contentType) {
  return fetch(url, {method : 'PUT', headers : {'Content-Type' : contentType}, body : body})
      .then(function(r) {
        if (!r.ok) {
          return r.text().then(function(t) { throw new Error(t || ('HTTP ' + r.status)); });
        }
        return r.text();
      });
}

function message(id, text, kind) {
  var n = $(id);
  if (!n) { return; }
  n.className = 'msg msg-' + (kind || 'ok');
  n.textContent = text;
  n.classList.remove('hidden');
}

function clearMessage(id) {
  var n = $(id);
  if (n) { n.classList.add('hidden'); }
}

/* Signal strength in words, so the reading means something without knowing
 * what dBm are. */
function signalWords(rssi) {
  if (rssi >= -55) { return 'excellent'; }
  if (rssi >= -67) { return 'good'; }
  if (rssi >= -75) { return 'fair'; }
  return 'weak';
}

/* --- footer ------------------------------------------------------------ */

(function() {
  var f = document.getElementById('footer');
  if (!f) { return; }
  fetch('/api/ota/version', {headers : {'Accept' : 'application/json'}})
      .then(function(r) { return r.json(); })
      .then(function(v) { f.textContent = 'thredge \u00b7 v' + v.version + ' \u00b7 ' + v.project; })
      .catch(function() { f.textContent = 'thredge'; });
})();

/* --- navigation -------------------------------------------------------- */

(function() {
  var pages = [
    {href : '/index.html', label : 'Status'},
    {href : '/wifi.html', label : 'Wi-Fi'},
    {href : '/thread.html', label : 'Thread'},
    {href : '/map.html', label : 'Map'},
    {href : '/update.html', label : 'Update'}
  ];

  var nav = document.getElementById('nav');
  if (!nav) { return; }

  var path = location.pathname;
  if (path === '/' || path === '') { path = '/index.html'; }

  var inner = el('div', 'nav-inner');
  var brand = el('a', 'nav-brand');
  brand.href = '/index.html';
  brand.appendChild(el('span', null, 'thredge'));
  brand.appendChild(el('small', null, 'Thread Border Router'));
  inner.appendChild(brand);

  var links = el('div', 'nav-links');
  pages.forEach(function(p) {
    var a = el('a', p.href === path ? 'active' : null, p.label);
    a.href = p.href;
    links.appendChild(a);
  });
  inner.appendChild(links);
  nav.appendChild(inner);
})();
