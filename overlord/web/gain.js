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
  tr.append(cell(box), cell(c.cue, 'cue'), cell(c.group), cell(c.in_yaml ? fmt(c.yaml) : '1 (unset)'), cell(slider), cell(num),
    cell(button('▶ Play', () => fetch('/cue/' + encodeURIComponent(c.cue), {method: 'POST'}).then(r => { if (!r.ok) throw new Error('did not play (see the log)'); }).catch(fail))),
    cell(c.overridden ? button('Reset', () => api('DELETE', '/' + encodeURIComponent(c.cue)).then(refresh).catch(fail)) : label));
  if (c.group === 'unlisted') tr.title = 'Not in the YAML cues list';
  return tr;
}
function render() {
  const n = data.overrides, q = $('filter').value.trim().toLowerCase();
  $('headline').textContent = n ? `${n} overridden` : 'Cue loudness';
  $('status').textContent = `${data.cues.length} cues. Highlighted rows differ from the YAML. 1 is the file's own level.`;
  $('reset-all').disabled = !n;
  $('yaml-box').textContent = data.yaml;
  const body = $('rows'); body.replaceChildren();
  const shown = data.cues.filter(c => !q || c.cue.toLowerCase().includes(q));
  if (!shown.length) { const tr = document.createElement('tr'); const td = cell('No cues.'); td.colSpan = 8; tr.append(td); body.append(tr); }
  for (const c of shown) body.append(row(c));
  summary();
}
const visible = () => { const q = $('filter').value.trim().toLowerCase(); return data ? data.cues.filter(c => !q || c.cue.toLowerCase().includes(q)) : []; };
const selected = () => visible().filter(c => !off.has(c.cue));
function summary() {   // the count, and the header box (ticked, empty or mixed)
  const v = visible(), n = selected().length, all = $('check-all');
  $('adjust-count').textContent = n;
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
}
const factor = () => Math.pow(4, +$('adjust').value);   // -1..1 on the slider is ×0.25..×4
$('adjust').oninput = () => { $('adjust-out').textContent = '×' + factor().toFixed(2); };
$('adjust').onchange = async () => {
  const f = factor(), gains = {};
  for (const c of selected()) gains[c.cue] = Math.min(10, Math.round(c.gain * f * 1000) / 1000);
  $('adjust').value = 0; $('adjust-out').textContent = '×1.00';
  if (f === 1 || !Object.keys(gains).length) return;
  try { await api('POST', '', {gains}); } catch (e) { fail(e); }
  refresh();
};
$('filter').oninput = () => data && render();
$('reset-all').onclick = () => { if (confirm('Reset every override to the YAML?')) api('DELETE', '').then(refresh).catch(fail); };
$('copy').onclick = async () => {
  try { await navigator.clipboard.writeText(data.yaml); $('copy').textContent = 'Copied'; }
  catch { const r = document.createRange(); r.selectNodeContents($('yaml-box')); getSelection().removeAllRanges(); getSelection().addRange(r); $('copy').textContent = 'Selected: press copy'; }
  setTimeout(() => { $('copy').textContent = 'Copy audio_gain YAML'; }, 2000);
};
refresh();
