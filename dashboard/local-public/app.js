'use strict';
const $ = (id) => document.getElementById(id);
let accessToken = '';
const accessMatch = location.hash.match(/^#(?:access=)?([0-9a-f]{32})$/i);
try {
  accessToken = accessMatch?.[1] || sessionStorage.getItem('park-access') || '';
} catch {}
if (!/^[0-9a-f]{32}$/i.test(accessToken)) accessToken = '';
if (accessMatch) {
  try {
    sessionStorage.setItem('park-access', accessToken);
  } catch {}
}
window.history.replaceState(null, '', '#camera');
const apiFetch = (path, options = {}) =>
  fetch(path, {
    ...options,
    headers: { ...options.headers, Authorization: 'Bearer ' + accessToken },
    signal: options.signal || AbortSignal.timeout(12000),
  });
let state = null,
  fetching = false,
  calibrationDirty = false,
  wifiSubmitting = false;
let cameraPaused = false,
  cameraTimer = null,
  cameraDrawing = false,
  previewGeneration = 0,
  lastFrame = null;
const canvas = $('camera'),
  ctx = canvas.getContext('2d');
function message(id, text, error = false) {
  $(id).textContent = text;
  $(id).classList.toggle('error', error);
}
function setMenu(open) {
  $('park-sidebar').classList.toggle('minimized', !open);
  document.body.classList.toggle('menu-minimized', !open);
  $('toggle-menu').setAttribute('aria-expanded', String(open));
  $('toggle-menu').setAttribute('aria-label', open ? 'Minimizar menu' : 'Expandir menu');
}
$('toggle-menu').addEventListener('click', () =>
  setMenu($('toggle-menu').getAttribute('aria-expanded') !== 'true'),
);
setMenu(!matchMedia('(max-width:900px)').matches);
async function post(path, body) {
  const r = await apiFetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const j = await r.json();
  if (!r.ok) throw Error(j.error || 'A ESP não confirmou a operação.');
  return j;
}
async function refresh() {
  if (fetching || document.hidden) return;
  fetching = true;
  try {
    const r = await apiFetch('/api/local');
    const j = await r.json();
    if (!r.ok) throw Error(j.error || 'Sem resposta da ESP.');
    state = j;
    $('park-name').textContent = j.device.name;
    $('access-name').textContent = j.device.access;
    const c = j.device.counter;
    if (c && !calibrationDirty) {
      $('axis').value = c.horizontal ? 'y' : 'x';
      $('line-a').value = Math.round(c.first * 100);
      $('line-b').value = Math.round(c.second * 100);
      $('reverse').checked = !!c.reverse;
      showDirection();
    }
    $('apply-lines').disabled = !c?.ready;
    const w = j.network.wifi;
    $('wifi-current').textContent = w.connected
      ? 'ESP conectada ao roteador por Wi-Fi'
      : 'Sem conexão confirmada';
    $('wifi-network').textContent = w.ssid || '—';
    $('wifi-address').textContent = j.device.address;
    $('wifi-signal').textContent = w.connected ? w.rssi + ' dBm' : '—';
    $('save-wifi').disabled = wifiSubmitting || w.busy;
    $('storage-status').textContent = j.storage.ok
      ? 'microSD: gravação disponível.'
      : 'microSD: ' + (j.storage.error || 'indisponível.');
    const cloud = j.cloud;
    $('cloud-status').textContent = !cloud?.configured
      ? 'Envio ainda não configurado.'
      : cloud.last_success
        ? 'Último envio confirmado: ' +
          new Date(cloud.last_success).toLocaleString('pt-BR', { timeZone: 'America/Sao_Paulo' }) +
          '.'
        : cloud.phase === 'waiting_clock'
          ? 'Aguardando horário válido para enviar.'
          : 'Aguardando confirmação de envio à nuvem.';
    $('request-error').hidden = true;
    if (!j.today) await post('/api/time', { epoch: Math.floor(Date.now() / 1000) });
  } catch (e) {
    $('request-error').hidden = false;
    $('request-error').textContent =
      'Não foi possível atualizar: ' + e.message + ' A última leitura pode estar desatualizada.';
  } finally {
    fetching = false;
  }
}
function showDirection() {
  const r = $('reverse').checked,
    x = $('axis').value === 'x';
  $('direction').textContent =
    'Entrada: ' +
    (r ? 'B para A' : 'A para B') +
    ', ' +
    (x
      ? r
        ? 'direita para esquerda'
        : 'esquerda para direita'
      : r
        ? 'baixo para cima'
        : 'cima para baixo') +
    '. Saída no sentido contrário.';
}
$('calibration-form').addEventListener('input', () => {
  calibrationDirty = true;
  showDirection();
});
$('calibration-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('apply-lines').disabled = true;
  try {
    await post('/api/calibration', {
      first: Number($('line-a').value) / 100,
      second: Number($('line-b').value) / 100,
      horizontal: $('axis').value === 'y',
      reverse: $('reverse').checked,
    });
    calibrationDirty = false;
    message('calibration-status', 'Linhas aplicadas. Contagens preservadas.');
    await refresh();
  } catch (e) {
    message('calibration-status', e.message, true);
  } finally {
    $('apply-lines').disabled = !state?.device.counter.ready;
  }
});
$('wifi-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (wifiSubmitting) return;
  const body = { ssid: $('wifi-ssid').value, password: $('wifi-password').value };
  if (
    new TextEncoder().encode(body.ssid).length > 32 ||
    new TextEncoder().encode(body.password).length > 63
  ) {
    message('wifi-message', 'Nome ou senha excede o limite em bytes.', true);
    return;
  }
  wifiSubmitting = true;
  $('save-wifi').disabled = true;
  $('wifi-password').value = '';
  try {
    await post('/api/wifi', body);
    message(
      'wifi-message',
      'Troca solicitada. Conecte este aparelho à nova rede. Se o endereço mudar, consulte pelo USB.',
    );
  } catch (e) {
    message(
      'wifi-message',
      'Troca não confirmada: ' + e.message + ' Confira a rede salva pelo USB.',
      true,
    );
  } finally {
    body.password = '';
    wifiSubmitting = false;
    $('save-wifi').disabled = false;
  }
});
// Only the preview is demand-driven; detection runs independently in firmware.
function scheduleCamera(delay = 350) {
  clearTimeout(cameraTimer);
  if (!cameraPaused && !document.hidden) cameraTimer = setTimeout(pollCamera, delay);
}
async function pollCamera() {
  if (cameraPaused || cameraDrawing || document.hidden) return;
  if (!state?.device.configured) {
    $('camera-status').textContent = 'Aguardando a ESP. Reconexão automática.';
    scheduleCamera(1500);
    return;
  }
  cameraDrawing = true;
  const epoch = previewGeneration;
  try {
    const response = await apiFetch('/api/frame', { signal: AbortSignal.timeout(9000) });
    const data = await response.json();
    if (!response.ok) throw Error(data.error || 'A câmera não respondeu.');
    if (cameraPaused || document.hidden || epoch !== previewGeneration) return;
    if (data.frame) {
      lastFrame = data.frame;
      await drawFrame(lastFrame, epoch);
      if (epoch !== previewGeneration) return;
      $('people-now').textContent = 'Pessoas no quadro: ' + data.frame.people;
    }
    $('camera-status').textContent =
      data.age > 6
        ? 'Imagem desatualizada. Verifique a ESP.'
        : data.frame
          ? 'Imagem ao vivo, ' + data.frame.width + ' × ' + data.frame.height + ' px.'
          : 'Aguardando imagem…';
  } catch (error) {
    $('camera-status').textContent = error.message + ' A última imagem pode estar desatualizada.';
  } finally {
    cameraDrawing = false;
    scheduleCamera();
  }
}
async function drawFrame(frame, epoch = previewGeneration) {
  const image = new Image();
  image.src = 'data:image/jpeg;base64,' + frame.jpeg;
  await image.decode();
  if (cameraPaused || document.hidden || epoch !== previewGeneration) return;
  canvas.width = frame.width;
  canvas.height = frame.height;
  ctx.drawImage(image, 0, 0);
  $('camera-placeholder').hidden = true;
  const trackText = {
    1: 'Lado A',
    2: 'Lado B',
    3: 'Sem lado inicial',
    4: 'Rastreando',
    5: 'Entrada contada',
    6: 'Saída contada',
  };
  $('tracks').textContent = frame.boxes.length
    ? frame.boxes
        .map((b) =>
          b[5] ? 'ID ' + b[5] + ': ' + (trackText[b[6]] || 'Rastreando') : 'Detecção sem trajeto',
        )
        .join('; ')
    : 'Nenhuma pessoa neste quadro.';
  const counter = frame.counter;
  if (counter) {
    ctx.lineWidth = 2;
    ctx.font = '13px "Segoe UI"';
    for (const [label, value, color] of [
      ['A', counter.first, '#65d9ff'],
      ['B', counter.second, '#ffb788'],
    ]) {
      ctx.strokeStyle = ctx.fillStyle = color;
      ctx.beginPath();
      if (counter.horizontal) {
        const y = value * canvas.height;
        ctx.moveTo(0, y);
        ctx.lineTo(canvas.width, y);
        ctx.fillText(label, 6, Math.max(14, y - 4));
      } else {
        const x = value * canvas.width;
        ctx.moveTo(x, 0);
        ctx.lineTo(x, canvas.height);
        ctx.fillText(label, x + 4, 16);
      }
      ctx.stroke();
    }
  }
  if ($('show-boxes').checked) {
    ctx.lineWidth = 2;
    ctx.font = '12px "Segoe UI"';
    for (const [x1, y1, x2, y2, score, id, trackState] of frame.boxes) {
      ctx.strokeStyle = trackState === 5 || trackState === 6 ? '#77f2a0' : '#ffdb52';
      ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
      ctx.fillStyle = 'white';
      ctx.beginPath();
      ctx.arc((x1 + x2) / 2, (y1 + y2) / 2, 3, 0, Math.PI * 2);
      ctx.fill();
      const label = (id ? 'ID ' + id : 'Pessoa') + ' ' + Math.round(score * 100) + '%';
      const x = Math.max(0, Math.min(x1, canvas.width - ctx.measureText(label).width - 8)),
        y = Math.max(0, y1 - 18);
      ctx.fillStyle = '#233b30';
      ctx.fillRect(x, y, ctx.measureText(label).width + 8, 18);
      ctx.fillStyle = 'white';
      ctx.fillText(label, x + 4, y + 13);
    }
  }
}
$('pause-camera').addEventListener('click', () => {
  cameraPaused = !cameraPaused;
  previewGeneration++;
  $('pause-camera').textContent = cameraPaused ? 'Retomar imagem' : 'Pausar imagem';
  if (cameraPaused) {
    clearTimeout(cameraTimer);
    $('camera-status').textContent = 'Imagem pausada. A contagem continua.';
  } else scheduleCamera(0);
});
$('show-boxes').addEventListener('change', () => {
  if (lastFrame && !cameraPaused) drawFrame(lastFrame).catch(() => {});
});
document.addEventListener('visibilitychange', () => {
  previewGeneration++;
  if (!document.hidden) {
    refresh();
    scheduleCamera(0);
  } else clearTimeout(cameraTimer);
});

if (accessToken) {
  refresh().then(() => scheduleCamera(0));
  setInterval(() => {
    if (!document.hidden) refresh();
  }, 5000);
} else {
  $('request-error').hidden = false;
  $('request-error').textContent =
    'Abra o link completo da ESP, com a chave de acesso; consulte pelo USB se necessário.';
}
