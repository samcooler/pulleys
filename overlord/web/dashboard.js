'use strict';
const $ = id => document.getElementById(id);
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const expanded = new Set();
let state = null, selected = localStorage.getItem('overlord-release') || '', posting = false, networkError = false, releaseOptionsKey = "";
const clock = timestamp => new Date(timestamp * 1000).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
// When a firmware batch was built, always in Pacific time (PST or PDT, whichever applies).
const built = timestamp => new Date(timestamp * 1000).toLocaleString('en-US', {timeZone:'America/Los_Angeles', year:'numeric', month:'short', day:'numeric', hour:'numeric', minute:'2-digit', timeZoneName:'short'});
const releaseLabel = row => row?.built_at ? built(row.built_at) : row?.tag || '';
const age = timestamp => {if (!timestamp) return 'Never'; const n = Math.max(0, Math.floor(Date.now()/1000-timestamp)); return n < 5 ? 'Just now' : n < 60 ? `${n}s ago` : n < 3600 ? `${Math.floor(n/60)}m ago` : `${Math.floor(n/3600)}h ago`;};
const badge = value => `<span class="badge ${esc(value)}">${esc(value.replaceAll('_',' '))}</span>`;
const release = () => state?.releases.find(row => row.id === selected);
function notify(message, error=false) {$('notice').textContent=message; $('notice').className=error?'error':''; $('notice').hidden=false;}
async function api(path='', options={}) {
  const response = await fetch(`/api/firmware${path}`, options);
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
  return body;
}
async function action(path, body={}) {
  if (posting) return;
  posting=true; renderButtons();
  try {
    const result = await api(path, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
    notify(`Queued ${result.job.kind} · ${result.job.id}`);
    await refresh();
  } catch(error) {notify(error.message,true);}
  finally {posting=false; renderButtons();}
}
function renderButtons() {
  $('scan').disabled=posting || !state;
  const target = release();
  const outdated = state?.devices.some(node => {
    const image=target?.manifest[node.env];
    return image?.uploaded && node.sha256!==image.build_hash;
  });
  $('update-all').disabled=posting || !outdated;
  document.querySelectorAll('[data-update],[data-retry]').forEach(button => button.disabled=posting);
}
function latestDeviceWork(id) {
  for (const job of state.jobs) {
    if (job.release_id===selected && job.devices[id]) return {...job.devices[id],job};
  }
  return null;
}
function deviceStatus(node, image, work) {
  if (work?.job.state==='running' && !['confirmed','skipped','failed'].includes(work.state)) return work.state;
  if (image && node.sha256===image.build_hash) return 'current';
  if (work?.state==='failed') return 'failed';
  if (!node.online) return 'not recent';
  if (!image?.uploaded) return 'no image';
  return 'outdated';
}
function render() {
  const focused=document.activeElement;
  const focusAttr=['data-expand','data-update','data-retry'].find(attr=>focused?.hasAttribute(attr));
  const focusValue=focusAttr?focused.getAttribute(focusAttr):null;
  const releases = state.releases;
  if (!releases.some(row=>row.id===selected)) selected=releases[0]?.id || '';
  const nextReleaseOptionsKey=JSON.stringify(releases.map(row=>[row.id,row.tag,row.built_at,row.ready]));
  if(nextReleaseOptionsKey!==releaseOptionsKey){
  $('release').innerHTML=releases.length?releases.map(row=>`<option value="${esc(row.id)}">${esc(releaseLabel(row))}${row.ready?'':' · partial upload'}</option>`).join(''):'<option value="">No release loaded</option>';
  releaseOptionsKey=nextReleaseOptionsKey;
  }
  if($('release').value!==selected) $('release').value=selected;
  const target = release();
  $('connection').className=`connection${state.bridge.connected?'':' offline'}`;
  $('connection').innerHTML=`<span class="dot${state.bridge.connected?' green':''}"></span> ${state.bridge.connected?'USB linked':'Bridge disconnected'}${state.bridge.id?' · '+esc(state.bridge.id):''}`;
  $('updated').textContent=`Updated ${clock(state.now)}`;
  $('count-seen').textContent=state.devices.length;
  $('count-online').textContent=`${state.devices.filter(node=>node.online).length} seen in the last 90 seconds`;
  $('count-current').textContent=state.devices.filter(node=>target?.manifest[node.env]?.build_hash===node.sha256).length;
  $('count-queued').textContent=state.jobs.filter(job=>job.state==='queued').length;
  $('count-failed').textContent=state.jobs.filter(job=>['failed','interrupted'].includes(job.state)).length;
  $('device-count').textContent=state.devices.length;
  $('devices').innerHTML=state.devices.length?state.devices.map(node=>{
    const image=target?.manifest[node.env], work=latestDeviceWork(node.id), status=deviceStatus(node,image,work);
    const known=releases.find(row=>node.release_ids.includes(row.id));
    const percent=work?.total?Math.min(100, Math.floor((work.bytes||0)/work.total*100)):0;
    const active=work?.job.state==='running' && !['confirmed','skipped','failed'].includes(work.state);
    const logs=state.logs.filter(event=>event.node_id===node.id);
    const canUpdate=image?.uploaded && node.sha256!==image.build_hash && !active;
    return `<tr class="device-row"><td><div class="device-title"><span class="dot${node.online?' online':''}"></span><button class="expand" data-expand="${esc(node.id)}" aria-expanded="${expanded.has(node.id)}" aria-controls="log-${esc(node.id)}">${esc(node.name || node.id)} <span class="chevron">${expanded.has(node.id)?'−':'+'}</span></button></div><div class="device-meta">${esc(node.env || node.role || 'Unknown role')}</div></td><td><span class="version">${esc(known?releaseLabel(known):'Unmanaged build')}</span><span class="hash" title="${esc(node.sha256)}">${node.sha256 && node.sha256!=='unknown'?esc(node.sha256.slice(0,12)):esc(node.build || 'Identity unknown')}</span></td><td>${badge(status)}${active?`<div class="node-progress" role="progressbar" aria-label="Firmware bytes served" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${percent}"><span style="width:${percent}%"></span></div><span class="hash">${percent}% served${work.attempt?' · try '+work.attempt:''}</span>`:''}</td><td class="seen" title="${node.last_seen?esc(new Date(node.last_seen*1000).toLocaleString()):''}">${age(node.last_seen)}</td><td>${canUpdate?`<button class="button small" data-update="${esc(node.id)}">Update</button>`:'<span class="subtle">—</span>'}</td></tr>${expanded.has(node.id)?`<tr class="node-detail" id="log-${esc(node.id)}"><td colspan="5"><pre>${esc(`Build: ${node.build || 'unknown'}\nBuilt: ${known?.built_at?built(known.built_at):'unknown'}\nIdentity: ${node.sha256 || 'unknown'}\n` + (logs.length?logs.slice(-30).map(event=>`${clock(event.at)}  ${event.message}`).join('\n'):'No update activity recorded for this device.'))}</pre></td></tr>`:''}`;
  }).join(''):'<tr><td colspan="5" class="empty">No devices reported yet.<br>Scan the mesh to collect running firmware versions.</td></tr>';
  const activeJob=state.jobs.find(job=>job.id===state.active_job);
  $('queue-badge').className=`badge ${activeJob?'running':'neutral'}`;
  $('queue-badge').textContent=activeJob?'Running':'Idle';
  $('queue').innerHTML=state.jobs.length?state.jobs.slice(0,8).map(job=>{
    const tag=releaseLabel(releases.find(row=>row.id===job.release_id));
    const results=Object.entries(job.devices);
    const confirmed=results.filter(([,row])=>row.state==='confirmed').length;
    return `<article class="job"><div class="job-title"><span>${esc({scan:'Mesh census',update:'Firmware update'}[job.kind]||job.kind)}</span>${badge(job.state)}</div><div class="job-meta">${esc(tag||'')} ${job.current_device?' · '+esc(job.current_device):''}<br>${esc(job.id)} · ${age(job.created_at)}${results.length?`<br>${confirmed} confirmed · ${results.filter(([,row])=>row.state==='skipped').length} already current`:''}</div>${job.error?`<div class="job-error">${esc(job.error)}</div>`:''}${['failed','interrupted'].includes(job.state)?`<button class="button small" data-retry="${esc(job.id)}">Retry failed</button>`:''}</article>`;
  }).join(''):'<p class="empty">The queue is clear.<br>Updates run one device at a time.</p>';
  $('activity').innerHTML=state.logs.length?state.logs.slice(-50).reverse().map(event=>`<div class="event ${event.level==='error'?'error':''}"><span class="event-time">${clock(event.at)}${event.node_id?' · '+esc(event.node_id):''}</span>${esc(event.message)}</div>`).join(''):'<p class="empty">Firmware events will appear here.</p>';
  renderButtons();
  if(focusAttr) Array.from(document.querySelectorAll('button')).find(button=>button.getAttribute(focusAttr)===focusValue)?.focus({preventScroll:true});
}
async function refresh() {
  try {state=await api(); if(networkError){$('notice').hidden=true; networkError=false;} render();}
  catch(error) {networkError=true; $('connection').className='connection offline'; $('connection').textContent='Server unavailable'; notify(`Cannot refresh: ${error.message}`,true);}
}
$('release').addEventListener('change',()=>{selected=$('release').value; localStorage.setItem('overlord-release',selected); render();});
$('scan').addEventListener('click',()=>action('/scan'));
$('update-all').addEventListener('click',()=>action('/updates',{release_id:selected,all:true}));
document.addEventListener('click',event=>{
  const expand=event.target.closest('[data-expand]'), update=event.target.closest('[data-update]'), retry=event.target.closest('[data-retry]');
  if(expand){const id=expand.dataset.expand; expanded.has(id)?expanded.delete(id):expanded.add(id); render();}
  if(update) action('/updates',{release_id:selected,device_ids:[update.dataset.update]});
  if(retry) action(`/jobs/${encodeURIComponent(retry.dataset.retry)}/retry`);
});
$('upload-form').addEventListener('submit',async event=>{
  event.preventDefault(); const button=$('upload-button'); button.disabled=true;
  try {
    const manifest=JSON.parse(await $('manifest-file').files[0].text());
    const files=Array.from($('binary-files').files);
    const names=new Set(Object.values(manifest.firmware || {}).map(image=>image.asset));
    if(files.some(file=>!names.has(file.name))) throw new Error('A selected binary is not listed in this manifest.');
    const result=await api('/releases',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(manifest)});
    selected=result.release.id;
    for(let i=0;i<files.length;i++){
      const file=files[i], entries=Object.entries(manifest.firmware).filter(([,image])=>image.asset===file.name);
      $('upload-result').textContent=`Uploading ${i+1}/${files.length}: ${file.name}`;
      for(const [env] of entries) await api(`/releases/${encodeURIComponent(selected)}/${encodeURIComponent(env)}`,{method:'PUT',headers:{'Content-Type':'application/octet-stream'},body:file});
    }
    localStorage.setItem('overlord-release',selected);
    $('upload-result').textContent=`Uploaded ${files.length} binaries. Select devices to update.`;
    notify(`Local release ${manifest.built_at?built(manifest.built_at):manifest.tag} is available.`); await refresh();
  }catch(error){$('upload-result').textContent=error.message; notify(error.message,true);}
  finally{button.disabled=false;}
});
async function poll(){await refresh(); setTimeout(poll,2000);}
poll();
