// The play log, live: /api/playlog?after=<n> every second, newest at the bottom.
(() => {
  const COLORS = ['red', 'blue', 'green', 'yellow', 'orange', 'purple'];
  const HEX = {red: '#ff3b30', blue: '#0a84ff', green: '#34c759', yellow: '#ffd60a', orange: '#ff9500', purple: '#bf5af2'};
  const MAX_ROWS = 3000;
  const log = document.getElementById('log');
  const followBtn = document.getElementById('follow');
  const conn = document.getElementById('connection');
  let seq = 0, follow = true, first = true;
  let state = null, stateAt = null, goals = [];
  const pulls = [];            // [t, colour] for the bars
  let clockSkew = 0;           // the Pi's clock minus ours (its clock may be off, with no internet)

  const show = {};
  document.querySelectorAll('[data-kind]').forEach(cb => {
    show[cb.dataset.kind] = cb.checked;
    cb.addEventListener('change', () => {
      show[cb.dataset.kind] = cb.checked;
      log.querySelectorAll(`.row[data-group="${cb.dataset.kind}"]`).forEach(r => r.classList.toggle('hidden', !cb.checked));
      if (follow) toBottom();
    });
  });

  const esc = s => String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const hhmmss = t => new Date(t * 1000).toLocaleTimeString([], {hour12: false});
  const sw = c => `<span class="sw ${esc(c)}"></span>`;
  const pretty = s => String(s).replace(/_/g, ' ');

  function stateClass(name) {
    if (name === 'victory2' || name === 'afterword') return 'win';
    if (name === 'party') return 'party';
    if (name === 'passive' || name === 'kickout') return 'idle';
    return '';
  }

  // One record -> [group, classes, icon, html], or null to skip it.
  function describe(r) {
    switch (r.kind) {
      case 'state':
        return ['state', `state ${stateClass(r.name)}`, '◆',
          `${esc(pretty(r.name).toUpperCase())}${r.prev ? ` <span style="opacity:.6;font-weight:400">from ${esc(pretty(r.prev))}</span>` : ''}${r.sync ? ' <span style="opacity:.6;font-weight:400">(caught up)</span>' : ''}`];
      case 'pull':
        return ['pull', `pull ${r.cls}`, sw(r.color),
          `<b>${esc(r.color)}</b> pull · channel ${r.ch}<span class="tag ${esc(r.cls)}">${r.cls === 'final' ? 'PURPLE' : r.cls === 'goal' ? 'GOAL' : 'NOT A GOAL'}</span>`];
      case 'cue': {
        const n = r.name || '';
        const lever = n.startsWith('audio_once_lever_');
        const janis = n.includes('janis');
        const video = n.startsWith('video_') || n.includes('__video');
        const icon = video ? '▶' : n.startsWith('text_') ? '✎' : n.startsWith('game_') ? '⚙' : '♪';
        return [lever ? 'lever' : 'cue', `cue ${video ? 'video' : ''} ${janis ? 'janis' : ''}`, icon,
          `${esc(n)}${r.enter ? '<span class="tag enter">ENTRY</span>' : ''}`];
      }
      case 'goals':
        return ['other', 'goals', '★',
          r.colors.length ? `Goals: ${r.colors.map(c => `<span class="chip">${sw(c)}${esc(c)}</span>`).join(' ')}` : 'Goals cleared'];
      case 'input': return ['other', 'input', '⚑', esc(pretty(r.name))];
      case 'count': return ['other', 'count', '#', `${esc(r.name)} = ${r.value}`];
      case 'param': return ['other', 'param', '⚙', `${esc(r.name)} = ${r.value}`];
      case 'start': return ['other', 'start', '↻', `game loaded: ${esc(r.game)}`];
      case 'palette': return ['other', 'palette', '◐', 'rope colours changed'];
      default: return null;
    }
  }

  function addRow(r) {
    const d = describe(r);
    if (!d) return;
    const [group, cls, icon, html] = d;
    const row = document.createElement('div');
    row.className = `row ${cls}${show[group] ? '' : ' hidden'}`;
    row.dataset.group = group;
    row.innerHTML = `<time>${hhmmss(r.t)}</time><span class="ic">${icon}</span><span class="msg">${html}</span>`;
    log.appendChild(row);
  }

  function track(r) {
    if (r.kind === 'state') { state = r.name; stateAt = r.t; }
    else if (r.kind === 'goals') goals = r.colors;
    else if (r.kind === 'pull') pulls.push([r.t, r.color]);
  }

  function renderNow() {
    const now = Date.now() / 1000 + clockSkew;
    document.getElementById('now-state').textContent = state ? pretty(state) : '—';
    if (stateAt) {
      const s = Math.max(0, Math.round(now - stateAt));
      document.getElementById('now-since').textContent = `for ${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
    }
    const g = document.getElementById('now-goals');
    g.innerHTML = goals.length ? goals.map(c => `<span class="chip">${sw(c)}${esc(c)}</span>`).join('') : 'none';
    while (pulls.length && pulls[0][0] < now - 60) pulls.shift();
    const counts = Object.fromEntries(COLORS.map(c => [c, 0]));
    pulls.forEach(([, c]) => { if (c in counts) counts[c]++; });
    const peak = Math.max(4, ...Object.values(counts));
    document.getElementById('now-bars').innerHTML = COLORS.map(c =>
      `<div class="bar"><i style="height:${Math.round(34 * counts[c] / peak) + 3}px;background:${HEX[c]}"></i><span>${counts[c]}</span></div>`).join('');
  }

  function toBottom() { log.scrollTop = log.scrollHeight; }
  function setFollow(on) {
    follow = on;
    followBtn.textContent = on ? 'Following' : 'Paused · jump to latest';
    followBtn.classList.toggle('primary', on);
    if (on) toBottom();
  }
  followBtn.addEventListener('click', () => setFollow(true));
  log.addEventListener('scroll', () => {
    const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
    if (atBottom !== follow) setFollow(atBottom);
  });

  async function poll() {
    try {
      const res = await fetch(`/api/playlog?after=${seq}&limit=${first ? 1500 : 500}`, {cache: 'no-store'});
      const body = await res.json();
      if (!res.ok) throw new Error(body.error || res.status);
      if (body.seq < seq) { seq = 0; log.innerHTML = ''; first = true; return poll(); }   // the app restarted
      if (first) log.innerHTML = body.records.length ? '' : '<div class="empty">Nothing played yet today.</div>';
      else if (body.records.length) log.querySelector('.empty')?.remove();
      body.records.forEach(r => { track(r); addRow(r); });
      if (body.now) clockSkew = body.now - Date.now() / 1000;
      while (log.children.length > MAX_ROWS) log.firstChild.remove();
      seq = body.seq;
      first = false;
      conn.className = 'connection';
      conn.innerHTML = '<span class="dot green"></span> Live';
      if (follow) toBottom();
    } catch (e) {
      conn.className = 'connection offline';
      conn.innerHTML = `<span class="dot"></span> ${esc(e.message || 'Offline')}`;
    }
  }

  setInterval(renderNow, 1000);
  setInterval(poll, 1000);
  poll().then(renderNow);
})();
