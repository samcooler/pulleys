'use strict';
const $ = id => document.getElementById(id);
const fmt = g => String(Math.round(g * 1000) / 1000);
const STEP = 0.05, BINS = 120;
let data = null, busy = false, timers = {}, waves = {}, pending = 0;
const off = new Set();      // cues unticked; everything is ticked by default
const drawers = [];         // one function per row on screen that (re)draws its waveform

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
function el(tag, props = {}, ...kids) { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; }
function button(label, fn, cls = 'button small') { return el('button', {className: cls, textContent: label, onclick: fn}); }
function fail(e) { $('connection').textContent = 'Error'; $('status').textContent = String(e.message || e); }
const clamp = v => Math.max(0, Math.min(10, Math.round(v * 1000) / 1000));

// --- one value, one slider with -/+ and a number box; used by every row and by "selected" ----
function gainControl(label, value, onSet) {
  const slider = el('input', {type: 'range', min: 0, max: 3, step: 0.01, value: Math.min(value, 3)});
  const num = el('input', {type: 'number', min: 0, max: 10, step: 0.01, value: fmt(value)});
  num.setAttribute('inputmode', 'decimal');
  slider.setAttribute('aria-label', `Gain for ${label}`); num.setAttribute('aria-label', `Gain for ${label}, number`);
  const set = v => { v = clamp(v); num.value = fmt(v); slider.value = Math.min(v, 3); onSet(v); };
  slider.oninput = () => set(+slider.value);
  num.onchange = () => num.value === '' || isNaN(+num.value) ? refresh() : set(+num.value);
  const step = (sign, word) => {
    const b = button(sign < 0 ? '−' : '+', () => set((+num.value || 0) + sign * STEP), 'button step');
    b.setAttribute('aria-label', `${word} ${label} by ${STEP}`); return b;
  };
  return el('span', {className: 'gainctl'}, step(-1, 'Lower'), slider, step(1, 'Raise'), num);
}

function send(cue, value) {   // slider drags are sent after a short pause
  clearTimeout(timers[cue]);
  timers[cue] = setTimeout(() => api('POST', '/' + encodeURIComponent(cue), {gain: value}).then(refresh).catch(fail), 150);
}
function play(cue) {
  return fetch('/cue/' + encodeURIComponent(cue), {method: 'POST'})
    .then(r => { if (!r.ok) throw new Error(`${cue} did not play (see the log)`); }).catch(fail);
}
function row(c) {
  const tr = el('tr', {className: [c.overridden && 'gain-over', c.missing && 'gain-missing'].filter(Boolean).join(' ')});
  const box = el('input', {type: 'checkbox', checked: !off.has(c.cue)});
  box.setAttribute('aria-label', `Select ${c.cue}`);
  box.onchange = () => { box.checked ? off.delete(c.cue) : off.add(c.cue); summary(); };
  const playBtn = button('▶ Play', () => play(c.cue), 'button small play');
  playBtn.disabled = c.missing;
  const meta = (c.in_yaml ? `YAML ${fmt(c.yaml)}` : 'YAML unset (1)') + ` · ${c.group}` + (c.missing ? ' · no media file' : '') + (c.overridden ? ' · overridden' : '');
  tr.append(cell(box, 'sel'), cell(c.cue, 'cue'), cell(playBtn, 'play'), cell(waveCell(c), 'wavecell'),
    cell(c.in_yaml ? fmt(c.yaml) : '1 (unset)', 'yaml'),
    cell(gainControl(c.cue, c.gain, v => send(c.cue, v)), 'gaincell'),
    cell(c.overridden ? button('Reset', () => api('DELETE', '/' + encodeURIComponent(c.cue)).then(refresh).catch(fail)) : '', 'reset'));
  tr.querySelector('td.yaml').classList.add('desk');
  tr.insertBefore(cell(meta, 'meta'), tr.querySelector('td.reset'));
  if (c.missing) tr.title = 'No media file for this cue in the media folders';
  else if (c.group === 'unlisted') tr.title = 'Not in the YAML cues list';
  return tr;
}

// --- waveforms: computed and cached by the Overlord; the page only draws them ------------------
const seconds = s => s < 10 ? s.toFixed(1) + 's' : s < 60 ? Math.round(s) + 's' : `${Math.floor(s / 60)}:${String(Math.round(s % 60)).padStart(2, '0')}`;
function draw(canvas, peaks, gain) {   // mirrored bars; height is peak × gain, red where that passes full scale
  const cs = getComputedStyle(document.documentElement), color = n => cs.getPropertyValue(n).trim();
  const w = canvas.clientWidth || 120, h = canvas.clientHeight || 28, dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  const g = canvas.getContext('2d'); g.scale(dpr, dpr);
  const mid = h / 2, slot = w / peaks.length, bar = Math.max(1, slot - 1);
  g.fillStyle = color('--wave-mid'); g.fillRect(0, mid - 0.5, w, 1);
  const normal = color('--wave'), clip = color('--wave-clip');
  peaks.forEach((p, i) => {
    const v = p * gain, half = Math.max(0.5, Math.min(1, v) * (mid - 1));
    g.fillStyle = v > 1 ? clip : normal;
    g.fillRect(i * slot, mid - half, bar, half * 2);
  });
}
function waveCell(c) {
  const box = el('div', {className: 'wave'});
  const dur = el('span', {className: 'dur', textContent: c.missing ? 'no file' : '…'});
  const canvas = el('canvas'); canvas.setAttribute('role', 'img');
  box.append(dur, canvas);
  if (c.missing) { canvas.hidden = true; return box; }
  drawers.push(() => {
    const w = waves[c.cue];
    if (!w) return;
    if (w.silent) { dur.textContent = 'no audio'; canvas.hidden = true; box.title = `${w.name} has no sound`; return; }
    const clipped = w.p.some(p => p * c.gain > 1);
    dur.textContent = seconds(w.d);
    box.title = `${w.name}${w.n > 1 ? ` (first of ${w.n})` : ''}, ${w.d.toFixed(2)} s, peak ${Math.round(Math.max(...w.p) * c.gain * 100)}% at gain ${fmt(c.gain)}${clipped ? ', clips' : ''}`;
    canvas.setAttribute('aria-label', `Waveform, ${seconds(w.d)}${clipped ? ', clipping' : ''}`);
    draw(canvas, w.p, c.gain);
  });
  return box;
}
async function loadWaves() {   // once, then again every couple of seconds while the Overlord is still drawing them
  try {
    const out = await (await fetch('/api/gain/waves', {cache: 'no-store'})).json();
    waves = out.waves || {}; pending = out.pending || 0;
    drawers.forEach(f => f());
    status();
  } catch (e) { pending = 0; }
  if (pending) setTimeout(loadWaves, 1500);
}

// --- the table ------------------------------------------------------------------------------------
const visible = () => { const q = $('filter').value.trim().toLowerCase(); return data ? data.cues.filter(c => !q || c.cue.toLowerCase().includes(q)) : []; };
const selected = () => visible().filter(c => !off.has(c.cue));
function status() {
  const missing = data ? data.cues.filter(c => c.missing).length : 0;
  $('status').textContent = data
    ? `${data.cues.length} cues${missing ? `, ${missing} with no media (last, in red)` : ''}. Highlighted rows differ from the YAML. 1 is the file's own level.`
      + (pending ? ` Drawing ${pending} waveform${pending > 1 ? 's' : ''}…` : '')
    : 'Loading…';
}
function render() {
  const n = data.overrides;
  $('headline').textContent = n ? `${n} overridden` : 'Cue loudness';
  status();
  $('reset-all').disabled = !n;
  $('yaml-box').textContent = data.yaml;
  const body = $('rows'); body.replaceChildren(); drawers.length = 0;
  const shown = visible();
  if (!shown.length) { const tr = el('tr'); const td = cell('No cues.'); td.colSpan = 7; tr.append(td); body.append(tr); }
  for (const c of shown) body.append(row(c));
  drawers.forEach(f => f());   // after they are in the page, so each canvas has its width
  summary();
}
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
$('filter').oninput = () => data && render();
async function refresh() {
  if (busy) return;
  busy = true;
  try { data = await api('GET', ''); $('cue-list').replaceChildren(...data.cues.map(c => el('option', {value: c.cue}))); render(); $('connection').textContent = 'Connected'; }
  catch (e) { fail(e); }
  finally { busy = false; }
  syncMaster();
}

// --- every selected cue at once ---------------------------------------------------------------------
function sendSelected(value) {   // one value for every selected cue, after a short pause
  clearTimeout(timers.all);
  timers.all = setTimeout(() => {
    const gains = Object.fromEntries(selected().map(c => [c.cue, value]));
    if (Object.keys(gains).length) api('POST', '', {gains}).then(refresh).catch(fail);
  }, 150);
}
const adjust = $('adjust'), adjustNum = $('adjust-num');
const setSelected = v => { v = clamp(v); adjustNum.value = fmt(v); adjust.value = Math.min(v, 3); sendSelected(v); };
adjust.oninput = () => setSelected(+adjust.value);
adjustNum.onchange = () => adjustNum.value === '' || isNaN(+adjustNum.value) ? syncAdjust() : setSelected(+adjustNum.value);
// -/+ step each cue from where it is, so a "mixed" group keeps its differences
const stepSelected = d => {
  const gains = Object.fromEntries(selected().map(c => [c.cue, clamp(c.gain + d)]));
  if (Object.keys(gains).length) api('POST', '', {gains}).then(refresh).catch(fail);
};
$('adjust-down').onclick = () => stepSelected(-STEP);
$('adjust-up').onclick = () => stepSelected(STEP);
function syncAdjust() {   // show the selected cues' gain if they agree, else "mixed"
  if (document.activeElement === adjust || document.activeElement === adjustNum) return;   // mid-edit
  const gains = [...new Set(selected().map(c => c.gain))];
  adjust.disabled = adjustNum.disabled = $('adjust-down').disabled = $('adjust-up').disabled = !gains.length;
  if (gains.length === 1) { adjust.value = Math.min(gains[0], 3); adjustNum.value = fmt(gains[0]); adjustNum.placeholder = ''; }
  else { adjust.value = 1; adjustNum.value = ''; adjustNum.placeholder = gains.length ? 'mixed' : ''; }
  $('adjust-note').textContent = gains.length > 1 ? 'They differ: the slider or number sets all of them to that value; − and + move each from where it is.' : '';
}

// --- master volume, copy, reset ----------------------------------------------------------------------
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
$('reset-all').onclick = () => { if (confirm('Reset every override to the YAML?')) api('DELETE', '').then(refresh).catch(fail); };
$('copy').onclick = async () => {
  try { await navigator.clipboard.writeText(data.yaml); $('copy').textContent = 'Copied'; }
  catch { const r = document.createRange(); r.selectNodeContents($('yaml-box')); getSelection().removeAllRanges(); getSelection().addRange(r); $('copy').textContent = 'Selected: press copy'; }
  setTimeout(() => { $('copy').textContent = 'Copy audio_gain YAML'; }, 2000);
};
let lastWidth = innerWidth;   // a rotated phone needs the waveforms redrawn at the new width
addEventListener('resize', () => { if (innerWidth === lastWidth) return; lastWidth = innerWidth; clearTimeout(timers.resize); timers.resize = setTimeout(() => data && render(), 200); });
refresh();
loadWaves();

$('cue-form').onsubmit = e => {   // send any cue by name, including ones for another Pi (player__cue)
  e.preventDefault();
  const name = $('cue-name').value.trim(), out = $('cue-result');
  if (!name) return;
  fetch('/cue/' + encodeURIComponent(name), {method: 'POST'})
    .then(r => { out.textContent = r.ok ? `sent ${name}` : `${name} did not play (see the log)`; })
    .catch(err => { out.textContent = String(err); });
};
