'use strict';

// Calendário do painel: datas civis em UTC evitam mudanças de dia por fuso/DST.
window.ParkDateRange = class ParkDateRange {
  constructor(today) {
    this.today = today;
    this.get = (id) => document.getElementById(id);
    this.field = 'start';
    this.get('range-start').addEventListener('click', () => this.selectField('start'));
    this.get('range-end').addEventListener('click', () => this.selectField('end'));
    this.get('range-prev').addEventListener('click', () => this.moveMonth(-1));
    this.get('range-next').addEventListener('click', () => this.moveMonth(1));
  }
  date(value) {
    return new Date(value + 'T12:00:00Z');
  }
  iso(date) {
    return date.toISOString().slice(0, 10);
  }
  open(start, end) {
    this.get('date-start').value = start;
    this.get('date-end').value = end;
    this.field = 'start';
    this.month = (start || this.today()).slice(0, 7) + '-01';
    this.get('date-form').hidden = false;
    this.get('custom-period').setAttribute('aria-expanded', 'true');
    this.render();
    this.get('range-start').focus();
  }
  close() {
    this.get('date-form').hidden = true;
    this.get('custom-period').setAttribute('aria-expanded', 'false');
  }
  selectField(field) {
    this.field = field;
    const value = this.get('date-' + field).value;
    if (value) this.month = value.slice(0, 7) + '-01';
    this.render();
  }
  moveMonth(delta) {
    const d = this.date(this.month);
    d.setUTCMonth(d.getUTCMonth() + delta);
    if (this.iso(d) > this.today()) return;
    this.month = this.iso(d);
    this.render();
  }
  choose(value) {
    if (value > this.today()) return;
    const a = this.get('date-start'),
      b = this.get('date-end');
    if (this.field === 'start' || !a.value) {
      a.value = value;
      b.value = '';
      this.field = 'end';
    } else {
      b.value = value;
      if (b.value < a.value) [a.value, b.value] = [b.value, a.value];
      this.field = 'start';
    }
    this.render(value);
  }
  key(event, value) {
    const offsets = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -7, ArrowDown: 7 };
    const d = this.date(value);
    if (event.key in offsets) d.setUTCDate(d.getUTCDate() + offsets[event.key]);
    else if (event.key === 'Home') d.setUTCDate(d.getUTCDate() - ((d.getUTCDay() + 6) % 7));
    else if (event.key === 'End') d.setUTCDate(d.getUTCDate() + 6 - ((d.getUTCDay() + 6) % 7));
    else if (event.key === 'PageUp' || event.key === 'PageDown') {
      const day = d.getUTCDate();
      d.setUTCDate(1);
      d.setUTCMonth(d.getUTCMonth() + (event.key === 'PageUp' ? -1 : 1));
      const last = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, 0)).getUTCDate();
      d.setUTCDate(Math.min(day, last));
    } else return;
    event.preventDefault();
    const next = this.iso(d);
    if (next > this.today()) return;
    this.month = next.slice(0, 7) + '-01';
    this.render(next);
  }
  render(focus) {
    const a = this.get('date-start').value,
      b = this.get('date-end').value;
    for (const field of ['start', 'end']) {
      const button = this.get('range-' + field),
        value = field === 'start' ? a : b;
      const label = value
        ? this.date(value).toLocaleDateString('pt-BR', { timeZone: 'UTC' })
        : 'Selecionar';
      button.replaceChildren();
      const caption = document.createElement('span'),
        selected = document.createElement('strong');
      caption.textContent = field === 'start' ? 'De' : 'Até';
      selected.textContent = label;
      button.append(caption, selected);
      button.setAttribute('aria-pressed', String(this.field === field));
    }
    this.get('range-month').textContent = this.date(this.month).toLocaleDateString('pt-BR', {
      month: 'long',
      year: 'numeric',
      timeZone: 'UTC',
    });
    this.get('range-next').disabled = this.month.slice(0, 7) === this.today().slice(0, 7);
    const grid = this.get('range-days');
    grid.replaceChildren();
    const first = this.date(this.month);
    first.setUTCDate(1 - ((first.getUTCDay() + 6) % 7));
    const selection = focus || (this.field === 'end' ? b || a : a) || this.today();
    const target = selection.slice(0, 7) === this.month.slice(0, 7) ? selection : this.month;
    let focused;
    for (let i = 0; i < 42; i++) {
      const d = new Date(first);
      d.setUTCDate(d.getUTCDate() + i);
      const value = this.iso(d),
        button = document.createElement('button');
      button.type = 'button';
      button.textContent = String(d.getUTCDate());
      button.disabled = value > this.today();
      const endpoint = value === a || value === b;
      button.className =
        'range-day' +
        (value.slice(0, 7) !== this.month.slice(0, 7) ? ' outside' : '') +
        (a && b && value > a && value < b ? ' between' : '') +
        (endpoint ? ' endpoint' : '');
      button.setAttribute(
        'aria-label',
        d.toLocaleDateString('pt-BR', { dateStyle: 'full', timeZone: 'UTC' }) +
          (value === a ? ', início' : '') +
          (value === b ? ', fim' : ''),
      );
      button.setAttribute('aria-pressed', String(endpoint));
      if (value === this.today()) button.setAttribute('aria-current', 'date');
      button.tabIndex = value === target ? 0 : -1;
      button.addEventListener('click', () => this.choose(value));
      button.addEventListener('keydown', (event) => this.key(event, value));
      grid.append(button);
      if (value === target) focused = button;
    }
    const valid = a && b && (this.date(b) - this.date(a)) / 86400000 <= 365;
    this.get('range-apply').disabled = !valid;
    this.get('range-hint').textContent = !b
      ? 'Selecione a data final.'
      : !valid
        ? 'Escolha um período de até 366 dias.'
        : 'Selecione o início e o fim do período.';
    if (focus && focused) focused.focus();
  }
};
