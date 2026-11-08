'use strict';
const $ = id => document.getElementById(id);
let state = null, selected = '', pending = false, dirty = new Map(), networkError = false;
function newSession() {
  const token=Array.from(crypto.getRandomValues(new Uint8Array(8)),value=>value.toString(16).padStart(2,'0')).join('').toUpperCase();
  sessionStorage.setItem('mapping-session',token);return token;
}
let session=sessionStorage.getItem('mapping-session');
if(!/^[0-9A-F]{16}$/.test(session || '')) session=newSession();
let clockDirty=false,clockBase=null,lastRenew=0;
const names = ['red','blue','green','yellow','orange','purple'];
function notice(text, error=false) {
  $('notice').textContent = text;
  $('notice').classList.toggle('error',error);
  $('notice').hidden = !text;
}
function current() { return state?.arbiters.find(row => row.id === selected); }
function paint(el, rgb) { el.style.backgroundColor = `rgb(${rgb.join(',')})`; }
function build() {
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
    form.innerHTML=`<h3><span class="swatch" id="rgb-swatch-${index}"></span>${name}</h3><div class="rgb-inputs"></div><button class="button secondary">Apply ${name}</button>`;
    ['r','g','b'].forEach(key => {
      const label=document.createElement('label'); label.textContent=key.toUpperCase();
      const input=document.createElement('input'); input.type='number'; input.min='0'; input.max='255'; input.step='1'; input.required=true; input.id=`rgb-${index}-${key}`; input.setAttribute('aria-label',`${name} ${key.toUpperCase()}`);
      input.addEventListener('input',()=>{if(!dirty.has(index)) dirty.set(index,current());});
      label.append(input); form.querySelector('.rgb-inputs').append(label);
    });
    form.addEventListener('submit',async event => {
      event.preventDefault();
      const values=Object.fromEntries(['r','g','b'].map(key => [key,Number($(`rgb-${index}-${key}`).value)]));
      const ok=await mutate('rgb',{color:index,...values},dirty.get(index));
      if(!ok && dirty.has(index)) dirty.set(index,current());
      if(ok) {dirty.delete(index);render();}
    });
    $('colors').append(form);
  });
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
  $('shuffle').disabled=!live; $('reset').disabled=!live;
  $('arbiter').disabled=pending || options.length<2 || !!(row?.mapping?.active && row.mapping.owner===session);
  $('connection').textContent=state.bridge_connected ? (row?.fresh ? 'Arbiter connected' : 'Waiting for arbiter') : 'Bridge disconnected';
  renderMapping(row,live);
  renderClock(row,live);
  if(!row) { $('arbiter-status').textContent=state.error || 'Waiting for a configuration snapshot…'; return; }
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
    state=data;render();
    if(networkError){notice("Connection restored.");networkError=false;}
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
$('reset').addEventListener('click',async()=>{if(await mutate('reset_rgb',{})){dirty.clear();render();}});
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
  if(document.visibilityState==='visible' && !pending && row?.fresh && row.mapping?.active && row.mapping.owner===session && Date.now()-lastRenew>6000) {
    lastRenew=Date.now();await mutate('map_renew',{session},null,true);
  }
},1000);
build();refresh();setInterval(refresh,2000);
