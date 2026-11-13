'use strict';
// Lever debug: one button per rope channel. A click is one pull, sent at once and
// never waiting on the one before, so rapid clicks are rapid pulls.
(() => {
  const $ = id => document.getElementById(id);
  const NAMES = ['red', 'blue', 'green', 'yellow', 'orange', 'purple'];
  const counts = new Map();
  let sent = 0, failed = 0, lastError = '';

  function say() {
    $('lever-status').textContent = lastError ? `Last pull failed: ${lastError}`
      : sent ? `${sent} pull${sent === 1 ? '' : 's'} sent${failed ? `, ${failed} failed` : ''}.`
      : 'Ready. Colours follow the arbiter\'s current channel colours.';
  }
  function pull(channel, button) {
    sent++;
    counts.set(channel, (counts.get(channel) || 0) + 1);
    button.querySelector('.count').textContent = '×' + counts.get(channel);
    button.classList.add('hit'); setTimeout(() => button.classList.remove('hit'), 90);
    fetch('/lever/' + channel, {method: 'POST', cache: 'no-store'})
      .then(async r => { if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.status); lastError = ''; })
      .catch(e => { failed++; lastError = `channel ${channel}: ${e.message || e}`; })
      .finally(say);
  }
  const buttons = new Map();
  function build() {
    for (let ch = 1; ch <= 24; ch++) {
      const b = document.createElement('button');
      b.type = 'button'; b.className = 'lever';
      b.innerHTML = `<strong>CH ${ch}</strong><small>—</small><span class="swatch"></span><span class="count"></span>`;
      b.setAttribute('aria-label', `Pull channel ${ch}`);
      b.addEventListener('click', () => pull(ch, b));   // not pointerdown: keyboard and screen readers work too
      $('levers').append(b); buttons.set(ch, b);
    }
    $('lever-clear').onclick = () => { counts.clear(); sent = failed = 0; lastError = ''; buttons.forEach(b => { b.querySelector('.count').textContent = ''; }); say(); };
  }
  async function colours() {   // names and swatches follow the arbiter; the buttons themselves are never rebuilt
    try {
      const data = await (await fetch('/api/installation', {cache: 'no-store'})).json();
      const row = (data.arbiters || [])[0];
      for (const [ch, b] of buttons) {
        const info = row && row.channels.find(c => c.channel === ch);
        b.querySelector('small').textContent = info ? NAMES[info.color] : '—';
        b.querySelector('.swatch').style.backgroundColor = info ? `rgb(${row.colors[info.color].rgb.join(',')})` : '';
      }
    } catch (e) { /* the installation page above already reports the connection */ }
  }
  build(); say(); colours(); setInterval(colours, 4000);
})();
