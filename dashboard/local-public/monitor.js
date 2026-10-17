'use strict';
// Monitoring requests never request a frame or renew the PIR activity timer.
(() => {
  const root = $('field-monitor');
  const rows = [];
  let boot = null, lastSeq = 0, lastReading = null, lastSeen = 0;
  let timer, busy = false, modeDirty = false, saving = false, offline = false, available = false;
  const time = (value) => new Date(value).toLocaleTimeString('pt-BR');
  const seconds = (value) => Math.max(0, Math.ceil(Number(value || 0) / 1000));
  const reasons = {
    disabled: 'Modo contínuo: sono desativado',
    pir_active: 'PIR ativo',
    gpio_unavailable: 'Leitura do PIR indisponível',
    boot_grace: 'Estabilização inicial após ligar ou RESET',
    activity_window: 'Aguardando período sem atividade',
    camera_recovery: 'Tentativa limitada de recuperação da câmera',
    preparing: 'Salvando e preparando o sono',
    checkpoint_failed: 'Falha ao salvar os totais na memória interna',
    journal_failed: 'Falha na gravação do diário microSD',
    journal_busy: 'Gravação em andamento no microSD',
    waiting_clock: 'Aguardando horário válido',
    cloud_busy: 'Envio à nuvem ainda ocupado',
    wifi_stop_failed: 'Não foi possível desligar o Wi-Fi',
    wake_arm_or_new_activity: 'Novo movimento ou falha ao preparar o despertar',
    new_activity: 'Nova atividade',
    cloud_stop_timeout: 'Envio interrompido no limite; registros não confirmados serão reenviados',
  };
  const wake = { pir: 'Sensor PIR', timer: 'Temporizador', power_on: 'Placa ligada', reset: 'Reinício / RESET' };
  function append(text, at = Date.now(), uptime = null) {
    rows.push({ at, uptime, boot, text });
    if (rows.length > 200) rows.shift();
    const list = $('monitor-events');
    list.replaceChildren(...rows.slice().reverse().map((row) => {
      const li = document.createElement('li');
      const stamp = document.createElement('time');
      stamp.dateTime = new Date(row.at).toISOString();
      stamp.textContent = time(row.at);
      li.append(stamp, document.createTextNode(row.text));
      return li;
    }));
    $('monitor-download').disabled = false;
  }
  function eventText(event) {
    const text = {
      boot: 'Inicialização: ' + (wake[event.reason] || event.reason),
      pir_high: 'PIR ativo (1).', pir_low: 'PIR inativo (0).',
      pir_error: 'Falha ao ler o PIR.',
      first_frame: 'Primeiro quadro processado pela IA neste boot.',
      ai_people: 'IA detectou pessoa(s).', ai_empty: 'IA não detectou pessoas neste quadro.',
      sleep_prepare: 'Preparação do sono iniciada. A contagem segue ativa durante o envio.',
      sleep_cancelled: 'Sono cancelado: ' + (reasons[event.reason] || event.reason),
      sleep_warning: 'Aviso antes do sono: ' + (reasons[event.reason] || event.reason),
      cloud_attempt: 'Tentativa de envio à nuvem iniciada.',
      cloud_ack: 'Nuvem confirmou o recebimento do envio.',
      cloud_failed: event.reason === 'permanent'
        ? 'Envio rejeitado ou erro persistente. Registros preservados; confira a configuração.'
        : 'Envio sem confirmação. Registros preservados para nova tentativa.',
      camera_recovery: 'IA sem quadros recentes. Uma tentativa de recuperação foi iniciada.',
      camera_recovered: 'Recuperação da câmera confirmada por novo quadro da IA.',
      camera_failed: 'Câmera indisponível. A falha será informada e não bloqueará o sono.',
      mode_changed: 'Modo salvo: ' + (event.reason === 'pir' ? 'sensor e deep sleep.' : 'contínuo para ajustes.'),
    };
    return text[event.kind] || event.kind;
  }
  function render(p) {
    const m = p.monitor;
    available = !!m?.available;
    lastReading = p;
    lastSeen = Date.now();
    $('power-save').disabled = saving || p.preparing;
    if (!modeDirty) $('power-mode').value = p.enabled ? 'pir' : 'continuous';
    if (!available) {
      offline = true;
      lastReading = null;
      $('power-save').disabled = true;
      for (const id of ['monitor-pir', 'monitor-ai', 'monitor-wait', 'monitor-wake', 'monitor-rises', 'monitor-cycles']) $(id).textContent = '—';
      root.dataset.state = 'offline';
      $('monitor-connection').textContent = m ? 'Monitor ainda indisponível' : 'Firmware sem monitor';
      $('monitor-explanation').textContent = m
        ? 'A ESP respondeu, mas o gerenciamento de energia ainda não foi inicializado. Confira a inicialização e a gravação.'
        : 'A ESP respondeu, mas precisa receber a versão com monitor de campo.';
      $('monitor-proof').textContent = 'Sem confirmação de sono disponível nesta leitura. As leituras anteriores foram retiradas.';
      $('monitor-timing').textContent = 'Não foi possível consultar os tempos desta versão.';
      $('monitor-sync').textContent = 'Sem leitura do envio à nuvem.';
      $('monitor-updated').textContent = 'Resposta incompatível às ' + time(lastSeen);
      return;
    }
    if (offline) append('Comunicação com a ESP restabelecida.');
    offline = false;
    root.dataset.state = p.preparing ? 'preparing' : 'online';
    $('monitor-connection').textContent = p.preparing ? 'Preparando sono' : 'ESP acordada';
    $('monitor-explanation').textContent = 'PIR: sinal de movimento. IA: detecção de pessoas na imagem. Um não substitui o outro.';
    $('monitor-pir').textContent = p.pir_level === 1
      ? 'Ativo há ' + seconds(m.pir_high_ms) + ' s' : p.pir_level === 0 ? 'Inativo (0)' : 'Indisponível';
    $('monitor-ai').textContent = p.camera_fault ? 'Falha informada; contagem indisponível'
      : !p.camera_started ? 'Câmera aguardando acionamento'
      : p.frame_age_ms < 0 ? 'Aguardando primeiro quadro'
      : p.frame_age_ms >= 5000 ? 'Sem quadros recentes'
      : m.people_detected === 1 ? 'Pessoa(s) detectada(s)' : 'Ativa, sem pessoas';
    const block = reasons[p.sleep_block] || p.sleep_block;
    $('monitor-wait').textContent = block || 'Condições de repouso atendidas';
    if (p.sleep_block === 'activity_window') $('monitor-wait').textContent += ' (' + seconds(p.quiet_remaining_ms) + ' s)';
    if (p.sleep_block === 'boot_grace') $('monitor-wait').textContent += ' (' + seconds(p.boot_grace_remaining_ms) + ' s)';
    $('monitor-wake').textContent = wake[m.wake_source] || m.wake_source;
    $('monitor-rises').textContent = String(m.pir_rises);
    $('monitor-cycles').textContent = String(m.confirmed_cycles);
    $('monitor-proof').textContent = m.paired_sleep
      ? 'Ciclo confirmado: o chip voltou de deep sleep por ' + (wake[m.wake_source] || m.wake_source) + '. O contador de ciclos reinicia após desligamento ou reinício comum. Isto não mede o consumo elétrico.'
      : m.deep_sleep_wake ? 'O chip informou despertar de deep sleep, mas não há marcador anterior pareado nesta versão.'
      : 'Ainda sem ciclo de deep sleep confirmado desde o último início comum. Ausência de resposta, sozinha, não comprova sono.';
    $('monitor-timing').textContent = p.idle_seconds + ' s sem atividade; despertar periódico após ' + (p.sync_seconds / 3600) + ' h de sono. '
      + (p.timer_only ? 'Despertar periódico: sincroniza sem ligar a câmera e volta a dormir. ' : '')
      + (m.first_frame_ms ? 'Do boot ao primeiro quadro: ' + (m.first_frame_ms / 1000).toFixed(1) + ' s. ' : '')
      + 'A prévia e este monitor não impedem o sono.';
    const cloud = p.cloud || {};
    const outcomes = {
      drained: 'Fila de envio concluída.', send_failed: 'Falha de envio; registros ficam para o próximo ciclo.',
      budget_exhausted: 'Limite de rede atingido; restantes preservados.', wifi_unavailable: 'Wi-Fi indisponível; nova tentativa no próximo ciclo.',
      permanent_error: 'Envio exige revisão de credencial ou dados.', storage_unavailable: 'Cartão indisponível; histórico pode estar incompleto.',
      not_configured: 'Envio à nuvem ainda não configurado.',
    };
    $('monitor-sync').textContent = (cloud.syncing ? 'Enviando à nuvem. '
      : cloud.permanent_error ? 'Envio exige revisão. '
      : cloud.phase === 'retry_wait' ? 'Nova tentativa em ' + seconds(cloud.next_attempt_ms) + ' s. '
      : cloud.pending ? 'Há registros pendentes ou armazenamento sem confirmação. ' : 'Sem pendências na última verificação. ')
      + (p.preparing ? 'Orçamento restante de rede: ' + seconds(p.network_remaining_ms) + ' s. ' : '')
      + (outcomes[cloud.sleep_result] || '')
      + (p.storage_ok === false ? ' Aviso: microSD sem gravação confirmada; não impede o sono.' : '')
      + (p.clock_ok === false ? ' Horário inválido; passagens sem horário não são inventadas nem enviadas como datadas.' : '');
    $('monitor-updated').textContent = 'Atualizado às ' + time(lastSeen);
    if (boot !== m.boot_id) {
      boot = m.boot_id; lastSeq = 0;
      if (m.paired_sleep) append('Deep sleep confirmado ao retornar: despertar por ' + (wake[m.wake_source] || m.wake_source) + '.');
    }
    const events = m.events || [];
    if (lastSeq && events.length && events[0].seq > lastSeq + 1) append('Alguns eventos antigos saíram da memória da ESP antes da consulta.');
    for (const event of events) if (event.seq > lastSeq) {
      append(eventText(event), lastSeen - p.uptime_ms + event.ms, event.ms);
      lastSeq = event.seq;
    }
  }
  function unavailable(error) {
    available = false;
    root.dataset.state = 'offline';
    $('power-save').disabled = true;
    $('monitor-connection').textContent = 'Sem resposta da ESP';
    $('monitor-explanation').textContent = 'Pode ser sono, troca de IP, falta de energia ou perda do Wi-Fi. Aguardando reconexão; os valores abaixo são da última leitura.';
    $('monitor-updated').textContent = lastSeen ? 'Última resposta às ' + time(lastSeen) : 'Nenhuma leitura recebida';
    if (!offline) append('ESP sem resposta. Sono não confirmado: ' + error.message);
    offline = true;
  }
  async function poll() {
    if (busy || document.hidden || !accessToken) return;
    busy = true;
    try {
      const response = await apiFetch('/api/monitor', { signal: AbortSignal.timeout(3000) });
      if (!response.ok) throw Error(response.status === 401 ? 'Abra o link completo de acesso.' : 'Leitura indisponível.');
      render(await response.json());
    } catch (error) { unavailable(error); }
    finally { busy = false; clearTimeout(timer); if (!document.hidden) timer = setTimeout(poll, offline ? 3000 : 1500); }
  }
  $('monitor-test').addEventListener('click', () => {
    setCameraPaused(true);
    append('Acompanhamento sem vídeo iniciado. Contagens e tempos da ESP foram preservados.');
    poll();
  });
  $('power-mode').addEventListener('change', () => { modeDirty = true; });
  $('power-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    if (saving) return;
    saving = true; $('power-save').disabled = true;
    try {
      const p = await post('/api/monitor', { enabled: $('power-mode').value === 'pir' });
      modeDirty = false; render(p);
      message('power-message', p.enabled ? 'Modo com sensor salvo. Os tempos não foram alterados.' : 'Modo contínuo salvo. A ESP não entrará em deep sleep.');
    } catch (error) { message('power-message', error.message, true); }
    finally { saving = false; $('power-save').disabled = !available || offline || !!lastReading?.preparing; }
  });
  $('monitor-download').addEventListener('click', () => {
    const header = 'Monitor de campo — ' + new Date().toLocaleString('pt-BR') + '\n'
      + 'Horários do aparelho que abriu o site. Sem medição de corrente.\n'
      + (lastReading ? 'Tempos configurados: inatividade ' + lastReading.idle_seconds + ' s; temporizador ' + lastReading.sync_seconds + ' s.\n' : '');
    const body = rows.map((r) => new Date(r.at).toISOString() + ' | boot ' + r.boot
      + (r.uptime === null ? '' : ' | uptime ' + r.uptime + ' ms') + ' | ' + r.text).join('\n');
    const url = URL.createObjectURL(new Blob([header, body], { type: 'text/plain;charset=utf-8' }));
    const link = document.createElement('a');
    link.href = url; link.download = 'monitor-parque.txt'; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  });
  document.addEventListener('visibilitychange', () => {
    clearTimeout(timer);
    if (!document.hidden) poll();
  });
  if (accessToken) poll();
})();
