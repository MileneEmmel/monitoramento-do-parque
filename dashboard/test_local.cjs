const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.join(__dirname, 'local-public');
const html = fs.readFileSync(path.join(root, 'index.html'), 'utf8');
const script = fs.readFileSync(path.join(root, 'app.js'), 'utf8');
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
assert.equal(ids.length, new Set(ids).size, 'Duplicate HTML IDs');
for (const match of script.matchAll(/\$\('([^']+)'\)/g))
  assert(ids.includes(match[1]), 'Missing element: ' + match[1]);
assert(!script.includes('/api/state'), 'Preview status must not scan full history');
assert(!html.includes('page-history') && !html.includes('reset-counter'));
const calls = [],
  timers = [],
  draws = [];
const ctx = new Proxy(
  { measureText: (t) => ({ width: t.length * 7 }) },
  { get: (target, key) => target[key] || ((...args) => draws.push([key, ...args])) },
);
class Element {
  constructor() {
    this.value = '';
    this.checked = false;
    this.attributes = {};
    this.listeners = {};
    this.classList = { toggle() {} };
  }
  addEventListener(name, fn) {
    this.listeners[name] = fn;
  }
  setAttribute(k, v) {
    this.attributes[k] = v;
  }
  getAttribute(k) {
    return this.attributes[k];
  }
  getContext() {
    return ctx;
  }
}
const elements = new Map(ids.map((id) => [id, new Element()]));
const get = (id) => {
  assert(elements.has(id), 'Unexpected element: ' + id);
  return elements.get(id);
};
const document = {
  getElementById: get,
  body: new Element(),
  hidden: false,
  listeners: {},
  addEventListener(name, fn) {
    this.listeners[name] = fn;
  },
};
const status = {
  today: '2026-10-11',
  device: {
    configured: true,
    name: 'Parque',
    access: 'Entrada',
    address: '192.168.0.2',
    counter: { ready: true, first: 0.4, second: 0.6, horizontal: false, reverse: false, frames: 1 },
  },
  network: { wifi: { connected: true, ssid: 'fixture', rssi: -40, busy: false } },
  storage: { ok: true },
  cloud: { configured: true, phase: 'ready' },
};
const frame = {
  width: 320,
  height: 240,
  jpeg: 'fixture',
  people: 1,
  counter: { first: 0.4, second: 0.6, horizontal: false },
  boxes: [[10, 20, 80, 200, 0.9, 1, 1]],
};
let decode = async () => {};
const context = vm.createContext({
  document,
  window: { history: { replaceState() {} } },
  location: { hash: '#camera' },
  sessionStorage: {
    getItem() {
      return null;
    },
  },
  matchMedia: () => ({ matches: false }),
  AbortSignal,
  TextEncoder,
  Date,
  Image: class {
    decode() {
      return decode();
    }
  },
  console,
  setTimeout(fn, delay) {
    timers.push({ fn, delay });
    return timers.length;
  },
  clearTimeout() {},
  setInterval() {},
});
// Keep responses explicit so tests never access a real ESP or external service.
context.fetch = async (url, options) => {
  calls.push({ url, options });
  return {
    ok: true,
    json: async () =>
      url === '/api/local'
        ? structuredClone(status)
        : url === '/api/frame'
          ? { frame, age: 0 }
          : {},
  };
};
vm.runInContext(script, context);
const run = (code) => vm.runInContext(code, context);
(async () => {
  run('accessToken="a".repeat(32)');
  await run('refresh()');
  assert.equal(calls[0].url, '/api/local');
  assert.equal(calls[0].options.headers.Authorization, 'Bearer ' + 'a'.repeat(32));
  assert.equal(get('line-a').value, 40);
  assert.equal(get('wifi-network').textContent, 'fixture');
  get('show-boxes').checked = true;
  await run('pollCamera()');
  assert(draws.some((v) => v[0] === 'drawImage'));
  assert(draws.some((v) => v[0] === 'strokeRect'));
  assert.equal(get('people-now').textContent, 'Pessoas no quadro: 1');
  get('pause-camera').listeners.click();
  const requestCount = calls.length;
  await run('pollCamera()');
  assert.equal(calls.length, requestCount, 'Paused preview must not request frames');
  status.device.counter.frames = 2;
  await run('refresh()');
  assert.equal(run('state.device.counter.frames'), 2, 'Status can advance while preview is paused');
  assert.match(get('camera-status').textContent, /contagem continua/);
  get('pause-camera').listeners.click();
  document.hidden = true;
  const hiddenCount = calls.length;
  await run('pollCamera()');
  await run('refresh()');
  assert.equal(calls.length, hiddenCount);
  document.hidden = false;
  const before = draws.length;
  let finishDecode;
  decode = () =>
    new Promise((resolve) => {
      finishDecode = resolve;
    });
  const pending = run('pollCamera()');
  await new Promise((resolve) => setImmediate(resolve));
  get('pause-camera').listeners.click();
  get('pause-camera').listeners.click();
  finishDecode();
  await pending;
  assert.equal(draws.length, before, 'Do not draw a stale frame after pause/resume');
  decode = async () => {};
  get('axis').value = 'y';
  get('line-a').value = 30;
  get('line-b').value = 70;
  await get('calibration-form').listeners.submit({ preventDefault() {} });
  const calibration = calls.find((v) => v.url === '/api/calibration');
  assert.deepEqual(JSON.parse(calibration.options.body), {
    first: 0.3,
    second: 0.7,
    horizontal: true,
    reverse: false,
  });
  assert(!calls.some((v) => v.url === '/api/reset'));
  context.fetch = async () => ({ ok: false, json: async () => ({ error: 'fixture unavailable' }) });
  await run('refresh()');
  assert.equal(get('request-error').hidden, false);
  assert.match(get('request-error').textContent, /fixture unavailable/);
  console.log(
    'PASS: DOM, auth, lightweight status, frame rendering, pause, hidden tab, stale-frame race, calibration and errors.',
  );
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
