'use strict';
const $ = id => document.getElementById(id);
let state = null, selected = '', pending = false, dirty = new Map(), networkError = false;
function newSession() {
  const token=Array.from(crypto.getRandomValues(new Uint8Array(8)),value=>value.toString(16).padStart(2,'0')).join('').toUpperCase();
  sessionStorage.setItem('mapping-session',token);return token;
}
let session=sessionStorage.getItem('mapping-session');
if(!/^[0-9A-F]{16}$/.test(session || '')) session=newSession();
let clockDirty=false,clockBase=null,lastRenew=0,receivedAt=Date.now();
const names = ['red','blue','green','yellow','orange','purple'];
function notice(text, error=false) {
  $('notice').textContent = text;
  $('notice').classList.toggle('error',error);
  $('notice').hidden = !text;
}
function current() { return state?.arbiters.find(row => row.id === selected); }
function paint(el, rgb) { el.style.backgroundColor = `rgb(${rgb.join(',')})`; }
// One checkbox per game colour (purple is never a goal) saying whether it is a goal now.
function buildGoals() {
  const box=$('overview-goals');
  for(let index=0;index<5;index++) {
    const label=document.createElement('label'); label.className='goal-chip';
    label.innerHTML=`<input type="checkbox" id="goal-${index}" aria-label="${names[index]} is a goal"><span class="swatch" id="goal-swatch-${index}" aria-hidden="true"></span>${names[index]}`;
    label.querySelector('input').addEventListener('change',event => {
      const mask=[0,1,2,3,4].reduce((bits,i)=>bits|($(`goal-${i}`).checked?1<<i:0),0);
      mutate('goals',{mask}).then(ok => {if(!ok) event.target.checked=!event.target.checked;});
    });
    box.append(label);
  }
}
function build() {
  buildGoals();
  for (let ch=1; ch<=24; ch++) {
    $('map-channel').add(new Option(`Channel ${ch}`,ch));
    const card = document.createElement('label'); card.className='channel';
    card.innerHTML=`<span class="channel-head"><strong>CH ${ch}</strong><span class="swatch" id="swatch-${ch}"></span></span><small id="ropes-${ch}"></small><span class="sr-only">Channel ${ch} colour</span><select id="channel-${ch}"></select>`;
    const select=card.querySelector('select');
    names.forEach((name,i) => select.add(new Option(name,i)));
    select.addEventListener('change', () => mutate('color',{channel:ch,color:Number(select.value)}));
    $('channels').append(card);
  }
  names.forEach((name,index) => {
    const form=document.createElement('form'); form.className='rgb-form';
    form.innerHTML=`<h3><button type="button" class="swatch swatch-btn" id="rgb-swatch-${index}" aria-label="Pick ${name} colour" aria-haspopup="dialog" title="Pick a colour"></button>${name}</h3><div class="rgb-inputs"></div><button class="button secondary">Apply ${name}</button>`;
    ['r','g','b'].forEach(key => {
      const label=document.createElement('label'); label.textContent=key.toUpperCase();
      const input=document.createElement('input'); input.type='number'; input.min='0'; input.max='255'; input.step='1'; input.required=true; input.id=`rgb-${index}-${key}`; input.setAttribute('aria-label',`${name} ${key.toUpperCase()}`);
      input.addEventListener('input',()=>{if(!dirty.has(index)) dirty.set(index,current());});
      label.append(input); form.querySelector('.rgb-inputs').append(label);
    });
    form.addEventListener('submit',async event => {
      event.preventDefault();
      await applyRgb(index,['r','g','b'].map(key => Number($(`rgb-${index}-${key}`).value)));
    });
    form.querySelector('.swatch-btn').addEventListener('click',event => openPicker(index));
    $('colors').append(form);
  });
}
// Sets a named colour's RGB on the arbiter and refreshes at once. Used by the form and the picker.
async function applyRgb(index,[r,g,b]) {
  const ok=await mutate('rgb',{color:index,r,g,b},dirty.get(index));
  if(!ok && dirty.has(index)) dirty.set(index,current());
  if(ok) {dirty.delete(index);render();}
  return ok;
}
// Clicking a swatch opens a picker showing the colour's R, G and B as integers (0-255), each a
// slider and a number box kept in step, over a live preview. Apply saves; Cancel or Esc changes nothing.
function openPicker(index) {
  const rgb=[...(current()?.colors[index].rgb || [0,0,0])];
  const dlg=document.createElement('dialog'); dlg.className='rgb-picker';
  dlg.innerHTML=`<form method="dialog"><h3>${names[index]}</h3><div class="rgb-preview"></div><div class="rgb-rows"></div><p class="rgb-readout"></p><div class="rgb-actions"><button value="cancel" class="button secondary">Cancel</button><button value="ok" class="button">Apply ${names[index]}</button></div></form>`;
  const preview=dlg.querySelector('.rgb-preview'), readout=dlg.querySelector('.rgb-readout');
  const pairs=[];
  const sync=()=>{paint(preview,rgb); readout.textContent=`rgb(${rgb.join(', ')})`; pairs.forEach(([range,num],k)=>{range.value=num.value=rgb[k];});};
  ['R','G','B'].forEach((key,k) => {
    const row=document.createElement('label'); row.className='rgb-row'; row.innerHTML=`<b>${key}</b>`;
    const range=document.createElement('input'); range.type='range';
    const num=document.createElement('input'); num.type='number';
    for(const el of [range,num]) {el.min=0; el.max=255; el.step=1; el.setAttribute('aria-label',`${names[index]} ${key}`);}
    range.classList.add(`ch-${key.toLowerCase()}`);
    const set=v=>{const n=Math.round(Number(v)); rgb[k]=Number.isFinite(n)?Math.min(255,Math.max(0,n)):0; sync();};
    range.addEventListener('input',()=>set(range.value));
    num.addEventListener('input',()=>{if(num.value!=='') set(num.value);});
    num.addEventListener('change',()=>set(num.value));
    pairs.push([range,num]); row.append(range,num); dlg.querySelector('.rgb-rows').append(row);
  });
  dlg.addEventListener('close',()=>{const ok=dlg.returnValue==='ok'; dlg.remove(); if(ok) applyRgb(index,rgb);});
  document.body.append(dlg); sync(); dlg.showModal();
}
function render() {
  if(!state) return;
  const options=state.arbiters.map(row=>row.id);
  if(!options.includes(selected)) {selected=options[0]||'';dirty.clear();clockDirty=false;clockBase=null;}
  if(Array.from($('arbiter').options).map(row=>row.value).join() !== options.join()) {
    $('arbiter').replaceChildren(...options.map(id => new Option(id,id)));
  }
  $('arbiter').value=selected;
  const row=current();
  const live=!!row?.fresh && state.bridge_connected && !state.busy && !pending;
  $('channel-fields').disabled=!live; $('rgb-fields').disabled=!live;
  $('shuffle').disabled=!live; $('reset').disabled=!live; $('reset-game').disabled=!live;
  $('arbiter').disabled=pending || options.length<2 || !!(row?.mapping?.active && row.mapping.owner===session);
  renderOverview(row);
  renderMapping(row,live);
  renderClock(row,live);
  if(!row) { $('arbiter-status').textContent=state.error || 'Waiting for a configuration snapshot…'; $('storage').textContent=state.busy ? 'Firmware work is in progress. Configuration updates are paused.' : 'Edits become available when the arbiter responds.'; return; }
  $('arbiter-status').textContent=`${row.id} · revision ${row.revision} · received ${row.age_seconds}s ago${row.fresh?'':' · stale'}`;
  $('storage').textContent=state.busy ? 'Firmware work is in progress. Configuration edits are paused.' : pending ? 'Waiting for the arbiter to apply the change…' : row.storage==='pending' ? 'Applied on arbiter; waiting for storage. Device convergence is not yet confirmed.' : row.storage==='failed' ? 'Applied on arbiter, but storage failed. Changes may be lost after reboot.' : 'Operator configuration saved on arbiter. Device convergence is not confirmed by this page.';
  if(row.game_shuffled) $('storage').textContent+=' The game is using a temporary shuffle; an operator edit restores the daily palette first.';
  row.channels.forEach(channel => {
    const ch=channel.channel;
    $(`channel-${ch}`).value=channel.color;
    $(`ropes-${ch}`).textContent=`${channel.ropes} rope${channel.ropes===1?'':'s'}${row.game_shuffled ? ` · daily ${names[channel.daily_color]}` : ''}`;
    paint($(`swatch-${ch}`),row.colors[channel.color].rgb);
  });
  row.colors.forEach((color,i) => {
    paint($(`rgb-swatch-${i}`),color.rgb);
    if(!dirty.has(i)) ['r','g','b'].forEach((key,k)=>{$(`rgb-${i}-${key}`).value=color.rgb[k];});
  });
}
async function refresh() {
  try {
    const response=await fetch('/api/installation'); const data=await response.json();
    if(!response.ok) throw new Error(data.error || 'Configuration unavailable');
    state=data;receivedAt=Date.now();render();
    if(networkError){notice("Connection restored.");networkError=false;renderOverview(current());}
  } catch(error) {
    if(state) {state.bridge_connected=false;render();}
    networkError=true;notice(error.message,true);
  }
}
async function mutate(op,values,base=null,quiet=false) {
  const row=current();
  if(!row?.fresh || pending) return false;
  const expected=base || row;
  const body={target:expected.id,boot:expected.boot,revision:expected.revision,...values};
  pending=true;render();if(!quiet) notice('Sending change to the arbiter…');
  let ok=false;
  try {
    const response=await fetch(`/api/installation/${op}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const data=await response.json();
    if(!response.ok) {
      const messages={conflict:'The configuration changed elsewhere. Review the current values and apply again.',mapping_busy:'Mapping is already in use by another operator.',mapping_session_lost:'The mapping session expired or ended. Start a new session.',capture_not_ready_or_map_full:'The capture is not ready, or the sensor map is full.',clock_set_failed:'The arbiter could not set its clock. Check the RTC and try again.'};
      throw new Error(messages[data.error] || data.error || 'The change could not be confirmed.');
    }
    if(!quiet) notice(op.startsWith('map_') ? 'Mapping session updated on arbiter.' : 'Applied on arbiter. Readback and storage status update below.');ok=true;
  } catch(error) {notice(error.message,true);}
  finally {pending=false;await refresh();render();}
  return ok;
}
$('arbiter').addEventListener('change',()=>{selected=$('arbiter').value;dirty.clear();clockDirty=false;clockBase=null;render();});
$('shuffle').addEventListener('click',()=>mutate('shuffle',{}));
$('reset-game').addEventListener('click',async()=>{if(confirm('Reset the game? This clears its state and history. Configuration is kept.')&&await mutate('reset_game',{})) notice('Game reset on the arbiter.');});
$('reset').addEventListener('click',async()=>{if(await mutate('reset_rgb',{})){dirty.clear();render();}});
function renderOverview(row) {
  const view=InstallationStatus.model(state,row,{pending,networkError,elapsedSeconds:(Date.now()-receivedAt)/1000});
  const fields={health:'health',freshness:'freshness',game:'gameName',state:'state','state-age':'stateAge',timer:'timer','timer-target':'timerTarget',nodes:'nodes',activity:'activity',events:'events',uptime:'uptime',skew:'skew',devices:'devices','goals-status':'goalsMessage'};
  for(const [id,key] of Object.entries(fields)) {
    const element=$(`overview-${id}`);
    if(element.textContent!==view[key]) element.textContent=view[key];
  }
  $('connection').textContent=view.health;
  document.querySelector('.overview-panel').classList.toggle('report-stale',!view.live);
  $('overview-devices').classList.toggle('status-warning',!view.devicesOk);
  const set=row?.game?.goal_set;   // older arbiter firmware has no goal_set: show, but do not edit
  const editable=Array.isArray(set) && !!row?.fresh && state.bridge_connected && !state.busy && !pending;
  for(let index=0;index<5;index++) {
    const box=$(`goal-${index}`);
    box.checked=Array.isArray(set) ? set.includes(index) : view.goals.includes(index);
    box.disabled=!editable;
    if(row?.colors?.[index]) paint($(`goal-swatch-${index}`),row.colors[index].rgb);
  }
  if(Array.isArray(set) && row.game?.ready && !row.game.goals_visible)
    $('overview-goals-status').textContent='set; not shown in this state';
  else if(Array.isArray(set)) $('overview-goals-status').textContent=view.live?'tick = goal · click a square to pull it':'last reported';
}
function renderMapping(row,live) {
  const map=row?.mapping;
  const owned=!!map?.active && map.owner===session;
  const ready=owned && map.phase==='pick' && map.sensors.length>0;
  $('map-start').disabled=!live || !!map?.active;
  $('map-again').disabled=!live || !owned;
  $('map-done').disabled=!live || !owned;
  $('map-channel').disabled=!live || !ready;
  $('map-assign').disabled=!live || !ready;
  const age=(row?.age_seconds || 0)*1000;
  if(!map?.active) $('map-status').textContent='Mapping is closed. Pulls reach the game.';
  else if(!owned) $('map-status').textContent=`Mapping is in use by ${map.kind==='touch'?'the touchscreen':'another browser'}. Pulls are withheld from the arbiter game.`;
  else {
    const left=Math.max(0,Math.ceil((map.listen_left_ms-age)/1000));
    const lease=Math.max(0,Math.ceil((map.lease_left_ms-age)/1000));
    const prompt=map.phase==='wait' ? 'Pull a rope to start a group.' : map.phase==='listen' ? `Listening: ${left}s remaining. Pull any other ropes that share this channel.` : map.assigned<0 ? 'Choose a channel for the captured ropes.' : `Assigned to channel ${map.assigned}. Pull a new rope to start the next group.`;
    $('map-status').textContent=`${prompt} Pulls are withheld from the arbiter game. Session lease: ${lease}s.`;
  }
  const body=$('map-sensors');body.replaceChildren();
  if(!map?.sensors.length) {const tr=document.createElement('tr');const td=document.createElement('td');td.colSpan=4;td.textContent='No ropes captured.';tr.append(td);body.append(tr);return;}
  map.sensors.forEach(sensor=>{
    const fresh=sensor.seen_ms_ago!==null && sensor.seen_ms_ago+age<20000;
    const confirmed=map.assigned>=0 && fresh && sensor.reported===map.assigned;
    const tr=document.createElement('tr');
    [sensor.id,sensor.reported??'Unknown',sensor.assigned??'Unmapped',confirmed?'Reported on assigned channel':!fresh?'Offline or stale':map.assigned>=0?'Waiting for report':'Awaiting assignment'].forEach(text=>{const td=document.createElement('td');td.textContent=text;tr.append(td);});
    body.append(tr);
  });
}
function renderClock(row,live) {
  const clock=row?.clock;
  const enabled=live && !!clock?.present;
  ['clock-time','clock-save','clock-cancel','clock-now'].forEach(id=>$(id).disabled=!enabled);
  $('overlord-time').textContent=state ? `Overlord local time: ${state.local_time} · ${state.timezone}. Manual entries use this installation local time.` : '';
  $('clock-status').textContent=!clock ? 'Waiting for the arbiter clock…' : !clock.present ? 'No RTC detected on the arbiter.' : `${clock.time || 'Clock read failed'} · source: ${clock.source}${clock.storage==='failed'?' · could not store recovery snapshot':''}`;
  if(!clockDirty && clock?.time) $('clock-time').value=clock.time.replace(' ','T');
}
$('map-start').addEventListener('click',async()=>{session=newSession();if(await mutate('map_begin',{session}))lastRenew=Date.now();});
$('map-again').addEventListener('click',()=>mutate('map_again',{session}));
$('map-assign').addEventListener('click',()=>mutate('map_assign',{session,channel:Number($('map-channel').value)}));
$('map-done').addEventListener('click',()=>mutate('map_end',{session}));
$('clock-time').addEventListener('input',()=>{if(!clockDirty)clockBase=current();clockDirty=true;});
$('clock-cancel').addEventListener('click',()=>{clockDirty=false;clockBase=null;render();});
$('clock-form').addEventListener('submit',async event=>{
  event.preventDefault();
  const value=$('clock-time').value;
  const time=value.replaceAll(/[-T:]/g,'').padEnd(14,'0');
  const ok=await mutate('clock',{time},clockBase);
  if(ok){clockDirty=false;clockBase=null;render();}else clockBase=current();
});
$('clock-now').addEventListener('click',async()=>{if(await mutate('clock_now',{})){clockDirty=false;clockBase=null;render();}});
setInterval(async()=>{
  const row=current();
  renderOverview(row);
  if(document.visibilityState==='visible' && !pending && row?.fresh && row.mapping?.active && row.mapping.owner===session && Date.now()-lastRenew>6000) {
    lastRenew=Date.now();await mutate('map_renew',{session},null,true);
  }
},1000);
// Game YAML editor: loaded the first time it is opened. Check validates on the Overlord; Save checks, then writes the game file.
(()=>{
  const box=$('yaml-text'), status=$('yaml-status'); let loaded=false, original='';
  const say=(text,cls='')=>{status.textContent=text; status.className='dim '+cls;};
  const buttons=on=>['yaml-check','yaml-save','yaml-reload'].forEach(id=>$(id).disabled=!on);
  async function post(save) {
    buttons(false); say(save?'Saving…':'Checking…');
    try {
      const response=await fetch('/api/game/yaml',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({yaml:box.value,save})});
      const data=await response.json(); if(!response.ok) throw new Error(data.error||'request failed');
      if(!data.ok) say(data.errors.join('\n'),'bad');
      else if(data.saved) {original=box.value; say('Saved. The arbiter and a running game pick it up when they next load it.','good');}
      else say('OK: no problems found.','good');
    } catch(error) {say(error.message,'bad');}
    buttons(true);
  }
  async function load() {
    buttons(false); say('Loading…');
    try {
      const response=await fetch('/api/game/yaml'); const data=await response.json(); if(!response.ok) throw new Error(data.error||'request failed');
      box.value=original=data.yaml; box.disabled=false; loaded=true; $('yaml-path').textContent=data.path.split('/').slice(-2).join('/'); say('Loaded.'); buttons(true);
    } catch(error) {say(error.message,'bad');}
  }
  $('yaml-panel').addEventListener('toggle',()=>{if($('yaml-panel').open && !loaded) load();});
  $('yaml-check').addEventListener('click',()=>post(false));
  $('yaml-save').addEventListener('click',()=>post(true));
  $('yaml-reload').addEventListener('click',()=>{if(box.value===original||confirm('Discard your edits and reload the file?')) load();});
  box.addEventListener('keydown',event=>{
    if(event.key==='Tab'){event.preventDefault(); document.execCommand('insertText',false,'  ');}
    else if((event.metaKey||event.ctrlKey)&&event.key==='s'){event.preventDefault(); post(true);}
  });
  window.addEventListener('beforeunload',event=>{if(loaded&&box.value!==original) event.preventDefault();});
})();
build();refresh();setInterval(refresh,2000);
