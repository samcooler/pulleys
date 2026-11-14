'use strict';
// Lever debug, on the goal chips: click a colour's square to pull a rope of that
// colour, as if its component were being activated. Every click is its own pull,
// sent at once without waiting for the one before, so quick clicks are quick pulls.
(() => {
  const $ = id => document.getElementById(id);
  const NAMES = ['red', 'blue', 'green', 'yellow', 'orange', 'purple'];
  const PURPLE = 5;   // the finals' colour: never a goal, so it has no tick, only a square
  let channels = null;   // the arbiter's current channel -> colour, kept fresh

  async function colours() {
    try {
      const data = await (await fetch('/api/installation', {cache: 'no-store'})).json();
      const row = (data.arbiters || [])[0];
      channels = row ? row.channels : null;
      const swatch = $('goal-swatch-5');
      if (swatch && row) swatch.style.backgroundColor = `rgb(${row.colors[PURPLE].rgb.join(',')})`;
    } catch (e) { /* the page's own status line reports the connection */ }
  }
  // A rope of that colour: one with ropes mapped to it if there is one, else any channel wearing it.
  function channelFor(colour) {
    const mine = (channels || []).filter(c => c.color === colour);
    return (mine.find(c => c.ropes > 0) || mine[0] || {}).channel;
  }
  function say(text) { const p = $('overview-goals-status'); if (p) p.textContent = text; }

  function fail(swatch, text) {
    swatch.classList.add('failed'); setTimeout(() => swatch.classList.remove('failed'), 600); say(text);
  }
  function send(swatch, channel) {
    fetch('/lever/' + channel, {method: 'POST', cache: 'no-store'})
      .then(async r => { if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.status); })
      .catch(e => fail(swatch, `Pull failed: ${e.message || e}`));
  }
  function pull(swatch) {
    const colour = Number(swatch.id.replace('goal-swatch-', ''));
    swatch.classList.add('hit'); setTimeout(() => swatch.classList.remove('hit'), 90);
    if (colour === PURPLE) {   // each purple rope is its own final component: one click pulls every roped one once
      const ropes = (channels || []).filter(c => c.color === PURPLE && c.ropes > 0).map(c => c.channel);
      if (!ropes.length) return fail(swatch, 'No purple rope is mapped right now.');
      ropes.forEach(ch => send(swatch, ch));
      return;
    }
    const channel = channelFor(colour);
    if (!channel) return fail(swatch, `No channel is wearing ${NAMES[colour]} right now.`);
    send(swatch, channel);
  }

  const box = $('overview-goals');
  box.addEventListener('click', event => {
    const swatch = event.target.closest('.swatch');
    if (!swatch || !box.contains(swatch)) return;
    event.preventDefault();    // the square is inside the label: do not also tick the goal
    pull(swatch);
  });
  box.addEventListener('keydown', event => {   // keyboard: Enter or Space on a focused square
    const swatch = event.target.closest?.('.swatch');
    if (swatch && (event.key === 'Enter' || event.key === ' ')) { event.preventDefault(); pull(swatch); }
  });
  function addPurple() {   // after installation.js builds the five goal chips
    if (!box.children.length || $('goal-swatch-5')) return;
    const chip = document.createElement('span');
    chip.className = 'goal-chip purple-chip';
    chip.title = 'Pulls every purple rope once: each is one of the final components';
    chip.innerHTML = '<span class="swatch" id="goal-swatch-5" style="background-color:#af52de"></span>purple';
    box.append(chip);
  }
  new MutationObserver(() => { addPurple(); box.querySelectorAll('.swatch:not([role])').forEach(s => {   // the chips are built after this runs
    s.removeAttribute('aria-hidden'); s.setAttribute('role', 'button'); s.tabIndex = 0;
    const name = NAMES[Number(s.id.replace('goal-swatch-', ''))];
    s.setAttribute('aria-label', `Pull ${name}: simulate that component being activated`); s.title = `Pull ${name}`;
  }); }).observe(box, {childList: true, subtree: true});
  colours(); setInterval(colours, 4000);
})();
