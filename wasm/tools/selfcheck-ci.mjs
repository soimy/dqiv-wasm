// Run the GPU self-check in a headless browser and assert its answers.
//
// Measured on 2026-10-19: every answer the self-check gives is green under SwiftShader
// (Chrome's software GL), which is what a CI runner has, so this is not a "would work in
// principle" - it is a copy of the run that worked. It is an auxiliary reference for now,
// not a gate, so the CI step runs it with continue-on-error.
//
// It needs no ROM. The loader has a "start without one" link and GameBoot.cpp:134 lets the
// game run with an empty pack on wasm, so the self-check runs with nothing handed over.
// Pass --rom only to measure the game itself as well.
//
// Usage: node selfcheck-ci.mjs --url http://localhost:8111/index.html --port 9333 [--rom file]

const arg=(name)=>{const i=process.argv.indexOf('--'+name);return i>0?process.argv[i+1]:undefined};
const URL_TO_LOAD=(arg('url')||'http://localhost:8111/index.html')+'?cb='+Date.now();
const ROM=arg('rom')||'';
const PORT=arg('port')||9333;

// The answers the self-check gives, verbatim, from the run that worked. These are pinned to
// exact numbers on purpose: byte order and the clip path are the two things most likely to
// be silently wrong, and a count that merely "looks reasonable" is how that happens.
const want=[
  'selfcheck byte order: ok',
  'selfcheck clip gradient: 0->2436, 0.5->2436, 0.8->2289, 1.0->1218, 1.5->0, again-0->2436',
  'selfcheck clip swapped: straddling-first 1218, inside-second 2436',
  'selfcheck clip two-in-one-call: 4888 (expect double of the single count)',
  'selfcheck clip order swapped: two-in-one-call-first 4888, single-second 2436',
  'selfcheck clip: inside 2436, straddling 1218, outside 0 -> ok',
  'selfcheck compose sources: ok',
  'selfcheck gpu compose: ok (blend 0, plain 0, mode2 0, affine16 0, affine8 0, affine-sub 0, obj 0/1827, obj1d 0/554, obj-sub 0/360 of 49152 differ)',
];

// .json() returns a Promise, so it has to be awaited before the field is read - reading it
// off the promise gives undefined and the WebSocket then reports "Invalid URL".
const version=await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json();
const ws=new WebSocket(version.webSocketDebuggerUrl);
let id=0;const pending=new Map();
const raw=(m,p={},sid)=>new Promise((r,j)=>{const n=++id;pending.set(n,{r,j});const msg={id:n,method:m,params:p};if(sid)msg.sessionId=sid;ws.send(JSON.stringify(msg))});
ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id&&pending.has(m.id)){const{r,j}=pending.get(m.id);pending.delete(m.id);m.error?j(new Error(JSON.stringify(m.error))):r(m.result||{})}};
await new Promise(r=>ws.onopen=r);
const race=(p,ms,l)=>Promise.race([p,new Promise((_,j)=>setTimeout(()=>j(new Error(`TIMEOUT ${l}`)),ms))]);
const ev=async(expr,ms)=>{for(let k=0;k<8;k++){try{return await race(s('Runtime.evaluate',{expression:expr,returnByValue:true,awaitPromise:true}),6000,'ev')}catch(e){await new Promise(r=>setTimeout(r,4000))}}return {result:{value:'EVAL_STALLED_after_8'}}};
const t=await race(raw('Target.createTarget',{url:'about:blank'}),8000,'create');
const a=await race(raw('Target.attachToTarget',{targetId:t.targetId,flatten:true}),8000,'attach');
const sid=a.sessionId;const s=(m,p={})=>raw(m,p,sid);
await race(s('Runtime.enable'),8000,'Runtime.enable');
await race(s('DOM.enable'),8000,'DOM.enable');
await race(s('Page.enable'),8000,'Page.enable');
// The loader fetches the .wasm by name, so a plain reload serves the cached copy
// and every measurement after a rebuild is silently the OLD binary.
await race(s('Network.enable'),8000,'Network.enable');
await race(s('Network.setCacheDisabled',{cacheDisabled:true}),8000,'cache');
await race(s('Page.navigate',{url:URL_TO_LOAD}),10000,'navigate');
await new Promise(r=>setTimeout(r,2000));
if(ROM) {
  const doc=await race(s('DOM.getDocument',{depth:-1}),6000,'getDoc');
  const node=await race(s('DOM.querySelector',{nodeId:doc.root.nodeId,selector:'#rom'}),6000,'query');
  if(!node.nodeId){console.error('NO FILE INPUT');process.exit(1)}
  await race(s('DOM.setFileInputFiles',{files:[ROM],nodeId:node.nodeId}),10000,'setFiles');
} else {
  // DOM.click does not exist in this Chrome, so the click goes through evaluate.
  const clicked=(await ev(`document.getElementById('norom').click(), 'clicked'`,8000)).result?.value;
  console.log('start without pack: '+clicked);
}
// The self-check runs during boot, so give it time, then read the journal it wrote.
let journal='';
for(let k=0;k<4;k++) {
  await new Promise(r=>setTimeout(r,15000));
  // The journal used to sit in /saves; it now lives in /tmp so that the save
  // directory's IndexedDB flush does not carry megabytes of it. Look in both.
  journal=(await ev(`(()=>{if(typeof FS==='undefined')return '(no journal)';for(const p of ['/tmp/journal.txt','/saves/journal.txt'])if(FS.analyzePath(p).exists)return new TextDecoder().decode(FS.readFile(p));return '(no journal)'})()`,12000)).result?.value||'';
  if(journal.includes('selfcheck byte order')) break;
}
if(!journal||journal==='(no journal)') {console.error('FAIL: no journal - the game never started');process.exit(1)}
const missing=want.filter(w=>!journal.includes(w));
const badError=journal.split('\n').filter(l=>/gl error 0x[1-9a-f]/i.test(l));
console.log('journal lines with selfcheck: '+journal.split('\n').filter(l=>l.includes('selfcheck')).length);
for(const w of want) console.log((journal.includes(w)?'  ok    ':'  MISSING ')+w);
for(const l of badError) console.log('  GL ERROR '+l.trim());
if(missing.length||badError.length) {console.error('FAIL: '+(missing.length?missing.length+' answer(s) missing, ':'')+(badError.length?badError.length+' GL error(s)':''));process.exit(1)}
console.log('PASS: every self-check answer is as measured (2026-10-19; the composition 2026-10-24)');
ws.close();process.exit(0);
