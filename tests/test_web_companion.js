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
  assert.equal(run('typeof timeSyncSample'), 'function');
  assert.equal(run('typeof selectTimeSyncReference'), 'function');
  const base = 1700000000000;
  function sample(id=1, up=4000, down=4000, processing=20000, offset=0) {
    const elapsed = up + processing + down;
    return {probe:{version:2,id,received_us:up,sent_us:up+processing,valid:true},
      wallBeforeMs:base+offset/1000,wallAfterMs:base+(offset+elapsed)/1000,
      monoBeforeMs:100,monoAfterMs:100+elapsed/1000};
  }
  const evaluate = value => run(`timeSyncSample(${JSON.stringify(value)})`);
  const good=evaluate(sample());
  assert.equal(good.networkUs,8000);
  assert.equal(good.offsetUs,base*1000);
  assert.equal(good.rttUs,28000);
  const slow=evaluate(sample(2,4000,4000,200000));
  assert.equal(slow.networkUs,8000);assert.equal(slow.offsetUs,base*1000);
  const samples=Array.from({length:12},(_,i)=>evaluate(sample(i+1,4000,4000,20000,(i%3-1)*500)));
  samples[0]=evaluate(sample(1,100000,4000));
  context.timeSamples=samples;
  const chosen=run('selectTimeSyncReference(timeSamples)');
  assert.equal(chosen.payload.reference_version,2);
  assert.equal(chosen.payload.reference_probe_id,5); // newest of the fastest third
  assert.equal(chosen.payload.reference_utc_us,base*1000+14000);
  assert.equal(chosen.transportUs,5500);
  const asymmetric=evaluate(sample(10,2000,14000));
  assert.equal(asymmetric.offsetUs-base*1000,6000);
  context.timeSamples=Array.from({length:6},()=>asymmetric);
  const asym=run('selectTimeSyncReference(timeSamples)');
  assert.ok(asym.transportUs>=Math.abs(asymmetric.offsetUs-base*1000));
  context.timeSamples=samples.slice(0,5);
  assert.throws(()=>run('selectTimeSyncReference(timeSamples)'),/样本/);
  context.timeSamples=Array.from({length:6},(_,i)=>evaluate(sample(i+1,4000,4000,20000,i*30000)));
  assert.throws(()=>run('selectTimeSyncReference(timeSamples)'),/稳定/);
  for (const mutate of [
    x=>x.probe.valid=false,x=>x.probe.version=1,x=>x.probe.id=0,
    x=>x.probe.received_us=-1,x=>x.probe.sent_us=1,
    x=>x.probe.sent_us=2**53,x=>x.probe.sent_us=1.5,
    x=>x.monoAfterMs=99,x=>x.monoAfterMs=1000,
  ]) { const x=sample();mutate(x);assert.equal(evaluate(x),null); }
  assert.throws(()=>evaluate({...sample(),wallAfterMs:base+1000}),/时间/);

  const realRequest=run('request'),realSend=run('sendCommand');
  const realUpdate=run('updateStatus'),realLoad=run('loadAccounts');
  let mono=1000,wall=base,index=0,calls=[],posts=[],mode='good',pending;
  context.Date=class extends Date {static now(){return wall;}};
  context.performance={now:()=>mono};
  function reset(next='good') {
    mode=next;mono=1000;wall=base;index=0;calls=[];posts=[];document.hidden=next==='hidden';
    run("connected=true;status={unlocked:true,session:'original',device_id:'test'};busy=false;");
  }
  context.captureRequest=async route=>{
    calls.push(route);
    if(route==='/api/status')return {unlocked:mode!=='locked',session:mode==='session'?'changed':'original',device_id:'test'};
    assert.equal(route,'/api/time-probe');
    assert.equal(run('busy'),true);
    if(mode==='pending' && index===0){await new Promise(resolve=>pending=resolve);}
    index++;
    if(mode==='midlock' && index===4)run('status.unlocked=false');
    if(mode==='midsession' && index===4)run("status.session='changed'");
    if(mode==='failedstep' && index===4){wall+=1000;throw new Error('timeout');}
    if(mode==='offline')throw new Error('timeout');
    const received=Math.round(mono*1000)+4000;
    mono+=28;wall+=28;
    if(mode==='clockstep' && index===4)wall+=1000;
    return {version:2,id:index,received_us:received,sent_us:received+20000,valid:mode!=='invalid'};
  };
  context.captureSend=async payload=>{posts.push(JSON.parse(JSON.stringify(payload)));};
  run('request=captureRequest;sendCommand=captureSend;');
  reset();await run('syncBrowserTime()');
  assert.equal(posts.length,1);assert.equal(index,12);
  assert.equal(posts[0].reference_version,2);
  assert.equal(posts[0].reference_probe_id,4);
  assert.equal(posts[0].reference_rtt_us,28000);
  assert.match(elements.get('notice').textContent,/手机/);
  assert.equal(run('busy'),false);
  for(const bad of ['offline','invalid','clockstep','session','locked','hidden','midlock','midsession','failedstep']) {
    reset(bad);await run('syncBrowserTime()');
    assert.equal(posts.length,0,bad);assert.equal(run('busy'),false);
    assert.ok(elements.get('error').textContent,bad);
  }
  reset('pending');const first=run('syncBrowserTime()');
  await Promise.resolve();await run('syncBrowserTime()');
  assert.equal(run('busy'),true);assert.equal(posts.length,0);
  pending();await first;assert.equal(posts.length,1);assert.equal(run('busy'),false);

  // Run real POST + acknowledgement path, with one POST and authenticated probes.
  context.request=realRequest;context.sendCommand=realSend;
  context.updateStatus=realUpdate;context.loadAccounts=realLoad;
  context.AbortController=AbortController;context.clearTimeout=clearTimeout;
  context.setTimeout=(callback,ms)=>ms===200 ? setTimeout(callback,0) : setTimeout(callback,ms);
  const fullStatus={unlocked:true,session:'original',time:1700000000,device_id:'test',ssid:'test',heap:100000,largest_block:50000,brightness:80,sound:true,results:[{id:91,ok:true}]};
  context.fetch=async(route,options)=>{
    calls.push(route);
    if(route==='/api/time-probe') {
      if((mode==='http403'||mode==='http423') && index===4)return {ok:false,status:mode==='http403'?403:423,text:async()=> 'Session or lock changed'};
      assert.equal(options.headers['X-Passport-Session'],'original');
      const received=Math.round(mono*1000)+4000;mono+=28;wall+=28;index++;
      return {ok:true,json:async()=>({version:2,id:index,received_us:received,sent_us:received+20000,valid:true})};
    }
    if(route==='/api/command') {
      posts.push(JSON.parse(options.body));
      if(mode==='postfailure')throw new Error('ambiguous POST');
      return {ok:true,json:async()=>({queued:true,id:91})};
    }
    if(mode==='resulttimeout' && route==='/api/status' && posts.length){mono+=7000;wall-=7000;return {ok:true,json:async()=>({...fullStatus,results:[]})};}
    return {ok:true,json:async()=>route==='/api/accounts'?{revision:3,accounts:[]}:fullStatus};
  };
  for(const next of ['good','postfailure','resulttimeout']) {
    reset(next);await run('syncBrowserTime()');
    assert.equal(posts.length,1,next);assert.equal(run('busy'),false);
    if(next!=='good')assert.ok(elements.get('error').textContent,next);
  }
  for(const next of ['http403','http423']) {
    reset(next);await run('syncBrowserTime()');
    assert.equal(posts.length,0);assert.equal(index,4);assert.equal(run('busy'),false);
  }
  reset('stalledprobe');let aborted=0;
  context.fetch=async route=>route==='/api/status' ? {ok:true,json:async()=>fullStatus} : new Promise(()=>{});
  context.setTimeout=(callback,ms)=>setTimeout(()=>{mono+=ms;wall+=ms;aborted++;callback();},0);
  await run('syncBrowserTime()');
  assert.equal(posts.length,0);assert.equal(run('busy'),false);assert.equal(aborted,6);
  context.setTimeout=setTimeout;
  // Deadline includes a response body that never settles, even without AbortSignal.timeout.
  context.fetch=async()=>({ok:true,json:()=>new Promise(()=>{})});
  await assert.rejects(()=>run("request('/api/time-probe',{},5)"),/超时/);
  context.fetch=()=>new Promise(()=>{});
  await assert.rejects(()=>run("request('/api/time-probe',{},5)"),/超时/);
  context.setTimeout=()=>{};
  console.log('Web four-timestamp selection, clock steps, bounded requests, one POST and cleanup: PASS');
}
console.log('Web URI import, Steam conversion, validation, XSS-safe rendering: PASS');
async function historyTests() {
  assert.equal(run('typeof readRtcHistory'), 'function');
  const realRequest=run('request');
  assert.notEqual(run("formatHttpError(503,'RTC_HISTORY_IO_ERROR')"), run("formatHttpError(503,'busy')"));
  assert.notEqual(run("formatHttpError(503,'RTC_HISTORY_BLOCKED')"), run("formatHttpError(503,'RTC_HISTORY_IO_ERROR')"));
  assert.notEqual(run("formatHttpError(503,'RTC_HISTORY_NOT_READY')"), run("formatHttpError(423,'')"));
  assert.equal(run("formatHttpError(503,'busy').includes('锁屏')"), false);
  assert.match(run("formatHttpError(422,'RTC_HISTORY_NOT_APPLICABLE')"), /石英/);
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
  setup();context.captureRequest=async()=>{const e=new Error('mode');e.status=422;e.code='RTC_HISTORY_NOT_APPLICABLE';throw e;};run('request=captureRequest');
  await run('rtcHistoryAction(true)');assert.equal(downloads,0);
  assert.equal(elements.get('error').textContent,'');assert.match(elements.get('rtcSummary').textContent,/石英/);
  assert.equal(run('busy'),false);
  let saved, revoked=0;
  context.Blob=class {constructor(parts){saved=JSON.parse(parts[0]);}};
  context.URL={createObjectURL(){return 'blob:local';},revokeObjectURL(){revoked++;}};
  setup(5);await run('rtcHistoryAction(true)');assert.equal(saved.rows.length,5);assert.equal(revoked,1);
  for(const secret of ['SECRET','PRIVATE','session','account','device_id','epoch'])assert.equal(JSON.stringify(saved).includes(secret),false);
  assert.match(elements.get('rtcSummary').textContent,/无新增睡眠证据/);
  assert.match(elements.get('rtcSummary').textContent,/未持久化/);
  assert.match(elements.get('rtcSummary').textContent,/多窗口估计 READY/);
  setup(100,(p,c)=>{if(c)throw new Error('409 stale');});saved=undefined;
  await run('rtcHistoryAction(true)');assert.equal(saved,undefined);assert.match(elements.get('error').textContent,/409 stale/);
  const calibration={quality:3,estimator_version:3,evidence_sequence:'3',evidence_utc_us:'1700000000000000',uncertainty_millippm:872000,uncertainty_valid:true,secret:'SECRET',epoch:'PRIVATE'};
  const calibrationPage=p=>Object.assign(p.summary,{active_ppm:-7165,calibration:{...calibration},coefficient_saved:false,coefficient_save_blocked:false,saved_ppm:-870,coefficient_save_budget_us:'86400000000'});
  setup(5,calibrationPage);
  const calibrated=await run('readRtcHistory(true)');
  assert.equal(calibrated.summary.calibration?.quality,3);
  assert.equal(calibrated.summary.calibration.evidence_sequence,'3');
  assert.equal(calibrated.summary.coefficient_save_budget_us,'86400000000');
  assert.equal(calibrated.summary.coefficient_saved,false);
  assert.equal(JSON.stringify(calibrated).includes('SECRET'),false);
  assert.equal(JSON.stringify(calibrated).includes('PRIVATE'),false);
  context.calibratedHistory=calibrated;run('renderRtcHistory(calibratedHistory)');
  assert.match(elements.get('rtcSummary').textContent,/RECOVERY/);
  assert.match(elements.get('rtcSummary').textContent,/872/);
  for(const mutate of [
    c=>c.quality=4,c=>c.quality=1.5,c=>c.uncertainty_millippm=-1,c=>c.uncertainty_valid='true',
    c=>c.evidence_sequence=9007199254740992,c=>c.evidence_utc_us='18446744073709551616',
    c=>delete c.estimator_version,c=>c.estimator_version=2,c=>c.evidence_sequence='0',
    c=>c.evidence_utc_us='1',c=>c.uncertainty_valid=false,c=>c.uncertainty_millippm=1.5
  ]){
    setup(5,p=>{calibrationPage(p);mutate(p.summary.calibration);});
    await assert.rejects(()=>run('readRtcHistory(true)'));
  }
  for(const mutate of [s=>s.calibration=null,s=>delete s.calibration,s=>s.coefficient_saved=true,
    s=>s.saved_ppm=20001,s=>s.saved_ppm=0.5,s=>delete s.saved_ppm,
    s=>s.coefficient_save_budget_us='86400000001',s=>s.coefficient_save_blocked=1]) {
    setup(5,p=>{calibrationPage(p);mutate(p.summary);});
    await assert.rejects(()=>run('readRtcHistory(true)'));
  }
  setup(5,p=>{calibrationPage(p);p.summary.saved_ppm=null;});
  assert.equal((await run('readRtcHistory(true)')).summary.saved_ppm,null);
  setup(5,p=>{calibrationPage(p);p.summary.saved_ppm=-7165;p.summary.coefficient_saved=true;});
  assert.equal((await run('readRtcHistory(true)')).summary.coefficient_saved,true);
  setup(5);await run('rtcHistoryAction(false)'); /* Older firmware remains readable. */
  assert.match(elements.get('rtcSummary').textContent,/未知/);
  context.URL=URL;
  context.request=realRequest;
  console.log('Web history bounds, generation, unlock/session, string widths and secret allowlist: PASS');
}
timeReferenceTests().then(historyTests).catch(error=>{console.error(error);process.exitCode=1;});
