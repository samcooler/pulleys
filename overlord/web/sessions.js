'use strict';
const $ = id => document.getElementById(id);
const dur = s => {
  if (!Number.isFinite(s)) return '—';
  s = Math.round(s);
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), sec = s % 60;
  return h ? `${h}h ${String(m).padStart(2, '0')}m` : m ? `${m}m ${String(sec).padStart(2, '0')}s` : `${sec}s`;
};
const clock = t => new Date(t * 1000).toLocaleString([], {month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit', second: '2-digit'});
const avg = xs => xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : NaN;
const cell = (text, cls) => { const td = document.createElement('td'); td.textContent = text; if (cls) td.className = cls; return td; };

function render(data) {
  const rows = data.sessions, done = rows.filter(s => !s.live);
  $('headline').textContent = rows[0]?.live ? `Session ${rows[0].n} in progress` : 'Idle';
  $('status').textContent = data.state ? `Game state: ${data.state} (idle is ${data.idle_state})` : 'Waiting for the game to report its state.';
  $('t-count').textContent = data.total;
  $('t-dur').textContent = dur(avg(done.map(s => s.duration)));
  $('t-pulls').textContent = done.length ? avg(done.map(s => s.pulls)).toFixed(1) : '—';
  $('t-gap').textContent = dur(avg(rows.map(s => s.gap).filter(g => g !== null)));
  $('t-longest').textContent = done.length ? dur(Math.max(...done.map(s => s.duration))) : '—';
  $('t-max').textContent = rows.length ? furthest(rows, data.order || []) : '—';
  const body = $('rows');
  body.replaceChildren();
  if (!rows.length) { const tr = document.createElement('tr'); tr.append(cell('No sessions yet. One starts when the game leaves its idle state.')); tr.firstChild.colSpan = 8; body.append(tr); return; }
  for (const s of rows) {
    const tr = document.createElement('tr'); if (s.live) tr.className = 'sess-live';
    tr.append(cell(s.n), cell(clock(s.start)), cell(s.live ? 'in progress' : clock(s.end) + (s.why && s.why !== 'idle' ? ` (${s.why})` : '')),
      cell(dur(s.duration), 'num'), cell(s.gap === null ? '—' : dur(s.gap), 'num'), cell(s.pulls, 'num'), cell(s.max_phase),
      cell(s.phases.map(([name, secs]) => `${name} ${dur(secs)}`).join(' → '), 'phases'));
    body.append(tr);
  }
}
function furthest(rows, order) {
  const rank = name => order.indexOf(name);
  return rows.map(s => s.max_phase).reduce((a, b) => rank(b) > rank(a) ? b : a);
}

async function refresh() {
  try {
    const res = await fetch('/api/sessions', {cache: 'no-store'});
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || res.status);
    render(data);
    $('connection').textContent = 'Connected';
  } catch (e) {
    $('connection').textContent = 'Connection lost';
    $('status').textContent = String(e.message || e);
  }
}
refresh();
setInterval(refresh, 5000);
