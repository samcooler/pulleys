'use strict';
const $ = id => document.getElementById(id);
const fmt = g => String(Math.round(g * 1000) / 1000);
let data = null, busy = false, timers = {};
const off = new Set();   // cues unticked; everything is ticked by default

async function api(method, path, body) {
  const res = await fetch('/api/gain' + path, {method, cache: 'no-store', body: body && JSON.stringify(body)});
  const out = await res.json();
  if (!res.ok) throw new Error(out.error || res.status);
  return out;
}
function cell(content, cls) {
  const td = document.createElement('td');
  if (cls) td.className = cls;
  if (content instanceof Node) td.append(content); else td.textContent = content;
  return td;
}
function button(label, fn, cls = 'button small') {
  const b = document.createElement('button'); b.className = cls; b.textContent = label; b.onclick = fn; return b;
}
function fail(e) { $('connection').textContent = 'Error'; $('status').textContent = String(e.message || e); }

function send(cue, value) {   // slider drags are sent after a short pause
  clearTimeout(timers[cue]);
  timers[cue] = setTimeout(() => api('POST', '/' + encodeURIComponent(cue), {gain: value}).then(refresh).catch(fail), 150);
}
function row(c) {
  const tr = document.createElement('tr'); if (c.overridden) tr.className = 'gain-over';
  const slider = Object.assign(document.createElement('input'), {type: 'range', min: 0, max: 3, step: 0.01, value: Math.min(c.gain, 3)});
  const num = Object.assign(document.createElement('input'), {type: 'number', min: 0, max: 10, step: 0.01, value: fmt(c.gain)});
  slider.setAttribute('aria-label', `Gain for ${c.cue}`); num.setAttribute('aria-label', `Gain for ${c.cue}, number`);
  slider.oninput = () => { num.value = fmt(+slider.value); send(c.cue, +slider.value); };
  num.onchange = () => { if (num.value === '' || isNaN(+num.value)) return refresh(); slider.value = Math.min(+num.value, 3); send(c.cue, +num.value); };
  const label = c.overridden ? 'overridden' : '';
  const box = Object.assign(document.createElement('input'), {type: 'checkbox', checked: !off.has(c.cue)});
  box.setAttribute('aria-label', `Select ${c.cue}`);
  box.onchange = () => { box.checked ? off.delete(c.cue) : off.add(c.cue); summary(); };
  tr.append(cell(box), cell(c.cue, 'cue'),
    cell(button('▶ Play', () => fetch('/cue/' + encodeURIComponent(c.cue), {method: 'POST'}).then(r => { if (!r.ok) throw new Error('did not play (see the log)'); }).catch(fail))),
    cell(waveCell(c), 'wavecell'),
    cell(c.group), cell(c.in_yaml ? fmt(c.yaml) : '1 (unset)'), cell(slider), cell(num),
    cell(c.overridden ? button('Reset', () => api('DELETE', '/' + encodeURIComponent(c.cue)).then(refresh).catch(fail)) : label));
  if (c.group === 'unlisted') tr.title = 'Not in the YAML cues list';
  return tr;
}
// --- waveforms: the browser decodes each cue's file and keeps the peaks -----------
const BINS = 120, peaks = new Map(), loading = new Map();   // cue -> {peaks, duration, name, count} | {failed}
let active = 0; const waiting = [];
function fetchPeaks(cue) {
  if (peaks.has(cue)) return Promise.resolve(peaks.get(cue));
  if (loading.has(cue)) return loading.get(cue);
  const job = new Promise(resolve => waiting.push([cue, resolve]));
  loading.set(cue, job); pump();
  return job;
}
function pump() {   // at most three decodes at a time
  while (active < 3 && waiting.length) {
    const [cue, resolve] = waiting.shift(); active++;
    decode(cue).catch(() => ({failed: true})).then(info => { peaks.set(cue, info); loading.delete(cue); active--; resolve(info); pump(); });
  }
}
async function decode(cue) {
  const res = await fetch('/api/gain/media/' + encodeURIComponent(cue), {cache: 'force-cache'});
  if (!res.ok) throw new Error(res.status);
  const ctx = new (window.OfflineAudioContext || window.webkitOfflineAudioContext)(1, 1, 44100);
  const buf = await ctx.decodeAudioData(await res.arrayBuffer());
  const out = new Float32Array(BINS), per = Math.max(1, Math.floor(buf.length / BINS));
  for (let ch = 0; ch < buf.numberOfChannels; ch++) {
    const d = buf.getChannelData(ch);
    for (let b = 0; b < BINS; b++) {
      let m = out[b];
      for (let i = b * per, end = Math.min(d.length, i + per); i < end; i++) { const v = Math.abs(d[i]); if (v > m) m = v; }
      out[b] = m;
    }
  }
  return {peaks: out, duration: buf.duration, name: res.headers.get('X-Media-Name'), count: +res.headers.get('X-Media-Count') || 1};
}
const seconds = s => s < 10 ? s.toFixed(1) + 's' : s < 60 ? Math.round(s) + 's' : `${Math.floor(s / 60)}:${String(Math.round(s % 60)).padStart(2, '0')}`;
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
function draw(canvas, info, gain) {   // mirrored bars; height is peak × gain, red where that passes full scale
  const w = canvas.clientWidth || 120, h = canvas.clientHeight || 28, dpr = window.devicePixelRatio || 1;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const g = canvas.getContext('2d'); g.scale(dpr, dpr);
  const mid = h / 2, slot = w / BINS, bar = Math.max(1, slot - 1);
  g.fillStyle = css('--wave-mid'); g.fillRect(0, mid - 0.5, w, 1);
  info.peaks.forEach((p, i) => {
    const v = p * gain, half = Math.max(0.5, Math.min(1, v) * (mid - 1));
    g.fillStyle = css(v > 1 ? '--wave-clip' : '--wave');
    g.fillRect(i * slot, mid - half, bar, half * 2);
  });
}
const seen = new IntersectionObserver(entries => {   // decode a row only once it scrolls into view
  for (const e of entries) if (e.isIntersecting) { seen.unobserve(e.target); e.target._load(); }
}, {rootMargin: '200px'});
function waveCell(c) {
  const box = document.createElement('div'); box.className = 'wave';
  const dur = document.createElement('span'); dur.className = 'dur'; dur.textContent = '…';
  const canvas = document.createElement('canvas'); canvas.setAttribute('role', 'img');
  const show = info => {
    if (info.failed) { dur.textContent = '—'; box.title = 'This browser could not read the sound from the file'; canvas.setAttribute('aria-label', 'No waveform'); return; }
    dur.textContent = seconds(info.duration);
    const clipped = info.peaks.some(p => p * c.gain > 1);
    box.title = `${info.name}${info.count > 1 ? ` (first of ${info.count})` : ''}, ${info.duration.toFixed(2)} s, peak ${Math.round(Math.max(...info.peaks) * c.gain * 100)}% at gain ${fmt(c.gain)}${clipped ? ', clips' : ''}`;
    canvas.setAttribute('aria-label', `Waveform, ${seconds(info.duration)}${clipped ? ', clipping' : ''}`);
    draw(canvas, info, c.gain);
  };
  box._load = () => fetchPeaks(c.cue).then(show);
  if (peaks.has(c.cue)) show(peaks.get(c.cue)); else seen.observe(box);
  box.append(dur, canvas);
  return box;
}

function render() {
  const n = data.overrides, q = $('filter').value.trim().toLowerCase();
  $('headline').textContent = n ? `${n} overridden` : 'Cue loudness';
  $('status').textContent = `${data.cues.length} cues. Highlighted rows differ from the YAML. 1 is the file's own level.`;
  $('reset-all').disabled = !n;
  $('yaml-box').textContent = data.yaml;
  const body = $('rows'); body.replaceChildren();
  const shown = data.cues.filter(c => !q || c.cue.toLowerCase().includes(q));
  if (!shown.length) { const tr = document.createElement('tr'); const td = cell('No cues.'); td.colSpan = 9; tr.append(td); body.append(tr); }
  for (const c of shown) body.append(row(c));
  summary();
}
const visible = () => { const q = $('filter').value.trim().toLowerCase(); return data ? data.cues.filter(c => !q || c.cue.toLowerCase().includes(q)) : []; };
const selected = () => visible().filter(c => !off.has(c.cue));
function summary() {   // the count, and the header box (ticked, empty or mixed)
  const v = visible(), n = selected().length, all = $('check-all');
  $('adjust-count').textContent = n;
  syncAdjust();
  all.checked = v.length > 0 && n === v.length;
  all.indeterminate = n > 0 && n < v.length;
}
function pick(fn) {   // change the ticks of the cues in view only, then redraw
  for (const c of visible()) fn(c.cue) ? off.delete(c.cue) : off.add(c.cue);
  render();
}
$('sel-all').onclick = () => pick(() => true);
$('sel-none').onclick = () => pick(() => false);
$('sel-inv').onclick = () => pick(cue => off.has(cue));
$('check-all').onchange = () => pick(() => $('check-all').checked);
async function refresh() {
  if (busy) return;
  busy = true;
  try { data = await api('GET', ''); render(); $('connection').textContent = 'Connected'; }
  catch (e) { fail(e); }
  finally { busy = false; }
  syncMaster();
}
function sendSelected(value) {   // one value for every selected cue, after a short pause
  clearTimeout(timers.all);
  timers.all = setTimeout(() => {
    const gains = Object.fromEntries(selected().map(c => [c.cue, value]));
    if (Object.keys(gains).length) api('POST', '', {gains}).then(refresh).catch(fail);
  }, 150);
}
$('adjust').oninput = () => { $('adjust-num').value = fmt(+$('adjust').value); sendSelected(+$('adjust').value); };
$('adjust-num').onchange = () => {
  const v = +$('adjust-num').value;
  if ($('adjust-num').value === '' || isNaN(v)) return syncAdjust();
  $('adjust').value = Math.min(v, 3); sendSelected(v);
};
function syncAdjust() {   // show the selected cues' gain if they agree, else "mixed"
  const a = $('adjust'), n = $('adjust-num');
  if (document.activeElement === a || document.activeElement === n) return;   // mid-edit
  const gains = [...new Set(selected().map(c => c.gain))];
  a.disabled = n.disabled = !gains.length;
  if (gains.length === 1) { a.value = Math.min(gains[0], 3); n.value = fmt(gains[0]); n.placeholder = ''; }
  else { a.value = 1; n.value = ''; n.placeholder = gains.length ? 'mixed' : ''; }
  $('adjust-note').textContent = gains.length > 1 ? 'They differ: setting a value applies it to all of them.' : '';
}
const master = $('master');
master.oninput = () => {
  $('master-out').textContent = master.value + '%';
  clearTimeout(timers.master);
  timers.master = setTimeout(() => fetch('/volume/' + master.value, {method: 'POST'}).then(r => { if (!r.ok) throw new Error('volume not set'); }).catch(fail), 150);
};
async function syncMaster() {
  if (document.activeElement === master) return;
  try { const v = (await (await fetch('/volume', {cache: 'no-store'})).json()).volume; master.value = v; $('master-out').textContent = v + '%'; }
  catch (e) { /* the connection indicator already says so */ }
}
$('filter').oninput = () => data && render();
$('reset-all').onclick = () => { if (confirm('Reset every override to the YAML?')) api('DELETE', '').then(refresh).catch(fail); };
$('copy').onclick = async () => {
  try { await navigator.clipboard.writeText(data.yaml); $('copy').textContent = 'Copied'; }
  catch { const r = document.createRange(); r.selectNodeContents($('yaml-box')); getSelection().removeAllRanges(); getSelection().addRange(r); $('copy').textContent = 'Selected: press copy'; }
  setTimeout(() => { $('copy').textContent = 'Copy audio_gain YAML'; }, 2000);
};
refresh();
