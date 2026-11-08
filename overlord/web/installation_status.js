'use strict';
// Pure presentation rules also exercised by the host tests. Timers never
// advance the game locally; only the arbiter can report a state transition.
const InstallationStatus = (() => {
  function duration(ms, roundUp=false) {
    if(!Number.isFinite(ms) || ms<0) return '—';
    const seconds=(roundUp?Math.ceil:Math.floor)(ms/1000);
    const s=String(seconds%60).padStart(2,'0'), m=Math.floor(seconds/60);
    return m<60 ? `${m}:${s}` : `${Math.floor(m/60)}:${String(m%60).padStart(2,'0')}:${s}`;
  }
  function model(state,row,{pending=false,elapsedSeconds=0,networkError=false}={}) {
    const age=(row?.age_seconds || 0)+Math.max(0,elapsedSeconds);
    const fresh=!!row?.fresh && !!state?.bridge_connected && !networkError && age<15;
    const live=fresh && !state?.busy;
    const game=row?.game, mesh=row?.mesh;
    const ready=!!game?.ready;
    const advance=live ? age*1000 : 0;
    let health='Connecting';
    if(networkError) health='Connection lost';
    else if(state) {
      health=!state.bridge_connected ? 'Bridge disconnected' : !row ? 'Waiting for arbiter' : !fresh ? 'Arbiter report stale' : state.busy ? 'Firmware work in progress' : pending ? 'Applying change' : 'Arbiter connected';
    }
    let goalsMessage=!game ? 'Waiting for game status. Updated arbiter firmware is required.' : !ready ? 'No game loaded.' : !game.goals_visible ? 'No active goals in this state.' : 'No goals selected.';
    const goals=ready && game.goals_visible ? (game.goal_colors || []).filter(c=>Number.isInteger(c) && c>=0 && c<6) : [];
    if(goals.length) goalsMessage=live ? 'Current goal colours' : 'Last reported goal colours';
    const hasTimer=ready && Number.isFinite(game.timer_left_ms) && game.timer_left_ms>=0;
    const left=hasTimer ? Math.max(0,game.timer_left_ms-advance) : null;
    return {
      health,live,fresh,goals,goalsMessage,
      freshness:row ? `${live?'Live report':'Last report'} · ${age.toFixed(1)}s ago${state?.busy?' · updates paused for firmware work':''}` : (state?.error || 'Waiting for an arbiter report.'),
      gameName:game ? ready ? game.name : 'No game loaded' : 'Game status unavailable',
      state:ready ? game.state : '—',
      stateAge:ready ? `${duration(game.state_age_ms+advance)} in state` : 'Waiting for a game',
      timer:hasTimer ? duration(left,true) : '—',
      timerTarget:hasTimer ? left===0 && live ? 'Awaiting arbiter transition' : `Next: ${game.timer_target || 'unknown'}` : ready ? 'No timer scheduled' : 'Waiting for a game',
      nodes:mesh ? `${mesh.sensors} sensors · ${mesh.screens} screens` : '—',
      activity:mesh ? `${Math.round(mesh.events_per_min)} / min` : '—',
      events:mesh ? `${mesh.events} total pulls` : 'Waiting for mesh status',
      uptime:Number.isFinite(row?.uptime_ms) ? duration(row.uptime_ms+advance) : '—',
      skew:mesh ? `${mesh.skew_spread_ms} ms` : '—',
      devices:game ? game.devices_status || 'No game-controlled devices declared.' : 'Waiting for device status.',
      devicesOk:game?.devices_ok !== false,
    };
  }
  return {model,duration};
})();
if(typeof module!=='undefined') module.exports=InstallationStatus;
