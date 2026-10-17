'use strict';
const $ = (id) => document.getElementById(id);
const cfg = window.PARK_CLOUD;
const number = (v) => new Intl.NumberFormat('pt-BR').format(BigInt(v || '0'));
const escapeHTML = (v) =>
  String(v).replace(
    /[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c],
  );
const momentLabel = (v) =>
  v
    ? new Date(v).toLocaleString('pt-BR', { timeZone: 'America/Sao_Paulo' })
    : 'Nenhuma sincronização';
const today = () => {
  const p = Object.fromEntries(
    new Intl.DateTimeFormat('en-CA', {
      timeZone: 'America/Sao_Paulo',
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
    })
      .formatToParts(new Date())
      .map((p) => [p.type, p.value]),
  );
  return p.year + '-' + p.month + '-' + p.day;
};
let session = null,
  parks = [],
  devices = [],
  park = '',
  start = '',
  end = '',
  report = null,
  page = 'overview',
  busy = false,
  generation = 0,
  days = 1;
let isOwner = false,
  cameraAccess = null,
  cameraError = '',
  cameraPending = false;
try {
  session = JSON.parse(sessionStorage.getItem('park-cloud-session') || 'null');
} catch {}
function saveSession(value) {
  session = value;
  try {
    if (value) sessionStorage.setItem('park-cloud-session', JSON.stringify(value));
    else sessionStorage.removeItem('park-cloud-session');
  } catch {}
}
async function request(path, { body, auth = true, method } = {}) {
  if (!cfg?.publishableKey) throw Error('A chave pública do projeto ainda não foi configurada.');
  const response = await fetch(cfg.url + path, {
    cache: 'no-store',
    method: method || (body ? 'POST' : 'GET'),
    headers: {
      apikey: cfg.publishableKey,
      ...(auth && session ? { Authorization: 'Bearer ' + session.access_token } : {}),
      ...(body ? { 'Content-Type': 'application/json' } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(20000),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    if (response.status === 401 && auth) {
      saveSession(null);
      showAuth();
    }
    throw Error(
      data.msg || data.message || data.error_description || 'Não foi possível consultar os dados.',
    );
  }
  return data;
}
const rpc = (name, body) => request('/rest/v1/rpc/' + name, { body });
function error(message = '') {
  $('error').textContent = message;
  $('error').hidden = !message;
}
function showAuth() {
  $('login').hidden = !!session;
  $('application').hidden = !session;
}
async function refreshSession() {
  if (!session) return;
  if ((session.expires_at || 0) > Date.now() / 1000 + 60) return;
  try {
    const result = await request('/auth/v1/token?grant_type=refresh_token', {
      auth: false,
      body: { refresh_token: session.refresh_token },
    });
    saveSession({ ...result, expires_at: Math.floor(Date.now() / 1000) + result.expires_in });
  } catch (e) {
    saveSession(null);
    showAuth();
    throw e;
  }
}
$('login-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  $('login-submit').disabled = true;
  $('login-message').textContent = 'Entrando…';
  const password = $('password').value;
  $('password').value = '';
  try {
    const result = await request('/auth/v1/token?grant_type=password', {
      auth: false,
      body: { email: $('email').value, password },
    });
    saveSession({ ...result, expires_at: Math.floor(Date.now() / 1000) + result.expires_in });
    showAuth();
    $('login-message').textContent = '';
    await loadParks();
  } catch (e) {
    $('login-message').textContent = e.message;
  } finally {
    $('login-submit').disabled = false;
  }
});
$('sign-out').addEventListener('click', async () => {
  generation++;
  try {
    await request('/auth/v1/logout', { method: 'POST' });
  } catch {}
  saveSession(null);
  parks = [];
  devices = [];
  park = '';
  report = null;
  isOwner = false;
  cameraAccess = null;
  cameraError = '';
  $('device-secret').hidden = true;
  $('device-token').value = '';
  render();
  showAuth();
});
const datePicker = new window.ParkDateRange(today);
function selectPeriod(n) {
  days = n;
  document.querySelectorAll('[data-days]').forEach((b) => {
    const active = +b.dataset.days === n;
    b.classList.toggle('selected', active);
    b.setAttribute('aria-pressed', String(active));
  });
  $('custom-period').classList.toggle('selected', n === 0);
  $('custom-period').setAttribute('aria-pressed', String(n === 0));
}
function setDates(n) {
  selectPeriod(n);
  datePicker.close();
  end = today();
  const d = new Date(end + 'T12:00:00-03:00');
  d.setUTCDate(d.getUTCDate() - n + 1);
  start = d.toISOString().slice(0, 10);
  $('date-start').value = start;
  $('date-end').value = end;
}
document.querySelectorAll('[data-days]').forEach((b) =>
  b.addEventListener('click', () => {
    generation++;
    setDates(+b.dataset.days);
    refresh();
  }),
);
$('date-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const a = $('date-start').value,
    b = $('date-end').value;
  if (!a || !b || a > b || b > today() || (new Date(b) - new Date(a)) / 86400000 > 365) {
    error('Selecione até 366 dias, sem datas futuras.');
    return;
  }
  start = a;
  end = b;
  selectPeriod(0);
  generation++;
  datePicker.close();
  $('custom-period').focus();
  refresh();
});
$('custom-period').addEventListener('click', () => {
  selectPeriod(0);
  datePicker.open(start, end);
});
async function loadParks() {
  await refreshSession();
  const linked = await request(
    '/rest/v1/park_devices?select=park_id&id=eq.' + encodeURIComponent(cfg.deviceId),
  );
  park = linked[0]?.park_id || '';
  parks = park
    ? await request('/rest/v1/parks?select=id,name,access_name&id=eq.' + encodeURIComponent(park))
    : [];
  $('onboarding').hidden = parks.length > 0;
  $('report-controls').hidden = !parks.length;
  if (!parks.length) park = '';
  const memberships = park
    ? await request('/rest/v1/park_members?select=role&park_id=eq.' + encodeURIComponent(park))
    : [];
  isOwner = memberships.some((m) => m.role === 'owner');
  cameraAccess = null;
  cameraError = '';
  $('edit-identification').disabled = !isOwner;
  setPage();
  if (park) await refresh();
  else {
    report = null;
    devices = [];
    render();
  }
}
const identificationDialog = $('identification-dialog');
$('edit-identification').addEventListener('click', () => {
  const p = parks.find((p) => p.id === park);
  if (!p) return;
  $('setting-name').value = p.name;
  $('setting-access').value = p.access_name;
  $('identification-message').textContent = '';
  identificationDialog.showModal();
});
$('close-identification').addEventListener('click', () => identificationDialog.close());
$('identification-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  $('save-identification').disabled = true;
  $('identification-message').textContent = 'Salvando…';
  try {
    await rpc('park_update_identification', {
      p_park: park,
      p_name: $('setting-name').value,
      p_access: $('setting-access').value,
    });
    await loadParks();
    identificationDialog.close();
  } catch (e) {
    $('identification-message').textContent = e.message;
  } finally {
    $('save-identification').disabled = false;
  }
});
async function refresh() {
  if (!session || !park || busy || document.hidden) return;
  busy = true;
  const version = generation;
  try {
    await refreshSession();
    if (days && end !== today()) setDates(days);
    const [next, ds, ca] = await Promise.all([
      rpc('park_report', { p_park: park, p_start: start, p_end: end }),
      request(
        '/rest/v1/park_devices?select=id,name,last_sync,storage_ok,clock_ok,enabled,operation&id=eq.' +
          encodeURIComponent(cfg.deviceId) +
          '&park_id=eq.' +
          encodeURIComponent(park),
      ),
      isOwner
        ? rpc('park_camera_access', { p_device: cfg.deviceId, p_include_key: false })
            .then((value) => ({ value }))
            .catch(() => ({
              error:
                'Não foi possível consultar o endereço. Tente novamente quando a conexão voltar.',
            }))
        : Promise.resolve({ value: null }),
    ]);
    if (version !== generation) return;
    report = next;
    devices = ds;
    cameraAccess = ca.value || null;
    cameraError = ca.error || '';
    error();
    render();
  } catch (e) {
    error(e.message + ' Os dados exibidos podem estar desatualizados.');
  } finally {
    busy = false;
    if (version !== generation) refresh();
  }
}
function table(rows, columns) {
  if (!rows.length) return '<p class="empty-list">Sem entradas ou saídas neste período.</p>';
  return (
    '<div class="table-scroll"><table><thead><tr>' +
    columns.map((c) => '<th>' + escapeHTML(c.title) + '</th>').join('') +
    '</tr></thead><tbody>' +
    rows
      .map(
        (row) =>
          '<tr>' +
          columns.map((c) => '<td>' + escapeHTML(c.value(row)) + '</td>').join('') +
          '</tr>',
      )
      .join('') +
    '</tbody></table></div>'
  );
}
const historyColumns = [
  { title: 'Registrado em', value: (r) => momentLabel(r.at) },
  { title: 'Entradas', value: (r) => number(r.entries) },
  { title: 'Saídas', value: (r) => number(r.exits) },
];
function render() {
  renderCameraAccess();
  const p = parks.find((p) => p.id === park);
  $('park-name').textContent = p?.name || 'Parque';
  $('access-name').textContent = p?.access_name || 'Acesso principal';
  const last = devices
    .map((d) => d.last_sync)
    .filter(Boolean)
    .sort()
    .at(-1);
  const operation = devices[0]?.operation || {};
  const planned = operation.state === 'sleep_planned';
  $('sync-note').textContent = last
    ? 'Última sincronização: ' +
      momentLabel(last) +
      (planned ? '. Sono programado informado; isso não confirma o estado atual.' : '. A placa envia novidades, não um sinal contínuo de presença.')
    : 'Aguardando o primeiro envio da ESP.';
  $('device-list').innerHTML = devices.length
    ? devices
        .map(
          (d) =>
            '<h3>' +
            escapeHTML(d.name) +
            '</h3><dl><dt>Último envio</dt><dd>' +
            escapeHTML(momentLabel(d.last_sync)) +
            '</dd><dt>Funcionamento na última comunicação</dt><dd>' +
            escapeHTML(d.operation?.state === 'sleep_planned'
              ? 'Preparação do sono informada. Estado atual não confirmado.'
              : 'Placa acordada na última comunicação. Estado atual não confirmado.') +
            (d.operation?.state === 'sleep_planned' && Number(d.operation.expected_wake_epoch) > 0
              ? ' Próximo despertar periódico previsto: ' + escapeHTML(momentLabel(new Date(Number(d.operation.expected_wake_epoch) * 1000).toISOString())) + '. O PIR pode antecipar.' : '') +
            '</dd><dt>Câmera na última leitura</dt><dd>' +
            (d.operation?.camera_ok === false ? 'Falha informada; contagem pode estar interrompida' : d.operation?.camera_ok === true ? 'Sem falha informada' : 'Sem leitura') +
            '</dd><dt>microSD na última leitura</dt><dd>' +
            (d.storage_ok === true
              ? 'Gravação disponível'
              : d.storage_ok === false
                ? 'Falha informada'
                : 'Sem leitura') +
            '</dd><dt>Relógio na última leitura</dt><dd>' +
            (d.clock_ok === true
              ? 'Válido'
              : d.clock_ok === false
                ? 'Sem horário válido'
                : 'Sem leitura') +
            '</dd></dl><button type="button" class="button" data-rotate-device="' +
            escapeHTML(d.id) +
            '">Substituir chave de envio</button>',
        )
        .join('')
    : '<p>Nenhuma placa cadastrada neste parque.</p>';
  $('device-form').hidden = !isOwner || !park || devices.length > 0;
  $('device-list')
    .querySelectorAll('[data-rotate-device]')
    .forEach((button) => {
      button.hidden = !isOwner;
    });
  if (!report) {
    $('entries').textContent = '—';
    $('exits').textContent = '—';
    $('chart').replaceChildren();
    $('recent-history').textContent = 'Aguardando registros.';
    $('daily-history').replaceChildren();
    $('full-history').replaceChildren();
    $('period-label').textContent = '';
    return;
  }
  $('entries').textContent = report.has_samples ? number(report.entries) : '—';
  $('exits').textContent = report.has_samples ? number(report.exits) : '—';
  const dateLabel = (v) =>
    new Date(v + 'T12:00:00-03:00').toLocaleDateString('pt-BR', {
      timeZone: 'America/Sao_Paulo',
      day: 'numeric',
      month: 'short',
      year: 'numeric',
    });
  $('period-label').textContent =
    start === end ? dateLabel(start) : dateLabel(start) + ' a ' + dateLabel(end);
  $('chart-title').textContent = start === end ? 'Passagens por hora' : 'Passagens por dia';
  const movements = (report.history || []).filter((r) => r.kind === 'movement');
  $('recent-history').innerHTML = table(movements.slice(0, 5), historyColumns);
  $('full-history').innerHTML = table(movements, historyColumns);
  $('daily-history').innerHTML = table(report.daily || [], [
    { title: 'Data', value: (r) => r.date.split('-').reverse().join('/') },
    { title: 'Entradas', value: (r) => (r.samples ? number(r.entries) : '—') },
    { title: 'Saídas', value: (r) => (r.samples ? number(r.exits) : '—') },
    { title: 'Estado', value: (r) => (r.samples ? 'Registros disponíveis' : 'Sem registros') },
  ]);
  drawChart(report);
}
function renderCameraAccess() {
  $('camera-access-section').hidden = !park || !isOwner;
  $('camera-address').textContent = cameraAccess?.available
    ? cameraAccess.address
    : 'Aguardando a ESP';
  $('camera-updated').textContent = cameraAccess?.updated_at
    ? momentLabel(cameraAccess.updated_at)
    : '—';
  $('camera-access-status').textContent =
    cameraError ||
    (!cameraAccess?.available
      ? 'O endereço aparece automaticamente depois do próximo envio da ESP.'
      : 'Último endereço informado. Abra na mesma rede da ESP; durante o sono, o vídeo e o site local ficam indisponíveis. O endereço não confirma que a placa está acessível agora.');
  $('open-camera-link').disabled = $('copy-camera-link').disabled =
    !cameraAccess?.available || cameraPending || !isOwner;
}
function validateCameraURL(value) {
  const url = new URL(value),
    parts = url.hostname.split('.').map(Number);
  const privateIP =
    parts.length === 4 &&
    parts.every((n) => Number.isInteger(n) && n >= 0 && n <= 255) &&
    (parts[0] === 10 ||
      (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) ||
      (parts[0] === 192 && parts[1] === 168));
  if (
    url.protocol !== 'http:' ||
    !privateIP ||
    url.port ||
    url.username ||
    url.password ||
    url.pathname !== '/' ||
    url.search ||
    !/^#[a-f0-9]{32}$/.test(url.hash)
  )
    throw Error('A ESP informou um link local inválido.');
  return url.href;
}
async function currentCameraURL() {
  await refreshSession();
  const result = await rpc('park_camera_access', { p_device: cfg.deviceId, p_include_key: true });
  if (!result.available || !result.url)
    throw Error('O link ainda não foi informado ou o serviço precisa ser atualizado. Aguarde uma comunicação da ESP.');
  return validateCameraURL(result.url);
}
$('open-camera-link').addEventListener('click', async () => {
  if (cameraPending || !isOwner) return;
  const popup = window.open('about:blank', '_blank');
  if (!popup) {
    $('camera-access-message').textContent =
      'O navegador bloqueou a nova aba. Permita a abertura ou use Copiar link para o celular.';
    return;
  }
  popup.opener = null;
  cameraPending = true;
  renderCameraAccess();
  $('camera-access-message').textContent = 'Consultando o link atual…';
  const version = generation;
  try {
    const url = await currentCameraURL();
    if (version !== generation || !session || !isOwner) {
      popup.close();
      return;
    }
    popup.location.replace(url);
    $('camera-access-message').textContent =
      'Câmera aberta em outra aba. Se não carregar, confira se este aparelho está no mesmo Wi-Fi da ESP.';
  } catch {
    popup.close();
    $('camera-access-message').textContent =
      'Não foi possível abrir o link atualizado. Confira a conexão e tente novamente.';
  } finally {
    cameraPending = false;
    renderCameraAccess();
  }
});
$('copy-camera-link').addEventListener('click', async () => {
  if (cameraPending || !isOwner) return;
  cameraPending = true;
  renderCameraAccess();
  $('camera-access-message').textContent = 'Consultando o link atual…';
  const version = generation;
  try {
    const url = await currentCameraURL();
    if (version !== generation || !session || !isOwner) return;
    await navigator.clipboard.writeText(url);
    $('camera-access-message').textContent =
      'Link completo copiado. Abra no celular conectado ao mesmo Wi-Fi da ESP; compartilhe somente com responsáveis.';
  } catch {
    $('camera-access-message').textContent =
      'Não foi possível copiar. Confira a conexão ou use Abrir câmera e ajustes.';
  } finally {
    cameraPending = false;
    renderCameraAccess();
  }
});
let chartSelection = -1,
  chartRange = '',
  chartBuckets = [],
  chartDetailVisible = false,
  chartRestoringFocus = false;
function drawChart(report) {
  const box = $('chart');
  const total = BigInt(report.entries || '0') + BigInt(report.exits || '0');
  const buckets = report.chart?.length
    ? report.chart
    : Array.from({ length: 24 }, (_, i) => ({
        label: String(i).padStart(2, '0') + 'h',
        entries: '0',
        exits: '0',
        samples: 0,
      }));
  chartBuckets = buckets;
  const range = start + '/' + end;
  if (range !== chartRange) {
    chartSelection = -1;
    chartRange = range;
    chartDetailVisible = false;
  }
  if (chartSelection >= buckets.length) chartSelection = -1;
  const width = Math.max(200, box.clientWidth || 800),
    height = box.clientHeight || 340;
  const left = width < 440 ? 34 : 48,
    right = 10,
    top = 24,
    bottom = 40;
  const plotWidth = width - left - right,
    plotHeight = height - top - bottom;
  const rawMax = Math.max(...buckets.map((b) => Math.max(Number(b.entries), Number(b.exits))), 1);
  const magnitude = Math.max(1, 10 ** Math.floor(Math.log10(rawMax / 4)));
  const tick = Math.max(1, Math.ceil(rawMax / 4 / magnitude) * magnitude),
    max = tick * 4;
  let html = `<svg viewBox="0 0 ${width} ${height}" aria-label="Gráfico de barras de entradas e saídas">`;
  for (let i = 0; i <= 4; i++) {
    const y = top + plotHeight * (1 - i / 4),
      value = Math.round((max * i) / 4);
    html += `<line class="${i ? 'grid-line' : 'axis-line'}" x1="${left}" y1="${y}" x2="${width - right}" y2="${y}"/><text x="${left - 12}" y="${y + 4}" text-anchor="end">${escapeHTML(new Intl.NumberFormat('pt-BR', { notation: value >= 10000 ? 'compact' : 'standard' }).format(value))}</text>`;
  }
  const step = plotWidth / buckets.length,
    bar = Math.max(1.6, Math.min(24, step * 0.3));
  const labelEvery = Math.max(1, Math.ceil(buckets.length / (width < 440 ? 5 : 12)));
  buckets.forEach((b, i) => {
    const cx = left + step * (i + 0.5);
    const text = b.samples
      ? b.label + ': ' + number(b.entries) + ' entradas, ' + number(b.exits) + ' saídas'
      : b.label + ': sem coleta';
    html += `<g class="bucket${i === chartSelection && chartDetailVisible ? ' active' : ''}" data-bucket="${i}" role="button" tabindex="${i === (chartSelection < 0 ? 0 : chartSelection) ? 0 : -1}" aria-pressed="${i === chartSelection && chartDetailVisible}" aria-label="${escapeHTML(text)}"><title>${escapeHTML(text)}</title><rect class="bucket-hit" x="${left + step * i}" y="${top - 4}" width="${step}" height="${plotHeight + 13}" rx="6"/>`;
    for (const [key, style, offset] of [
      ['entries', 'bar-entry', -bar - 1],
      ['exits', 'bar-exit', 1],
    ]) {
      const value = Number(b[key]),
        h = (value / max) * plotHeight;
      if (value)
        html += `<rect class="${style}" x="${cx + offset}" y="${top + plotHeight - h}" width="${bar}" height="${h}" rx="${Math.min(4, bar / 2)}"/>`;
    }
    if (!b.samples)
      html += `<line class="no-sample" x1="${cx - 2}" y1="${top + plotHeight + 8}" x2="${cx + 2}" y2="${top + plotHeight + 8}"/>`;
    else if (!Number(b.entries) && !Number(b.exits))
      html += `<circle class="zero-marker" cx="${cx}" cy="${top + plotHeight}" r="2.4"/>`;
    html += '</g>';
    if (i % labelEvery === 0 || i === buckets.length - 1)
      html += `<text x="${cx}" y="${height - 10}" text-anchor="middle">${escapeHTML(b.label)}</text>`;
  });
  html += '</svg>';
  if (!report.has_samples || !total)
    html +=
      '<div class="chart-status"><strong>' +
      (!report.has_samples ? 'Sem dados neste período' : 'Nenhuma passagem no período') +
      '</strong><p>' +
      (!report.has_samples
        ? 'Os dados aparecem depois que a ESP envia seus registros.'
        : 'Os horários com coleta estão marcados no eixo. Traços abaixo do eixo indicam horários sem coleta.') +
      '</p></div>';
  // Keep focus during the regular 5-second refresh without moving a reader's focus.
  const focusedBucket = box.contains?.(document.activeElement)
    ? document.activeElement?.getAttribute('data-bucket')
    : null;
  box.innerHTML = html;
  if (chartSelection < 0 || !chartDetailVisible) $('chart-detail').hidden = true;
  box.setAttribute(
    'aria-label',
    !report.has_samples
      ? 'Sem dados coletados no período'
      : `Passagens no período: ${number(report.entries)} entradas e ${number(report.exits)} saídas. Use as setas para consultar os horários. Traços abaixo do eixo indicam falta de coleta.`,
  );
  if (chartSelection >= 0 && chartDetailVisible) showChartBucket(chartSelection, false);
  if (focusedBucket !== null) {
    chartRestoringFocus = true;
    try {
      box.querySelector?.('[data-bucket="' + focusedBucket + '"]')?.focus({ preventScroll: true });
    } finally {
      chartRestoringFocus = false;
    }
  }
}
function showChartBucket(index, focus = false) {
  const bucket = chartBuckets[index];
  if (!bucket) return;
  chartSelection = index;
  chartDetailVisible = true;
  const detail = $('chart-detail');
  detail.innerHTML =
    '<strong>' +
    escapeHTML(bucket.label) +
    '</strong><p>' +
    (bucket.samples
      ? number(bucket.entries) + ' entradas / ' + number(bucket.exits) + ' saídas'
      : 'Sem coleta neste horário') +
    '</p>';
  detail.hidden = false;
  $('chart')
    .querySelectorAll('[data-bucket]')
    .forEach((group) => {
      const active = Number(group.dataset.bucket) === index;
      group.classList.toggle('active', active);
      group.setAttribute('aria-pressed', String(active));
      group.setAttribute('tabindex', active ? '0' : '-1');
      if (active && focus) group.focus({ preventScroll: true });
    });
}
function hideChartBucket() {
  chartDetailVisible = false;
  $('chart-detail').hidden = true;
  $('chart')
    .querySelectorAll('[data-bucket]')
    .forEach((group) => {
      group.classList.remove('active');
      group.setAttribute('aria-pressed', 'false');
    });
}
$('chart').addEventListener('pointerleave', (event) => {
  if (event.pointerType !== 'touch') hideChartBucket();
});
$('chart').addEventListener('focusout', (event) => {
  if (!$('chart').contains?.(event.relatedTarget)) hideChartBucket();
});
document.addEventListener('pointerdown', (event) => {
  if (!$('chart').contains?.(event.target)) hideChartBucket();
});
$('chart').addEventListener('click', (event) => {
  const group = event.target.closest?.('[data-bucket]');
  if (group) showChartBucket(Number(group.dataset.bucket));
});
$('chart').addEventListener('pointerover', (event) => {
  if (event.pointerType === 'touch') return;
  const group = event.target.closest?.('[data-bucket]');
  if (group) showChartBucket(Number(group.dataset.bucket));
});
$('chart').addEventListener('focusin', (event) => {
  if (chartRestoringFocus) return;
  const group = event.target.closest?.('[data-bucket]');
  if (group) showChartBucket(Number(group.dataset.bucket));
});
$('chart').addEventListener('keydown', (event) => {
  const group = event.target.closest?.('[data-bucket]');
  if (!group) return;
  let index = Number(group.dataset.bucket);
  if (event.key === 'ArrowRight') index = Math.min(chartBuckets.length - 1, index + 1);
  else if (event.key === 'ArrowLeft') index = Math.max(0, index - 1);
  else if (event.key === 'Home') index = 0;
  else if (event.key === 'End') index = chartBuckets.length - 1;
  else if (event.key === 'Escape') {
    hideChartBucket();
    return;
  } else if (!['Enter', ' '].includes(event.key)) return;
  event.preventDefault();
  showChartBucket(index, true);
});
let chartResizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(chartResizeTimer);
  chartResizeTimer = setTimeout(() => {
    if (report && page === 'overview') drawChart(report);
  }, 120);
});
function setPage() {
  page = location.hash.slice(1);
  const titles = { overview: 'Visitação', history: 'Histórico', equipment: 'Equipamento' };
  if (!titles[page]) page = 'overview';
  $('page-title').textContent = titles[page];
  for (const id of Object.keys(titles)) $('page-' + id).hidden = id !== page;
  document.querySelectorAll('[data-page]').forEach((a) => {
    const active = a.dataset.page === page;
    a.classList.toggle('active', active);
    if (active) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
  const show = !!park && ['overview', 'history'].includes(page);
  $('report-controls').hidden = !show;
  $('report-actions').hidden = !show;
  if (!show) datePicker.close();
  $('chart-detail').hidden = true;
  if (report && page === 'overview') drawChart(report);
}
window.addEventListener('hashchange', setPage);
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
$('export').addEventListener('click', () => {
  if (!report) return;
  const csv =
    'data,entradas,saidas\r\n' +
    report.daily
      .map((r) => [r.date, r.samples ? r.entries : '', r.samples ? r.exits : ''].join(','))
      .join('\r\n');
  const url = URL.createObjectURL(new Blob(['\ufeff' + csv], { type: 'text/csv;charset=utf-8' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = 'visitacao-' + start + '-' + end + '.csv';
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
$('print').addEventListener('click', () => window.print());
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) refresh();
});
$('device-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const button = event.submitter;
  button.disabled = true;
  try {
    const result = await rpc('park_create_device', {
      p_park: park,
      p_name: $('device-name').value,
    });
    $('device-id').value = result.device_id;
    $('device-token').value = result.device_token;
    $('device-secret').hidden = false;
    await refresh();
  } catch (e) {
    error(e.message);
  } finally {
    button.disabled = false;
  }
});
$('hide-device-secret').addEventListener('click', () => {
  $('device-token').value = '';
  $('device-id').value = '';
  $('device-secret').hidden = true;
});
for (const [button, field] of [
  ['copy-device-id', 'device-id'],
  ['copy-device-token', 'device-token'],
])
  $(button).addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText($(field).value);
      $('copy-message').textContent =
        field === 'device-id'
          ? 'ID copiado. Cole no terminal.'
          : 'Chave copiada. Cole apenas no configurador USB.';
    } catch {
      $(field).type = 'text';
      $(field).select();
      $('copy-message').textContent =
        'A cópia automática foi bloqueada. O valor está selecionado; use Ctrl+C ou a opção Copiar do celular.';
    }
  });
$('show-device-token').addEventListener('click', () => {
  const show = $('device-token').type === 'password';
  $('device-token').type = show ? 'text' : 'password';
  $('show-device-token').textContent = show ? 'Ocultar chave' : 'Mostrar chave';
  $('show-device-token').setAttribute('aria-pressed', String(show));
});
$('device-list').addEventListener('click', async (event) => {
  const button = event.target.closest('[data-rotate-device]');
  if (!button) return;
  if (
    !window.confirm(
      'Substituir a chave de envio desta placa? A chave anterior deixará de funcionar. Configure a nova pelo USB.',
    )
  )
    return;
  button.disabled = true;
  try {
    const result = await rpc('park_rotate_device', { p_device: button.dataset.rotateDevice });
    $('device-id').value = result.device_id;
    $('device-token').value = result.device_token;
    $('device-token').type = 'password';
    $('show-device-token').textContent = 'Mostrar chave';
    $('show-device-token').setAttribute('aria-pressed', 'false');
    $('copy-message').textContent = '';
    $('device-secret').hidden = false;
    $('device-secret').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (e) {
    error(e.message);
  } finally {
    button.disabled = false;
  }
});
setDates(1);
$('date-start').max = today();
$('date-end').max = today();
setPage();
showAuth();
if (session) loadParks().catch((e) => error(e.message));
setInterval(refresh, 30000);
