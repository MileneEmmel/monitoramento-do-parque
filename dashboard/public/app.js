'use strict';
const $ = id => document.getElementById(id);
const titles = {
  overview: ['Visitação'], history: ['Histórico'],
  camera: ['Câmera e contagem'], equipment: ['Equipamento'], network: ['Wi-Fi e acesso']
};
const number = value => new Intl.NumberFormat('pt-BR').format(BigInt(value || '0'));
const dateLabel = value => new Date(value+'T12:00:00-03:00').toLocaleDateString('pt-BR', {day:'2-digit',month:'short',year:'numeric',timeZone:'America/Sao_Paulo'});
const momentLabel = value => value ? new Date(value).toLocaleString('pt-BR', {day:'2-digit',month:'2-digit',hour:'2-digit',minute:'2-digit',timeZone:'America/Sao_Paulo'}) : 'Nenhuma leitura';
function duration(seconds) {
  if (seconds < 60) return Math.floor(seconds)+' s';
  const minutes = Math.floor(seconds/60);
  return minutes < 60 ? minutes+' min' : Math.floor(minutes/60)+' h '+String(minutes%60).padStart(2,'0')+' min';
}
const escapeHTML = text => String(text).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let accessToken = '';
const accessMatch = location.hash.match(/^#(?:access=)?([0-9a-f]{32})$/i);
try { accessToken = accessMatch?.[1] || sessionStorage.getItem('park-access') || ''; } catch {}
if (!/^[0-9a-f]{32}$/i.test(accessToken)) accessToken = '';
if (accessMatch) {
  try { sessionStorage.setItem('park-access', accessToken); } catch {}
  window.history.replaceState(null, '', '#overview');
}
const apiFetch = (path, options = {}) => fetch(path, {...options, headers: {...options.headers, Authorization: 'Bearer ' + accessToken}});
let state = null, start = '', end = '', page = 'overview', csrf = '', rangeDays = 1;
let fetching = false, refreshAgain = false, requestGeneration = 0, cameraPaused = false, lastFrame = null, cameraDrawing = false, calibrationDirty = false;
let cameraTimer = null;
const dialog = $('identification-dialog');
let counterEpoch = 0;
const canvas = $('camera'), ctx = canvas.getContext('2d');

// Navigation is collapsible without changing the device or the current page.
const compactNavigation = () => Boolean(window.matchMedia?.('(max-width: 900px)').matches);
function setMenu(open, restoreFocus = false) {
  $('park-sidebar').classList.toggle('minimized', !open);
  document.body.classList.toggle('menu-minimized', !open);
  $('toggle-menu').setAttribute('aria-expanded', String(open));
  $('toggle-menu').setAttribute('aria-label', open ? 'Minimizar menu' : 'Expandir menu');
  if (restoreFocus) $('toggle-menu').focus();
}
$('toggle-menu').addEventListener('click', () => {
  const opening = $('toggle-menu').getAttribute('aria-expanded') !== 'true';
  setMenu(opening);
});
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && !dialog.open && $('toggle-menu').getAttribute('aria-expanded') === 'true' &&
      (compactNavigation() || $('park-sidebar').contains?.(document.activeElement))) setMenu(false, true);
});
document.querySelectorAll('[data-page]').forEach(link => link.addEventListener('click', () => {
  if (compactNavigation()) setMenu(false);
}));
setMenu(!compactNavigation());

function setPage() {
  page = location.hash.slice(1);
  if (!titles[page]) page = 'overview';
  for (const [key] of Object.entries(titles)) $('page-'+key).hidden = key !== page;
  document.querySelectorAll('[data-page]').forEach(link => {
    const active = link.dataset.page === page;
    link.classList.toggle('active',active);
    if (active) link.setAttribute('aria-current','page'); else link.removeAttribute('aria-current');
  });
  $('page-title').textContent = titles[page][0];
  $('period-bar').hidden = !['overview','history'].includes(page);
  if ($('period-bar').hidden) $('date-form').hidden = true;
  document.querySelector('.heading-actions').hidden = $('period-bar').hidden;
  if(state){hideChartBucket();render(state);requestGeneration++;refresh();}
  if (page === 'camera') scheduleCamera(0); else clearTimeout(cameraTimer);
}
window.addEventListener('hashchange',setPage);
setPage();

function setDates(days) {
  if (!state) return;
  rangeDays = days;
  end = state.today;
  const day = new Date(end+'T12:00:00-03:00');
  day.setUTCDate(day.getUTCDate()-days+1);
  start = day.toISOString().slice(0,10);
  $('date-start').value = start; $('date-end').value = end;
  document.querySelectorAll('[data-days]').forEach(button => {
    const selected = Number(button.dataset.days) === days;
    button.classList.toggle('selected',selected); button.setAttribute('aria-pressed',String(selected));
  });
  $('custom-period').classList.remove('selected');
  $('date-form').hidden = true; $('custom-period').setAttribute('aria-expanded','false');
}
document.querySelectorAll('[data-days]').forEach(button => button.addEventListener('click',() => {
  setDates(Number(button.dataset.days)); requestGeneration++; refresh();
}));
$('custom-period').addEventListener('click',() => {
  $('date-form').hidden = !$('date-form').hidden;
  $('custom-period').setAttribute('aria-expanded',String(!$('date-form').hidden));
  if (!$('date-form').hidden) $('date-start').focus();
});
$('date-form').addEventListener('submit',event => {
  event.preventDefault();
  start = $('date-start').value; end = $('date-end').value; rangeDays = 0;
  document.querySelectorAll('[data-days]').forEach(button => {button.classList.remove('selected');button.setAttribute('aria-pressed','false');});
  $('custom-period').classList.add('selected');
  requestGeneration++; refresh();
});
const query = () => new URLSearchParams({start,end}).toString();
async function post(path, body) {
  const response = await apiFetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify(body),signal:AbortSignal.timeout(22000)});
  const data = await response.json();
  if (!response.ok) throw Error(data.error || 'Não foi possível confirmar a operação.');
  return data;
}
async function refresh() {
  if (fetching) {refreshAgain=true;return;}
  fetching = true;
  const generation = requestGeneration;
  try {
    const response = await apiFetch('/api/state'+(page!=='camera' && start?'?'+query():''),{signal:AbortSignal.timeout(12000)});
    const data = await response.json();
    if (!response.ok) throw Error(data.error || 'Falha ao carregar o período.');
    if (generation !== requestGeneration) {refreshAgain=true;return;}
    if (!data.today) {
      $('request-error').hidden=false;
      $('request-error').textContent='A ESP está sem horário válido. Tentando sincronizar com este aparelho…';
      await post('/api/time',{epoch:Math.floor(Date.now()/1000)});
      refreshAgain=true; return;
    }
    const previousToday = state?.today;
    state = data; csrf = data.csrf || '';
    if (!start || (rangeDays && previousToday !== data.today)) {
      setDates(rangeDays || 1);
      if (page!=='camera' && (data.report.start !== start || data.report.end !== end)) {refreshAgain=true;return;}
    }
    $('date-start').max = data.today; $('date-end').max = data.today;
    $('request-error').hidden = true;
    render(data);
  } catch (error) {
    $('request-error').hidden=false;
    $('request-error').textContent='Não foi possível atualizar o painel: '+error.message+' Os dados exibidos podem estar desatualizados.';
  } finally {
    fetching = false;
    if (refreshAgain) {refreshAgain=false;setTimeout(refresh,0);}
  }
}

function render(data) {
  const d=data.device,r=data.report,c=d.counter;
  $('park-name').textContent=d.name; $('access-name').textContent=d.access;
  $('print-context').textContent=d.name+' / '+d.access;
  document.title='Monitoramento do parque | '+d.name;
  $('period-label').textContent=start===end?dateLabel(start):dateLabel(start)+' a '+dateLabel(end);
  $('export').href='/api/export.csv?'+query();
  $('notice').hidden=d.connected && c?.storage_ok && data.storage?.ok && data.storage?.clock_ok;
  $('notice').classList.toggle('warning',!$('notice').hidden);
  if (!d.configured) {
    $('notice-title').textContent='ESP indisponível';
    $('notice-description').textContent='Reconexão automática. Verifique a alimentação, o USB e a rede salva na placa.';
  } else if (!d.connected) {
    $('notice-title').textContent='ESP offline';
    $('notice-description').textContent=d.error || 'Reconexão automática em andamento. O histórico foi mantido.';
  } else if (!c?.storage_ok) {
    $('notice-title').textContent='Atenção à gravação dos totais na placa';
    $('notice-description').textContent='A ESP informou uma falha de armazenamento. Desligar a placa pode perder os totais ainda não gravados.';
  } else {
    $('notice-title').textContent='Horário da ESP indisponível';
    $('notice-description').textContent='Aguardando sincronização do horário para organizar os registros por dia.';
  }
  if (d.connected && !data.storage?.ok) {
    $('notice').hidden=false;$('notice').classList.add('warning');
    $('notice-title').textContent='Histórico sem gravação confirmada';
    $('notice-description').textContent=data.storage?.error || 'Verificando microSD. Não remova o cartão com a placa ligada.';
  }
  $('equipment-history').textContent=data.storage?.ok?'microSD, '+data.storage.capacity_mb+' MB, gravação ativa':data.storage?.error || 'Verificando microSD';
  $('cleanup-before').max=data.today;
  $('period-entries').textContent=r.has_samples?number(r.entries):'—';
  $('period-exits').textContent=r.has_samples?number(r.exits):'—';
  $('entries-caption').textContent=r.has_samples?'Entradas e saídas no período':'Sem coleta no período';
  $('collection-start').textContent=r.started_at?momentLabel(r.started_at):'Ainda não iniciado';
  $('last-seen').textContent=momentLabel(d.last_seen);
  $('chart-title').textContent=start===end?'Passagens por hora':'Passagens por dia';
  $('chart-note').textContent='Registros gravados pela ESP no microSD. Horários de Brasília.';
  drawChart(r);
  $('recent-history').replaceChildren(historyTable(r.history.filter(item=>item.kind==='movement').slice(0,5)));
  $('full-history').replaceChildren(historyTable(r.history));
  $('daily-history').innerHTML=r.daily.map(day=>`<tr><td>${escapeHTML(dateLabel(day.date))}</td><td class="number-entry">${day.samples?number(day.entries):'—'}</td><td class="number-exit">${day.samples?number(day.exits):'—'}</td><td class="table-muted">${!day.samples?'Sem registros':day.samples===1?'Primeira leitura':'Registros disponíveis'}</td></tr>`).join('');
  renderCameraCounts(data);
  $('equipment-connection').textContent=d.connected?'Recebendo dados':d.configured?'Sem conexão':'Não configurada';
  $('equipment-address').textContent=d.address || 'Ainda não informado';
  $('equipment-counter').textContent=c?(c.ready?'Ativa na última leitura':'Indisponível'):'Aguardando leitura';
  $('equipment-storage').textContent=c?(!c.storage_ok?'Falha informada pela ESP':c.pending?'Alterações aguardando gravação':'Totais gravados na última leitura'):'Aguardando leitura';
  $('apply-lines').disabled=!d.connected;
  if(c)showTracking(c);
  if (c && !calibrationDirty) {
    $('line-a').value=Math.round(c.first*100); $('line-b').value=Math.round(c.second*100);
    $('axis').value=c.horizontal?'y':'x'; $('reverse').checked=c.reverse;
    showDirection();
  }
  renderNetwork(data);
}

function renderCameraCounts(data){
  const r=data.report;
  const todayReport=r.start===data.today && r.end===data.today;
  const readable=data.device.connected && data.storage?.ok && data.storage?.clock_ok && todayReport;
  $('board-entries').textContent=readable && r.has_samples?number(r.entries):'—';
  $('board-exits').textContent=readable && r.has_samples?number(r.exits):'—';
  $('camera-count-note').textContent=!data.storage?.ok?'Dados do dia indisponíveis: falha no microSD.':
    !data.storage?.clock_ok?'Aguardando horário válido na ESP.':
    !todayReport || !data.device.connected?'Aguardando leitura dos dados de hoje.':
    !r.has_samples?'Ainda sem registros de hoje no microSD.':
    dateLabel(data.today)+' · Mesmo histórico do dia, salvo no microSD.';
}

function renderNetwork(data){
  const wifi=data.network?.wifi;
  $('wifi-current').textContent=wifi?.connected?'ESP conectada ao roteador por Wi-Fi':'Sem conexão confirmada';
  $('wifi-network').textContent=wifi?.ssid || '—';
  $('wifi-address').textContent=data.device.address || '—';
  $('wifi-signal').textContent=wifi?.connected?wifi.rssi+' dBm ('+(wifi.rssi>=-60?'Bom':wifi.rssi>=-75?'Regular':'Fraco')+')':'—';
  $('save-wifi').disabled=wifiSubmitting || !!wifi?.busy;
  $('mobile-link-panel').hidden=!accessToken;
  $('mobile-url').value=accessToken?location.origin+'/#'+accessToken:'';
}

let wifiSubmitting=false;
$('wifi-form').addEventListener('submit',async event=>{
  event.preventDefault();
  if(wifiSubmitting || state?.network?.wifi.busy)return;
  const body={ssid:$('wifi-ssid').value,password:$('wifi-password').value};
  if(new TextEncoder().encode(body.ssid).length>32 || new TextEncoder().encode(body.password).length>63){
    message('wifi-message','Nome e senha excedem o limite em bytes. Acentos ocupam mais de um byte.',true);return;
  }
  wifiSubmitting=true;$('save-wifi').disabled=true;
  message('wifi-message','Enviando a nova rede diretamente à ESP…');
  $('wifi-password').value='';
  try{
    await post('/api/wifi',body);
    message('wifi-message','Troca solicitada. Conecte este aparelho à nova rede. Se o endereço da ESP mudar ou a conexão falhar, consulte ou configure pelo cabo USB.');
  }catch(error){message('wifi-message','Não foi possível confirmar a troca: '+error.message+' Confira a rede salva pelo USB antes de tentar novamente.',true);}
  finally{body.password='';wifiSubmitting=false;$('save-wifi').disabled=false;}
});
$('copy-mobile').addEventListener('click',async()=>{
  try{
    if(navigator.clipboard?.writeText) await navigator.clipboard.writeText($('mobile-url').value);
    else {
      const input=$('mobile-url');input.type='text';input.select();
      if(!document.execCommand('copy'))throw Error('Cópia manual');
      input.type='password';
    }
    message('mobile-message','Link copiado. Abra no aparelho conectado ao mesmo Wi-Fi.');
  }
  catch{
    $('mobile-url').type='text';$('mobile-url').select();
    $('toggle-mobile-link').textContent='Ocultar link';$('toggle-mobile-link').setAttribute('aria-pressed','true');
    message('mobile-message','Selecione e copie o link acima.');
  }
});
$('toggle-mobile-link').addEventListener('click',()=>{
  const show=$('mobile-url').type==='password';
  $('mobile-url').type=show?'text':'password';
  $('toggle-mobile-link').textContent=show?'Ocultar link':'Mostrar link';
  $('toggle-mobile-link').setAttribute('aria-pressed',String(show));
});

let chartSelection = -1, chartRange = '', chartBuckets = [], chartDetailVisible = false, chartRestoringFocus = false;
function drawChart(report) {
  const box = $('chart');
  const total = BigInt(report.entries || '0') + BigInt(report.exits || '0');
  const buckets = report.chart?.length ? report.chart : Array.from({length: 24}, (_, i) => ({
    label: String(i).padStart(2, '0') + 'h', entries: '0', exits: '0', samples: 0
  }));
  chartBuckets = buckets;
  const range = start + '/' + end;
  if (range !== chartRange) { chartSelection = -1; chartRange = range; chartDetailVisible = false; }
  if (chartSelection >= buckets.length) chartSelection = -1;
  const width = Math.max(200, box.clientWidth || 800), height = box.clientHeight || 340;
  const left = width < 440 ? 34 : 48, right = 10, top = 24, bottom = 40;
  const plotWidth = width - left - right, plotHeight = height - top - bottom;
  const rawMax = Math.max(...buckets.map(b => Math.max(Number(b.entries), Number(b.exits))), 1);
  const magnitude = Math.max(1, 10 ** Math.floor(Math.log10(rawMax / 4)));
  const tick = Math.max(1, Math.ceil(rawMax / 4 / magnitude) * magnitude), max = tick * 4;
  let html = `<svg viewBox="0 0 ${width} ${height}" aria-label="Gráfico de barras de entradas e saídas">`;
  for (let i = 0; i <= 4; i++) {
    const y = top + plotHeight * (1 - i / 4), value = Math.round(max * i / 4);
    html += `<line class="${i ? 'grid-line' : 'axis-line'}" x1="${left}" y1="${y}" x2="${width-right}" y2="${y}"/><text x="${left-12}" y="${y+4}" text-anchor="end">${escapeHTML(new Intl.NumberFormat('pt-BR', {notation: value>=10000 ? 'compact' : 'standard'}).format(value))}</text>`;
  }
  const step = plotWidth / buckets.length, bar = Math.max(1.6, Math.min(24, step * .30));
  const labelEvery = Math.max(1, Math.ceil(buckets.length / (width < 440 ? 5 : 12)));
  buckets.forEach((b, i) => {
    const cx = left + step * (i + .5);
    const text = b.samples ? b.label + ': ' + number(b.entries) + ' entradas, ' + number(b.exits) + ' saídas' : b.label + ': sem coleta';
    html += `<g class="bucket${i===chartSelection && chartDetailVisible ? ' active' : ''}" data-bucket="${i}" role="button" tabindex="${i===(chartSelection<0 ? 0 : chartSelection) ? 0 : -1}" aria-pressed="${i===chartSelection && chartDetailVisible}" aria-label="${escapeHTML(text)}"><title>${escapeHTML(text)}</title><rect class="bucket-hit" x="${left+step*i}" y="${top-4}" width="${step}" height="${plotHeight+13}" rx="6"/>`;
    for (const [key, style, offset] of [['entries', 'bar-entry', -bar-1], ['exits', 'bar-exit', 1]]) {
      const value = Number(b[key]), h = value / max * plotHeight;
      if (value) html += `<rect class="${style}" x="${cx+offset}" y="${top+plotHeight-h}" width="${bar}" height="${h}" rx="${Math.min(4,bar/2)}"/>`;
    }
    if (!b.samples) html += `<line class="no-sample" x1="${cx-2}" y1="${top+plotHeight+8}" x2="${cx+2}" y2="${top+plotHeight+8}"/>`;
    else if (!Number(b.entries) && !Number(b.exits)) html += `<circle class="zero-marker" cx="${cx}" cy="${top+plotHeight}" r="2.4"/>`;
    html += '</g>';
    if (i % labelEvery === 0 || i === buckets.length-1)
      html += `<text x="${cx}" y="${height-10}" text-anchor="middle">${escapeHTML(b.label)}</text>`;
  });
  html += '</svg>';
  if (!report.has_samples || !total) html += '<div class="chart-status"><strong>' +
    (!report.has_samples ? 'Sem dados neste período' : 'Nenhuma passagem no período') +
    '</strong><p>' + (!report.has_samples ? 'O histórico começa com os registros da ESP no microSD.' :
      'Os horários com coleta estão marcados no eixo. Traços abaixo do eixo indicam horários sem coleta.') + '</p></div>';
  // Keep focus during the regular 5-second refresh without moving a reader's focus.
  const focusedBucket = box.contains?.(document.activeElement) ?
    document.activeElement?.getAttribute('data-bucket') : null;
  box.innerHTML = html;
  if (chartSelection < 0 || !chartDetailVisible) $('chart-detail').hidden = true;
  box.setAttribute('aria-label', !report.has_samples ? 'Sem dados coletados no período' :
    `Passagens no período: ${number(report.entries)} entradas e ${number(report.exits)} saídas. Use as setas para consultar os horários. Traços abaixo do eixo indicam falta de coleta.`);
  if (chartSelection >= 0 && chartDetailVisible) showChartBucket(chartSelection, false);
  if (focusedBucket !== null) {
    chartRestoringFocus = true;
    try { box.querySelector?.('[data-bucket="'+focusedBucket+'"]')?.focus({preventScroll:true}); }
    finally { chartRestoringFocus = false; }
  }
}
function showChartBucket(index, focus = false) {
  const bucket = chartBuckets[index];
  if (!bucket) return;
  chartSelection = index;
  chartDetailVisible = true;
  const detail = $('chart-detail');
  detail.innerHTML = '<strong>' + escapeHTML(bucket.label) + '</strong><p>' +
    (bucket.samples ? number(bucket.entries) + ' entradas / ' + number(bucket.exits) + ' saídas' : 'Sem coleta neste horário') + '</p>';
  detail.hidden = false;
  $('chart').querySelectorAll('[data-bucket]').forEach(group => {
    const active = Number(group.dataset.bucket) === index;
    group.classList.toggle('active', active);
    group.setAttribute('aria-pressed', String(active));
    group.setAttribute('tabindex', active ? '0' : '-1');
    if (active && focus) group.focus({preventScroll:true});
  });
}
function hideChartBucket() {
  chartDetailVisible = false;
  $('chart-detail').hidden = true;
  $('chart').querySelectorAll('[data-bucket]').forEach(group => {
    group.classList.remove('active');
    group.setAttribute('aria-pressed', 'false');
  });
}
$('chart').addEventListener('pointerleave', event => {
  if (event.pointerType !== 'touch') hideChartBucket();
});
$('chart').addEventListener('focusout', event => {
  if (!$('chart').contains?.(event.relatedTarget)) hideChartBucket();
});
document.addEventListener('pointerdown', event => {
  if (!$('chart').contains?.(event.target)) hideChartBucket();
});
$('chart').addEventListener('click', event => {
  const group = event.target.closest?.('[data-bucket]');
  if (group) showChartBucket(Number(group.dataset.bucket));
});
$('chart').addEventListener('pointerover', event => {
  if (event.pointerType === 'touch') return;
  const group = event.target.closest?.('[data-bucket]');
  if (group) showChartBucket(Number(group.dataset.bucket));
});
$('chart').addEventListener('focusin', event => {
  if (chartRestoringFocus) return;
  const group = event.target.closest?.('[data-bucket]');
  if (group) showChartBucket(Number(group.dataset.bucket));
});
$('chart').addEventListener('keydown', event => {
  const group = event.target.closest?.('[data-bucket]');
  if (!group) return;
  let index = Number(group.dataset.bucket);
  if (event.key === 'ArrowRight') index = Math.min(chartBuckets.length-1, index+1);
  else if (event.key === 'ArrowLeft') index = Math.max(0, index-1);
  else if (event.key === 'Home') index = 0;
  else if (event.key === 'End') index = chartBuckets.length-1;
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
  chartResizeTimer = setTimeout(() => { if (state && page === 'overview') drawChart(state.report); }, 120);
});
function historyTable(items) {
  items=items.filter(item=>item.kind==='movement');
  const wrapper=document.createElement('div');
  if(!items.length) {
    wrapper.className='empty-list';
    wrapper.innerHTML='<strong>Sem entradas ou saídas neste período.</strong>';
    return wrapper;
  }
  wrapper.className='table-scroll';
  wrapper.innerHTML='<table><thead><tr><th>Registrado em</th><th>Entradas</th><th>Saídas</th></tr></thead><tbody>'+items.map(item=> {
    return `<tr><td>${escapeHTML(momentLabel(item.at))}</td><td class="number-entry">${number(item.entries)}</td><td class="number-exit">${number(item.exits)}</td></tr>`;
  }).join('')+'</tbody></table>';
  return wrapper;
}

const eventText={1:'Novo trajeto antes de A',2:'Novo trajeto depois de B',3:'Pessoa detectada entre as linhas',4:'Entrada contada',5:'Saída contada',6:'Trajeto retomado',7:'Trajeto perdido no lado A',8:'Trajeto perdido no lado B',9:'Trajeto perdido sem lado inicial',10:'Travessia descartada: mais de 2 s sem detecção',11:'Limite de trajetos atingido',12:'Totais zerados'};
function showTracking(c){
  $('tracking-metrics').textContent=`Desde a inicialização: ${c.created ?? 0} trajetos, ${c.resumed ?? 0} retomadas, ${c.expired ?? 0} expirados, ${c.discarded ?? 0} travessias descartadas.`;
  $('tracking-events').replaceChildren(...[...(c.events || [])].reverse().map(event=>{const li=document.createElement('li');li.textContent=(event.ms/1000).toFixed(1)+' s: '+(event.id?'ID '+event.id+', ':'')+(eventText[event.code] || 'Evento desconhecido');return li;}));
}
$('edit-identification').addEventListener('click',()=> {
  $('setting-name').value=state?.device.name || 'Parque';
  $('setting-access').value=state?.device.access || 'Acesso principal';
  $('identification-message').textContent='';
  dialog.showModal();
});
$('close-identification').addEventListener('click',()=>dialog.close());
dialog.addEventListener('click',event=>{if(event.target===dialog){const rect=dialog.getBoundingClientRect();if(event.clientX<rect.left||event.clientX>rect.right||event.clientY<rect.top||event.clientY>rect.bottom)dialog.close();}});
function message(id,text,error=false){$(id).textContent=text;$(id).classList.toggle('error',error);}
$('identification-form').addEventListener('submit',async event=> {
  event.preventDefault();
  $('save-identification').disabled=true;
  message('identification-message','Salvando…');
  try {
    await post('/api/settings',{name:$('setting-name').value,access:$('setting-access').value});
    await refresh(); dialog.close();
  } catch(error){message('identification-message',error.message,true);}
  finally{$('save-identification').disabled=false;}
});
$('print-report').addEventListener('click',()=>window.print());

let cleanupPlan = null, cleanupBusy = false;
function invalidateCleanup(){
  cleanupPlan=null;
  $('cleanup-confirm').value='';
  $('cleanup-confirm-panel').hidden=true;
  $('cleanup-preview').textContent='';
  $('delete-history').disabled=true;
}
$('cleanup-before').addEventListener('input',invalidateCleanup);
$('cleanup-confirm').addEventListener('input',()=>{
  $('delete-history').disabled=cleanupBusy || !cleanupPlan || $('cleanup-confirm').value!=='APAGAR';
});
$('cleanup-form').addEventListener('submit',async event=>{
  event.preventDefault();if(cleanupBusy)return;
  invalidateCleanup();cleanupBusy=true;$('preview-cleanup').disabled=true;
  message('cleanup-message','Consultando o microSD…');
  try{
    const result=await post('/api/history/cleanup',{action:'preview',before:$('cleanup-before').value});
    $('cleanup-preview').textContent=result.files+' arquivo(s), '+number(result.bytes)+' bytes. Somente datas anteriores a '+dateLabel(result.before)+'.';
    if(result.files>0){cleanupPlan=result;$('cleanup-confirm-panel').hidden=false;}
    message('cleanup-message',result.files?'Confira a seleção. A exclusão não pode ser desfeita.':'Nenhum arquivo elegível. Nada será apagado.');
  }catch(error){message('cleanup-message',error.message,true);}
  finally{cleanupBusy=false;$('preview-cleanup').disabled=false;}
});
$('delete-history').addEventListener('click',async()=>{
  if(cleanupBusy || !cleanupPlan || $('cleanup-confirm').value!=='APAGAR')return;
  const selection=cleanupPlan;cleanupBusy=true;$('delete-history').disabled=true;$('preview-cleanup').disabled=true;$('cleanup-before').disabled=true;
  try{
    const result=await post('/api/history/cleanup',{action:'delete',before:selection.before,plan:selection.plan,confirm:'APAGAR'});
    invalidateCleanup();message('cleanup-message',result.removed+' arquivo(s) apagado(s). Registros de hoje preservados.');
    requestGeneration++;await refresh();
  }catch(error){invalidateCleanup();message('cleanup-message',error.message+' Consulte os arquivos novamente antes de tentar outra vez.',true);}
  finally{cleanupBusy=false;$('preview-cleanup').disabled=false;$('cleanup-before').disabled=false;}
});

function showDirection(){
  const reverse=$('reverse').checked,vertical=$('axis').value==='x';
  $('direction').textContent='Entrada: '+(reverse?'B para A':'A para B')+', '+(vertical?(reverse?'direita para esquerda':'esquerda para direita'):(reverse?'baixo para cima':'cima para baixo'))+'. Saída no sentido contrário.';
}
$('calibration-form').addEventListener('input',()=>{calibrationDirty=true;showDirection();});
$('calibration-form').addEventListener('submit',async event=> {
  event.preventDefault(); $('apply-lines').disabled=true;
  message('calibration-status','Aguardando confirmação da ESP…');
  try {
    await post('/api/calibration',{first:Number($('line-a').value)/100,second:Number($('line-b').value)/100,horizontal:$('axis').value==='y',reverse:$('reverse').checked});
    calibrationDirty=false; message('calibration-status','Linhas aplicadas. Os totais foram mantidos.'); await refresh();
  } catch(error){message('calibration-status',error.message,true);}
  finally{$('apply-lines').disabled=!state?.device.connected;}
});

function scheduleCamera(delay=350){clearTimeout(cameraTimer);if(page==='camera'&&!cameraPaused&&!document.hidden)cameraTimer=setTimeout(pollCamera,delay);}
async function pollCamera() {
  if(page!=='camera'||cameraPaused||cameraDrawing||document.hidden)return;
  if(!state?.device.configured){$('camera-status').textContent='Aguardando a ESP. Reconexão automática.';scheduleCamera(1500);return;}
  cameraDrawing=true;
  const epoch=counterEpoch;
  try {
    const response=await apiFetch('/api/frame',{signal:AbortSignal.timeout(9000)});
    const data=await response.json();if(!response.ok)throw Error(data.error || 'A câmera não respondeu.');
    if(page!=='camera'||cameraPaused||epoch!==counterEpoch)return;
    if(data.frame){lastFrame=data.frame;await drawFrame(lastFrame,epoch);if(epoch!==counterEpoch)return;$('people-now').textContent='Pessoas no quadro: '+data.frame.people;}
    $('camera-status').textContent=data.age>6?'Imagem desatualizada. Verifique a ESP.':data.frame?'Imagem ao vivo, '+data.frame.width+' × '+data.frame.height+' px.':'Aguardando imagem…';
  } catch(error){$('camera-status').textContent=error.message+' A última imagem pode estar desatualizada.';}
  finally{cameraDrawing=false;scheduleCamera();}
}
async function drawFrame(frame,epoch=counterEpoch){
  const image=new Image();image.src='data:image/jpeg;base64,'+frame.jpeg;
  await image.decode();if(page!=='camera'||cameraPaused||epoch!==counterEpoch)return;
  canvas.width=frame.width;canvas.height=frame.height;ctx.drawImage(image,0,0);
  $('camera-placeholder').hidden=true;
  const trackText={1:'Lado A',2:'Lado B',3:'Sem lado inicial',4:'Rastreando',5:'Entrada contada',6:'Saída contada'};
  $('tracks').textContent=frame.boxes.length?frame.boxes.map(b=>b[5]?'ID '+b[5]+': '+(trackText[b[6]] || 'Rastreando'):'Detecção sem trajeto').join('; '):'Nenhuma pessoa neste quadro.';
  const counter=frame.counter;
  if(counter){
    ctx.lineWidth=2;ctx.font='13px "Segoe UI"';
    for(const [label,value,color] of [['A',counter.first,'#65d9ff'],['B',counter.second,'#ffb788']]){
      ctx.strokeStyle=ctx.fillStyle=color;ctx.beginPath();
      if(counter.horizontal){const y=value*canvas.height;ctx.moveTo(0,y);ctx.lineTo(canvas.width,y);ctx.fillText(label,6,Math.max(14,y-4));}
      else{const x=value*canvas.width;ctx.moveTo(x,0);ctx.lineTo(x,canvas.height);ctx.fillText(label,x+4,16);}
      ctx.stroke();
    }
  }
  if($('show-boxes').checked){
    ctx.lineWidth=2;ctx.font='12px "Segoe UI"';
    for(const [x1,y1,x2,y2,score,id,trackState] of frame.boxes){
      ctx.strokeStyle=trackState===5||trackState===6?'#77f2a0':'#ffdb52';
      ctx.strokeRect(x1,y1,x2-x1,y2-y1);ctx.fillStyle='white';ctx.beginPath();ctx.arc((x1+x2)/2,(y1+y2)/2,3,0,Math.PI*2);ctx.fill();
      const label=(id?'ID '+id:'Pessoa')+' '+Math.round(score*100)+'%';
      const x=Math.max(0,Math.min(x1,canvas.width-ctx.measureText(label).width-8)),y=Math.max(0,y1-18);
      ctx.fillStyle='#233b30';ctx.fillRect(x,y,ctx.measureText(label).width+8,18);ctx.fillStyle='white';ctx.fillText(label,x+4,y+13);
    }
  }
}
$('pause-camera').addEventListener('click',()=>{cameraPaused=!cameraPaused;$('pause-camera').textContent=cameraPaused?'Retomar imagem':'Pausar imagem';if(cameraPaused){clearTimeout(cameraTimer);$('camera-status').textContent='Imagem pausada. A contagem continua.';}else scheduleCamera(0);});
$('show-boxes').addEventListener('change',()=>{if(lastFrame&&!cameraPaused)drawFrame(lastFrame).catch(()=>{});});
document.addEventListener('visibilitychange',()=>{if(!document.hidden){refresh();scheduleCamera(0);}else clearTimeout(cameraTimer);});
$('export').addEventListener('click',async event=>{
  event.preventDefault();
  try {
    const response=await apiFetch('/api/export.csv?'+query(),{signal:AbortSignal.timeout(20000)});
    if(!response.ok)throw Error('Não foi possível exportar o período.');
    const url=URL.createObjectURL(await response.blob()),link=document.createElement('a');
    link.href=url;link.download='visitacao-'+start+'_'+end+'.csv';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  }catch(error){$('request-error').hidden=false;$('request-error').textContent=error.message;}
});
async function startDashboard(){
  if(!accessToken){
    $('request-error').hidden=false;
    $('request-error').textContent='Abra o link completo da ESP, com a chave de acesso. Para obtê-lo pelo USB, execute iniciar.ps1 na pasta dashboard.';
    return;
  }
  await post('/api/time',{epoch:Math.floor(Date.now()/1000)}).catch(()=>{});
  await refresh();
  setInterval(()=>{if(!document.hidden)refresh();},5000);
}
startDashboard().catch(()=>{$('request-error').hidden=false;$('request-error').textContent='A ESP não respondeu. Confira a alimentação, o Wi-Fi e o link completo da placa.';});

