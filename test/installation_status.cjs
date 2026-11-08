'use strict';
const assert=require('node:assert/strict');
const {model,duration}=require('../overlord/web/installation_status.js');
const state={bridge_connected:true,busy:false};
const row={fresh:true,age_seconds:2,uptime_ms:120000,
  game:{ready:true,name:'Pulleys',state:'easy_round',state_age_ms:8000,timer_left_ms:42000,timer_target:'round_end',goals_visible:true,goal_colors:[0,2],devices_ok:true,devices_status:'devices: 3/3 as set'},
  mesh:{sensors:12,screens:3,events:90,events_per_min:30,skew_spread_ms:8}};
let view=model(state,row,{elapsedSeconds:1});
assert.equal(view.health,'Arbiter connected');
assert.deepEqual(view.goals,[0,2]);
assert.equal(view.goalsMessage,'Current goal colours');
assert.equal(view.stateAge,'0:11 in state');
assert.equal(view.timer,'0:39');
assert.equal(view.nodes,'12 sensors · 3 screens');
assert.equal(view.activity,'30 / min');
assert.equal(view.uptime,'2:03');
assert.equal(view.skew,'8 ms');
// Party/idle states must hide previous round's goals even if a mask remains.
view=model(state,{...row,game:{...row.game,goals_visible:false}});
assert.deepEqual(view.goals,[]);
assert.equal(view.goalsMessage,'No active goals in this state.');
view=model(state,{...row,game:{...row.game,goal_colors:[]}});
assert.equal(view.goalsMessage,'No goals selected.');
// A stale report remains visibly historical; don't keep running its timer.
view=model(state,row,{elapsedSeconds:20});
assert.equal(view.live,false);
assert.equal(view.timer,'0:42');
assert.equal(view.goalsMessage,'Last reported goal colours');
assert.equal(view.health,'Arbiter report stale');
view=model({...state,bridge_connected:false},row);
assert.equal(view.health,'Bridge disconnected');
assert.equal(view.live,false);
view=model({...state,busy:true},row);
assert.equal(view.health,'Firmware work in progress');
assert.equal(view.live,false);
assert.equal(view.timer,'0:42');
assert.match(view.freshness,/updates paused/);
view=model(state,row,{networkError:true});
assert.equal(view.health,'Connection lost');
assert.equal(view.live,false);
// Firmware predating the new fields should still render configuration controls.
view=model(state,{fresh:true,age_seconds:0});
assert.equal(view.state,'—');
assert.match(view.goalsMessage,/Updated arbiter firmware is required/);
view=model(state,{...row,game:{ready:false,goal_colors:[0,2],goals_visible:true}});
assert.deepEqual(view.goals,[]);
assert.equal(view.gameName,'No game loaded');
assert.equal(view.timer,'—');
view=model(state,{...row,game:{...row.game,timer_left_ms:-1}});
assert.equal(view.timerTarget,'No timer scheduled');
view=model(state,{...row,game:{...row.game,timer_left_ms:1}});
assert.equal(view.timer,'0:00');
assert.equal(view.timerTarget,'Awaiting arbiter transition');
assert.equal(view.state,'easy_round'); // never invent the target state locally
assert.equal(duration(3601000),'1:00:01');
assert.equal(duration(1,true),'0:01');
assert.equal(duration(-1),'—');
assert.equal(model(null,null).health,'Connecting');
assert.equal(model(state,null).health,'Waiting for arbiter');
