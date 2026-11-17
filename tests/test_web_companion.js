const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../tools/web_auth_companion.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1].replace(/\npoll\(\);\s*$/, '\n');
const elements = new Map();
function element() {
  return {
    value:'', children:[], dataset:{}, classList:{add(){},remove(){}},
    addEventListener(){}, setAttribute(){}, focus(){}, reset(){}, click(){},
    replaceChildren(){this.children=[];}, append(...nodes){this.children.push(...nodes);},
    querySelector(){return element();},
    set innerHTML(value){throw new Error('Unsafe HTML insertion');}
  };
}
const document = {
  getElementById(id){if(!elements.has(id)) elements.set(id,element());return elements.get(id);},
  createElement(tag){const node=element();node.tagName=tag;return node;},
  querySelectorAll(){return [];}
};
const context = vm.createContext({document, URL, TextEncoder, AbortSignal, setTimeout(){}, atob, console});
vm.runInContext(script, context);
const run = expression => vm.runInContext(expression, context);
const uri = 'otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=SHA512&digits=8&period=60';
const parsed = run(`parseOtpUri(${JSON.stringify(uri)})`);
assert.equal(parsed.algorithm,2);assert.equal(parsed.digits,8);assert.equal(parsed.period,60);
assert.equal(parsed.account,'alice');
assert.equal(run("base64ToBase32('MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=')"),'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ');
assert.equal(run("parseOtpUri('otpauth://totp/Steam:test?secret=JBSWY3DPEHPK3PXP&encoder=steam').digits"),5);
for(const bad of ['https://example.com','otpauth://hotp/a?secret=JBSWY3DPEHPK3PXP','otpauth://totp/a?secret=bad!','otpauth://totp/a?secret=JBSWY3DPEHPK3PXP&algorithm=MD5']){
  assert.throws(()=>run(`parseOtpUri(${JSON.stringify(bad)})`));
}
const payload='<img src=x onerror=alert(1)>';
run(`accounts=[{index:0,issuer:${JSON.stringify(payload)},account:'test',algorithm:2,digits:8,period:30}];connected=true;status={unlocked:true};renderAccounts();`);
const list=elements.get('accountList');
assert.equal(list.children[0].children[0].children[0].textContent,payload);
assert.equal(list.children[0].children[0].children[0].tagName,'strong');
assert.equal(html.includes('innerHTML'),false);
run('accountRevision=7;editAccount(accounts[0]);accountRevision=8;');
assert.equal(run('editingRevision'),7);
assert.equal(run('editing'),0);
async function timeReferenceTests() {
  assert.equal(run('typeof syncBrowserTime'), 'function');
  assert.equal(html.includes("$('syncTime').addEventListener('click', syncBrowserTime)"), true);
  const realRequest=run('request'), realSend=run('sendCommand');
  const realUpdate=run('updateStatus'), realLoad=run('loadAccounts');
  for(const signal of [undefined,{}]) {
    context.AbortSignal=signal;
    context.fetch=async(route,options)=>{assert.equal(route,'/api/status');assert.equal(options.cache,'no-store');return {ok:true,json:async()=>({oldChrome:true})};};
    assert.equal((await run("request('/api/status')")).oldChrome,true);
  }
  context.AbortSignal=AbortSignal;
  const base = 1700000000000;
  const probe = {id:17,hrt_us:12345,window_us:20,valid:true};
  async function sync({wall=[base,base+40,base+80],mono=[100,140.0005],sample=probe,performance=true,failGet=false}={}) {
    const calls=[];
    context.Date = {now:()=>wall.shift()};
    context.performance = performance ? {now:()=>mono.shift()} : undefined;
    context.captureRequest = async route => { calls.push(route); if(failGet) throw new Error('offline'); return {unlocked:true,session:'fresh',time_probe:sample}; };
    context.captureSend = async payload => { calls.push(JSON.parse(JSON.stringify(payload))); };
    run("request=captureRequest;sendCommand=captureSend;status={unlocked:true,session:'old'};connected=true;busy=false;");
    await run('syncBrowserTime()');
    assert.equal(calls[0], '/api/status');
    assert.equal(calls[1].command,'SYNC_TIME');
    assert.equal(calls[1].value,Math.floor((base+80)/1000));
    return calls[1];
  }
  const good=await sync();
  assert.equal(good.reference_probe_id,17);
  assert.equal(good.reference_utc_us,1700000000020000);
  assert.equal(good.reference_rtt_us,40001);
  assert.equal(good.reference_clock_delta_us,40000);
  assert.match(elements.get('notice').textContent,/不保证/);
  for(const options of [
    {sample:undefined,failGet:true},{sample:null},{sample:{...probe,valid:false}},
    {sample:{...probe,id:0}},{sample:{...probe,id:2**32}},
    {sample:{...probe,hrt_us:-1}},{sample:{...probe,window_us:1001}},
    {sample:{...probe,window_us:0.5}},{sample:{...probe,hrt_us:Infinity}},
    {wall:[base,base+141,base+80]}, {mono:[100,2101]}, {mono:[140,100]},
    {performance:false},{wall:[0,40,base+80]}
  ]) {
    const legacy=await sync(options);
    assert.equal('reference_probe_id' in legacy,false);
    assert.match(elements.get('notice').textContent,/未提供/);
  }
  let sends=0;
  context.captureSend=async()=>{sends++;};
  run('sendCommand=captureSend;connected=false;');
  await run('syncBrowserTime()');
  assert.equal(sends,0);
  context.request=realRequest;context.sendCommand=realSend;
  context.updateStatus=realUpdate;context.loadAccounts=realLoad;
  const wall=[base,base+40,base+80];
  context.Date=class extends Date {static now(){return wall.length ? wall.shift() : base+80;}};
  const mono=[100,140];context.performance={now:()=>mono.shift()};
  const routes=[];let posted;
  const fullStatus={unlocked:true,session:'fresh-session',time:1700000000,device_id:'test',ssid:'test',heap:100000,largest_block:50000,brightness:80,sound:true,time_probe:probe,results:[{id:91,ok:true}]};
  context.fetch=async(route,options)=>{
    routes.push(route);
    if(route==='/api/command') {
      assert.equal(options.method,'POST');
      assert.equal(options.headers['X-Passport-Session'],'fresh-session');
      posted=JSON.parse(options.body);return {ok:true,json:async()=>({queued:true,id:91})};
    }
    return {ok:true,json:async()=>route==='/api/accounts' ? {revision:3,accounts:[]} : fullStatus};
  };
  context.setTimeout=callback=>{callback();};
  run("connected=true;status={unlocked:true,session:'old-session'};");
  await run('syncBrowserTime()');
  assert.deepEqual(routes,['/api/status','/api/command','/api/status','/api/accounts']);
  assert.equal(posted.reference_probe_id,17);
  assert.equal(posted.reference_utc_us,1700000000020000);
  assert.equal(posted.reference_rtt_us,40000);
  assert.equal(posted.value,1700000000);
  routes.length=0;context.setTimeout=()=>{};
  await run('poll()');
  assert.deepEqual(routes,['/api/status']);
  console.log('Web explicit time reference, clock-step and legacy fallback: PASS');
}
console.log('Web URI import, Steam conversion, validation, XSS-safe rendering: PASS');
async function historyTests() {
  assert.equal(run('typeof readRtcHistory'), 'function');
  const realRequest=run('request');
  const large='18446744073709551615';
  const row={sequence:large,source:2,utc_end_us:large,raw_us:large,raw_fraction_q19:524287,awake_delta_us:large,utc_delta_us:large,combined_uncertainty_us:large,flags:3,temperature_centi_c:-123,battery_mv:3800,temp_age_s:1,battery_age_s:2,epoch:'PRIVATE',session:'SECRET',account:'SECRET',reference_id:'PRIVATE'};
  const summary={ready:true,accepted_sequence:large,durable_sequence:'0',dropped:'0',save_budget_us:large,durable_count:0,pending_count:1,flags:0,store_status:0,save_result:0,active_ppm:-870,active_qualified:true,legacy_candidate_ppm:123,legacy_candidate_qualified:false,learning_reason:13,session:'SECRET',shadow:{version:1,source:2,result:2,samples:2,inliers:2,excluded:0,ppm:-869,uncertainty_ppm:1,holdout_baseline_error_us:2,holdout_candidate_error_us:1,holdout_present:true,holdout_improved:true,secret:'SECRET'}};
  let routes=[];
  function setup(total=100,mutate=()=>{}) {
    routes=[];
    context.captureRequest=async(route,options)=>{
      assert.equal(options.headers['X-Passport-Session'],'private-session');routes.push(route);
      const cursor=Number(new URL(route,'http://local').searchParams.get('cursor')||0);
      const rows=Array.from({length:Math.min(5,total-cursor)},()=>({...row}));
      const page={generation:large,cursor,total,next_cursor:cursor+rows.length,summary:{...summary},rows,session:'SECRET',device_id:'SECRET',time_probe:{secret:'SECRET'}};
      mutate(page,cursor);return page;
    };
    run("request=captureRequest;connected=true;status={unlocked:true,session:'private-session'};busy=false;");
  }
  setup();
  const data=await run('readRtcHistory(true)');
  assert.equal(routes.length,20);assert.equal(data.rows.length,100);
  assert.equal(data.rows[0].raw_us,large);assert.equal(data.generation,large);
  assert.match(routes[1],/cursor=5&generation=18446744073709551615/);
  const exported=JSON.stringify(data);
  for(const secret of ['SECRET','PRIVATE','session','account','device_id','time_probe','epoch','reference_id'])assert.equal(exported.includes(secret),false);
  setup(0);assert.equal((await run('readRtcHistory(true)')).rows.length,0);assert.equal(routes.length,1);
  setup();await run('readRtcHistory(false)');assert.equal(routes.length,1);
  for(const mutate of [
    p=>p.total=101,p=>p.rows.push({...row}),p=>p.next_cursor=0,p=>p.cursor=5,
    p=>p.rows[0].sequence=9007199254740992,p=>p.generation=9007199254740992,
    p=>p.rows[0].raw_us='18446744073709551616',p=>p.summary.ready=false,
    (p,c)=>{if(c)p.generation='1';},(p,c)=>{if(c)p.total=99;},
    (p,c)=>{if(c)run('status.unlocked=false');},(p,c)=>{if(c)run("status.session='changed'");}
  ]) { setup(100,mutate);await assert.rejects(()=>run('readRtcHistory(true)')); }
  setup();context.captureRequest=async()=>{throw new Error('409 stale');};run('request=captureRequest');
  await assert.rejects(()=>run('readRtcHistory(true)'),/409 stale/);
  setup();run('status.unlocked=false');await assert.rejects(()=>run('readRtcHistory(true)'));assert.equal(routes.length,0);
  setup();let downloads=0;context.Blob=class {constructor(){downloads++;}};
  context.captureRequest=async()=>{throw new Error('timeout');};run('request=captureRequest');
  await run('rtcHistoryAction(true)');assert.equal(downloads,0);assert.match(elements.get('error').textContent,/timeout/);
  assert.equal(run('busy'),false);
  let saved, revoked=0;
  context.Blob=class {constructor(parts){saved=JSON.parse(parts[0]);}};
  context.URL={createObjectURL(){return 'blob:local';},revokeObjectURL(){revoked++;}};
  setup(5);await run('rtcHistoryAction(true)');assert.equal(saved.rows.length,5);assert.equal(revoked,1);
  for(const secret of ['SECRET','PRIVATE','session','account','device_id','epoch'])assert.equal(JSON.stringify(saved).includes(secret),false);
  assert.match(elements.get('rtcSummary').textContent,/无新增睡眠证据/);
  assert.match(elements.get('rtcSummary').textContent,/未持久化/);
  assert.match(elements.get('rtcSummary').textContent,/READY（仅诊断，未激活）/);
  setup(100,(p,c)=>{if(c)throw new Error('409 stale');});saved=undefined;
  await run('rtcHistoryAction(true)');assert.equal(saved,undefined);assert.match(elements.get('error').textContent,/409 stale/);
  context.URL=URL;
  context.request=realRequest;
  console.log('Web history bounds, generation, unlock/session, string widths and secret allowlist: PASS');
}
timeReferenceTests().then(historyTests).catch(error=>{console.error(error);process.exitCode=1;});
