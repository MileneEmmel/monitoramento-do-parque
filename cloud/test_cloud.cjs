const fs = require('node:fs'),
  vm = require('node:vm'),
  assert = require('node:assert/strict');
const html = fs.readFileSync(__dirname + '/public/index.html', 'utf8');
const script = fs.readFileSync(__dirname + '/public/cloud.js', 'utf8');
const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]));
for (const m of script.matchAll(/\$\('([^']+)'\)/g)) assert.ok(ids.has(m[1]), 'Missing ' + m[1]);
assert.doesNotMatch(html, /data-page="camera"|id="camera"|open-local|local-url/);
assert.doesNotMatch(html, /park-select|park-form|Criar parque/);
assert.match(script, /encodeURIComponent\(cfg.deviceId\)/);
assert.doesNotMatch(script, /rpc\('park_create'/);
assert.doesNotMatch(script, /service_role|sb_secret|localStorage/);
assert.match(script, /p_include_key:\s*false/);
assert.match(script, /p_include_key:\s*true/);
assert.doesNotMatch(html, /<(?:video|canvas|iframe)\b/);
class Element {
  constructor() {
    this.children = [];
    this.listeners = {};
    this.attributes = {};
    this.style = {};
    this.classList = { toggle() {}, remove() {}, add() {} };
    this.parentElement = { append() {} };
  }
  addEventListener(k, f) {
    this.listeners[k] = f;
  }
  setAttribute(k, v) {
    this.attributes[k] = v;
  }
  getAttribute(k) {
    return this.attributes[k];
  }
  removeAttribute(k) {
    delete this.attributes[k];
  }
  replaceChildren(...v) {
    this.children = v;
  }
  append(...v) {
    this.children.push(...v);
  }
  querySelectorAll() {
    return [];
  }
  focus() {}
}
const elements = new Map();
const get = (id) => {
  if (!elements.has(id)) elements.set(id, new Element());
  return elements.get(id);
};
const context = vm.createContext({
  document: {
    getElementById: get,
    querySelectorAll() {
      return [];
    },
    querySelector() {
      return null;
    },
    createElement() {
      return new Element();
    },
    addEventListener() {},
    body: new Element(),
    hidden: false,
  },
  window: { PARK_CLOUD: {}, addEventListener() {} },
  location: { hash: '#overview' },
  matchMedia() {
    return { matches: false };
  },
  sessionStorage: {
    getItem() {
      return null;
    },
  },
  setInterval() {},
  setTimeout() {},
  URL,
  Blob,
  Intl,
  Date,
  AbortSignal,
  console,
});
vm.runInContext(fs.readFileSync(__dirname + '/public/date-range.js', 'utf8'), context);
vm.runInContext(script, context);
const run = (code) => vm.runInContext(code, context);
assert.equal(run("number('18446744073709551615')"), '18.446.744.073.709.551.615');
assert.equal(run("escapeHTML('<img onerror=x>')"), '&lt;img onerror=x&gt;');
assert.match(run('today()'), /^\d{4}-\d{2}-\d{2}$/);
run('setDates(7)');
assert.equal(run('(new Date(end)-new Date(start))/86400000'), 6);
get('custom-period').listeners.click();
assert.equal(get('custom-period').attributes['aria-pressed'], 'true');
assert.equal(get('date-form').hidden, false);
get('custom-period').listeners.click();
assert.equal(get('date-form').hidden, false, 'Clicking the active period must not toggle it off');
run("datePicker.choose('2026-01-15'); datePicker.choose('2026-01-10')");
assert.equal(get('date-start').value, '2026-01-10');
assert.equal(get('date-end').value, '2026-01-15');
assert.equal(get('range-apply').disabled, false);
run("datePicker.choose('2024-01-01'); datePicker.choose('2026-01-01')");
assert.equal(get('range-apply').disabled, true);
run("datePicker.choose('2999-01-01')");
assert.equal(get('date-start').value, '2024-01-01');
run('setDates(30)');
assert.equal(get('custom-period').attributes['aria-pressed'], 'false');
assert.equal(get('custom-period').attributes['aria-expanded'], 'false');
assert.equal(get('date-form').hidden, true);
run(
  'drawChart({has_samples:true,entries:"9007199254740993",exits:"2",chart:[{label:"10h",entries:"9007199254740993",exits:"2",samples:1}]})',
);
assert.match(get('chart').innerHTML, /<svg/);
assert.match(get('chart').innerHTML, /grid-line/);
assert.match(get('chart').attributes['aria-label'], /9\.007\.199\.254\.740\.993/);
assert.equal(get('chart-detail').hidden, true);
run('showChartBucket(0)');
assert.equal(get('chart-detail').hidden, false);
run('hideChartBucket()');
assert.equal(get('chart-detail').hidden, true);
run('report=null;render()');
assert.equal(get('entries').textContent, '—');
assert.equal(get('full-history').children.length, 0);
run('setMenu(false)');
assert.equal(get('toggle-menu').attributes['aria-label'], 'Expandir menu');
run('park="test";isOwner=false;cameraAccess=null;renderCameraAccess()');
assert.equal(get('camera-access-section').hidden, true);
run('isOwner=true;renderCameraAccess()');
assert.equal(get('open-camera-link').disabled, true);
run(
  'cameraAccess={available:true,fresh:true,address:"192.168.0.230",updated_at:new Date().toISOString()};renderCameraAccess()',
);
assert.equal(get('open-camera-link').disabled, false);
run('cameraAccess.fresh=false;renderCameraAccess()');
assert.equal(get('open-camera-link').disabled, true);
assert.equal(
  run('validateCameraURL("http://192.168.0.230/#"+"a".repeat(32))'),
  'http://192.168.0.230/#' + 'a'.repeat(32),
);
for (const address of ['127.0.0.1', '8.8.8.8', '172.15.0.1', 'evil.example'])
  assert.throws(() => run('validateCameraURL("http://' + address + '/#"+"a".repeat(32))'));
for (const address of ['10.0.0.2', '172.16.0.2', '172.31.255.254'])
  assert.ok(run('validateCameraURL("http://' + address + '/#"+"a".repeat(32))'));
for (const value of [
  'https://192.168.0.1/#' + 'a'.repeat(32),
  'http://192.168.0.1:1234/#' + 'a'.repeat(32),
  'http://192.168.0.1/?key=leak#' + 'a'.repeat(32),
  'http://192.168.0.1/#invalid',
  'javascript:alert(1)',
])
  assert.throws(() => run('validateCameraURL(' + JSON.stringify(value) + ')'));
(async () => {
  run(
    'var openedURL="",copiedURL="",popupClosed=false;window.open=()=>({opener:null,location:{replace(value){openedURL=value;}},close(){popupClosed=true;}});var navigator={clipboard:{async writeText(value){copiedURL=value;}}};session={expires_at:Date.now()/1000+3600};isOwner=true;cameraAccess.fresh=true;request=async()=>({fresh:true,url:"http://192.168.0.230/#"+"a".repeat(32)});',
  );
  await get('open-camera-link').listeners.click();
  assert.equal(run('openedURL'), 'http://192.168.0.230/#' + 'a'.repeat(32));
  await get('copy-camera-link').listeners.click();
  assert.equal(run('copiedURL'), run('openedURL'));
  assert.equal(run('cameraPending'), false);
  run('openedURL="";popupClosed=false;request=async()=>({fresh:false});');
  await get('open-camera-link').listeners.click();
  assert.equal(run('openedURL'), '');
  assert.equal(run('popupClosed'), true);
  run(
    'openedURL="";popupClosed=false;request=async()=>{generation++;return {fresh:true,url:"http://192.168.0.230/#"+"a".repeat(32)};};',
  );
  await get('open-camera-link').listeners.click();
  assert.equal(run('openedURL'), '');
  assert.equal(run('popupClosed'), true);
  run('isOwner=false;openedURL="";');
  await get('open-camera-link').listeners.click();
  assert.equal(run('openedURL'), '');
  console.log(
    'Cloud: DOM, camera separation, exact integers, chart, sidebar, owner-only discovery, stale/invalid links, open/copy and session races: PASS',
  );
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
