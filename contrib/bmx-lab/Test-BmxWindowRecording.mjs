// Offline mocks only: no @oai/sky import, native UI, recording or game launch.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomUUID } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { beginBmxWindowRecording,stepBmxWindowRecording,endBmxWindowRecording,recordBmxWindowUntilTerminal } from './Record-BmxWindow.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const fixture=path.join(root,'build','bmx','sampled-capture-fixtures',randomUUID());
await fs.mkdir(fixture,{recursive:true});
let checks=0;
function check(condition){assert.ok(condition);checks++;}
function crc32(buffer){let value=0xffffffff;for(const byte of buffer){value^=byte;for(let bit=0;bit<8;bit++)value=(value>>>1)^((value&1)?0xedb88320:0);}return (value^0xffffffff)>>>0;}
function chunk(type,bytes){const label=Buffer.from(type);const out=Buffer.alloc(bytes.length+12);out.writeUInt32BE(bytes.length,0);label.copy(out,4);bytes.copy(out,8);out.writeUInt32BE(crc32(Buffer.concat([label,bytes])),bytes.length+8);return out;}
const ihdr=Buffer.alloc(13);ihdr.writeUInt32BE(16,0);ihdr.writeUInt32BE(16,4);ihdr[8]=8;ihdr[9]=2;
const raw=Buffer.alloc(16*(16*3+1));for(let y=0;y<16;y++)for(let x=0;x<16;x++){const at=y*49+1+x*3;raw[at]=x*15;raw[at+1]=y*15;raw[at+2]=100;}
const png=Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',ihdr),chunk('IDAT',deflateSync(raw)),chunk('IEND',Buffer.alloc(0))]);
const window={app:`process:${path.join(root,'build','bmx-game','Skate.exe')}`,id:123,title:'SYNTHETIC MOCK ONLY'};
const ownedProcess={process_id:12345,process_created_utc:new Date().toISOString(),runtime_sha256:'A'.repeat(64),game_exe:path.join(root,'build','bmx-game','Skate.exe')};
const screenshot={id:'SYNTHETIC',url:`data:image/png;base64,${png.toString('base64')}`};
const base={window,workspaceRoot:root,ownedProcess,durationMs:1000,maxFrames:2,minIntervalMs:100,captureTimeoutMs:1000};
const sky={list_windows:async()=>[window],get_window_state:async()=>({window,screenshots:[screenshot]})};
try{
  const session=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'valid')});
  const first=await stepBmxWindowRecording(session);check(first.captured.length===1);
  const second=await stepBmxWindowRecording(session);check(second.captured.length===1);
  const result=await endBmxWindowRecording(session);check(result.completed && result.sampled_live && !result.full_motion_60fps && !result.visual_acceptance);
  const manifest=JSON.parse(await fs.readFile(result.manifest_path,'utf8'));
  check(manifest.samples.length===2 && manifest.samples[1].presentation_monotonic_ms>manifest.samples[0].presentation_monotonic_ms);
  check(manifest.audit.minimum_sample_gap_ms>=80 && manifest.samples[0].capture_started_utc.endsWith('Z'));
  check((await fs.readFile(path.join(fixture,'valid',manifest.samples[0].file))).equals(png));
  await assert.rejects(beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'valid')}),/EEXIST/);checks++;
  await assert.rejects(beginBmxWindowRecording({...base,sky,window:{...window,app:'process:C:\\normal\\Skate.exe'},outputDirectory:path.join(fixture,'wrong-exe')}),/isolated/);checks++;
  await assert.rejects(beginBmxWindowRecording({...base,sky:{...sky,list_windows:async()=>[window,window]},outputDirectory:path.join(fixture,'ambiguous')}),/exactly one/);checks++;
  const changed=await beginBmxWindowRecording({...base,sky:{...sky,get_window_state:async()=>({window:{...window,id:124},screenshots:[screenshot]})},outputDirectory:path.join(fixture,'changed')});
  await assert.rejects(stepBmxWindowRecording(changed),/identity changed/);checks++;
  check(changed.closed && changed.manifest.completed===false);
  const transient=await beginBmxWindowRecording({...base,sky:{...sky,get_window_state:async()=>({window,screenshots:[screenshot,screenshot]})},outputDirectory:path.join(fixture,'transient')});
  await assert.rejects(stepBmxWindowRecording(transient),/exactly one/);checks++;
  const hung=await beginBmxWindowRecording({...base,sky:{...sky,get_window_state:async()=>new Promise(()=>{})},outputDirectory:path.join(fixture,'timeout')});
  await assert.rejects(stepBmxWindowRecording(hung),/timed out/);checks++;
  check(hung.closed && hung.manifest.termination_reason==='capture_failed');
  await assert.rejects(beginBmxWindowRecording({...base,sky,outputDirectory:path.dirname(root)}),/workspace/);checks++;
  const terminalReceipt=(extra={})=>({schema_version:1,event:'bmx_native_test_terminal',terminal:true,
    scenario:'post_fall_remount',owned_pid:ownedProcess.process_id,owned_created_utc:ownedProcess.process_created_utc,
    process_identity_verified:true,terminal_utc:new Date().toISOString(),passed:true,phase:'native_evidence_complete',
    runtime_sha256:ownedProcess.runtime_sha256,...extra});
  const terminalPath=path.join(fixture,'native-terminal.json');
  let liveCount=0;
  const terminalSky={...sky,get_window_state:async()=>{
    if(++liveCount===1)await fs.writeFile(terminalPath,JSON.stringify(terminalReceipt()));
    return {window,screenshots:[screenshot]};
  }};
  const continuous=await beginBmxWindowRecording({...base,maxFrames:5,sky:terminalSky,outputDirectory:path.join(fixture,'continuous')});
  const complete=await recordBmxWindowUntilTerminal(continuous,{terminalPath,scenario:'post_fall_remount'});
  check(complete.completed&&complete.termination_reason==='native_terminal_observed'&&liveCount===2);
  check(continuous.manifest.native_terminal.final_sample_index===1&&
    Date.parse(continuous.manifest.samples[1].capture_started_utc)>=Date.parse(continuous.manifest.native_terminal.receipt.terminal_utc));
  check(continuous.manifest.samples[1].capture_started_monotonic_ms>=continuous.manifest.native_terminal.observed_monotonic_ms);
  for(const [label,extra] of Object.entries({pid:{owned_pid:54321},creation:{owned_created_utc:'2020-01-01T00:00:00.000Z'},
    hash:{runtime_sha256:'B'.repeat(64)},scenario:{scenario:'other'},phase:{phase:'native_evidence_failed'},
    stale:{terminal_utc:'2020-01-01T00:00:00.000Z'},future:{terminal_utc:'2099-01-01T00:00:00.000Z'},
    nearFuture:{terminal_utc:new Date(Date.now()+1800).toISOString()},
    offsetTime:{terminal_utc:new Date().toISOString().replace('Z','+00:00')},
    identity:{process_identity_verified:false},passed:{passed:'true'},schema:{schema_version:2}})) {
    const wrongPath=path.join(fixture,`${label}-terminal.json`);
    const wrong=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,`terminal-${label}`)});
    await fs.writeFile(wrongPath,JSON.stringify(terminalReceipt(extra)));
    await assert.rejects(recordBmxWindowUntilTerminal(wrong,{terminalPath:wrongPath,scenario:'post_fall_remount'}),/mismatch/);checks++;
    check(wrong.closed&&!wrong.manifest.completed&&wrong.manifest.samples.length===0);
  }
  const failedPath=path.join(fixture,'failed-terminal.json');
  const failedNative=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'failed-native')});
  await stepBmxWindowRecording(failedNative);
  await fs.writeFile(failedPath,JSON.stringify(terminalReceipt({passed:false,phase:'native_evidence_failed'})));
  const failedCapture=await recordBmxWindowUntilTerminal(failedNative,{terminalPath:failedPath,scenario:'post_fall_remount'});
  check(failedCapture.completed&&!failedNative.manifest.native_terminal.receipt.passed);
  // PowerShell's seven digits must remain exact for process identity while
  // terminal comparisons coexist with the capture ledger's millisecond clock.
  const psUtc=()=>new Date().toISOString().replace(/\.(\d{3})Z$/,(_,digits)=>`.${digits}0000Z`);
  const psOwner={...ownedProcess,process_created_utc:psUtc()};
  const psSession=await beginBmxWindowRecording({...base,sky,ownedProcess:psOwner,outputDirectory:path.join(fixture,'powershell-utc')});
  await stepBmxWindowRecording(psSession);
  const psPath=path.join(fixture,'powershell-terminal.json');
  await fs.writeFile(psPath,JSON.stringify(terminalReceipt({owned_created_utc:psOwner.process_created_utc,terminal_utc:psUtc()})));
  const psResult=await recordBmxWindowUntilTerminal(psSession,{terminalPath:path.relative(root,psPath),scenario:'post_fall_remount'});
  check(psResult.completed&&psSession.manifest.native_terminal.receipt.owned_created_utc===psOwner.process_created_utc);
  check(psSession.manifest.native_terminal.path===psPath);
  for(const [label,value] of Object.entries({null:null,false:false,string:'receipt',array:[]})) {
    const primitive=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,`primitive-${label}`)});
    const primitivePath=path.join(fixture,`primitive-${label}.json`);
    await fs.writeFile(primitivePath,JSON.stringify(value));
    await assert.rejects(recordBmxWindowUntilTerminal(primitive,{terminalPath:primitivePath,scenario:'post_fall_remount'}),/mismatch/);checks++;
    check(primitive.closed&&!primitive.manifest.completed&&primitive.manifest.samples.length===0);
  }
  const invalidCalendar=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'invalid-calendar')});
  invalidCalendar.manifest.capture_started_utc='2020-01-01T00:00:00.000Z'; // Synthetic clock fixture only.
  const calendarPath=path.join(fixture,'invalid-calendar.json');
  await fs.writeFile(calendarPath,JSON.stringify(terminalReceipt({terminal_utc:'2020-02-30T12:00:00.000Z'})));
  await assert.rejects(recordBmxWindowUntilTerminal(invalidCalendar,{terminalPath:calendarPath,scenario:'post_fall_remount'}),/mismatch/);checks++;
  const invalidPath=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'invalid-path')});
  await assert.rejects(recordBmxWindowUntilTerminal(invalidPath,{terminalPath:path.dirname(root),scenario:'post_fall_remount'}),/workspace/);checks++;
  check(invalidPath.closed&&!invalidPath.manifest.completed&&invalidPath.manifest.termination_reason==='terminal_capture_failed');
  // Model a file that grew after stat without racing the filesystem. The
  // recorder must bound the actual read, not rely on the earlier size alone.
  const growthPath=path.join(fixture,'grown-terminal.json');
  await fs.writeFile(growthPath,Buffer.alloc(65537,32));
  const growth=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'grown-terminal')});
  const originalOpen=fs.open;let largestRead=0,unboundedRead=false;
  fs.open=async(...args)=>{
    const handle=await originalOpen(...args);
    if(args[0]!==growthPath)return handle;
    return {stat:async()=>({isFile:()=>true,size:1}),
      read:async(buffer,offset,length,position)=>{largestRead=Math.max(largestRead,length);return handle.read(buffer,offset,length,position);},
      readFile:async()=>{unboundedRead=true;return handle.readFile();},close:()=>handle.close()};
  };
  try {
    await assert.rejects(recordBmxWindowUntilTerminal(growth,{terminalPath:growthPath,scenario:'post_fall_remount'}),/exceeds bounded read size/);checks++;
    check(largestRead>0&&largestRead<=65537&&!unboundedRead&&growth.closed);
  } finally {fs.open=originalOpen;}
  const lostWindow=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'failed-terminal-frame')});
  await stepBmxWindowRecording(lostWindow);
  const lostPath=path.join(fixture,'failed-terminal-frame.json');
  await fs.writeFile(lostPath,JSON.stringify(terminalReceipt({passed:false,phase:'native_evidence_failed'})));
  lostWindow.sky={...sky,get_window_state:async()=>({window:{...window,id:124},screenshots:[screenshot]})};
  await assert.rejects(recordBmxWindowUntilTerminal(lostWindow,{terminalPath:lostPath,scenario:'post_fall_remount'}),/identity changed/);checks++;
  check(lostWindow.closed&&!lostWindow.manifest.completed&&lostWindow.manifest.termination_reason==='capture_failed');
  check(lostWindow.manifest.native_terminal.receipt.passed===false&&lostWindow.manifest.native_terminal.final_sample_index===null);
  const lostManifest=JSON.parse(await fs.readFile(path.join(lostWindow.directory,'capture-manifest.json'),'utf8'));
  check(lostManifest.failure.includes('identity changed')&&lostManifest.native_terminal.receipt.phase==='native_evidence_failed');
  const absent=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,'no-terminal')});
  await assert.rejects(recordBmxWindowUntilTerminal(absent,{terminalPath:path.join(fixture,'absent.json'),scenario:'post_fall_remount'}),/limit/);checks++;
  check(!absent.manifest.completed&&absent.manifest.termination_reason==='terminal_capture_failed');
  // Fault injection at the real publication boundary, with real synthetic
  // frame files. No window capture, UI, native inputs or game process exists.
  const renameSession=async(label)=>{
    const value=await beginBmxWindowRecording({...base,sky,outputDirectory:path.join(fixture,label)});
    await stepBmxWindowRecording(value);await stepBmxWindowRecording(value);return value;
  };
  for(const code of ['EPERM','EACCES','EBUSY']) {
    const value=await renameSession(`transient-rename-${code}`);
    const main=path.join(value.directory,'capture-manifest.json'),pending=path.join(value.directory,'capture-manifest.pending.json');
    const originalRename=fs.rename,originalWrite=fs.writeFile,originalTimer=globalThis.setTimeout;
    const originalMain=await fs.readFile(main);let attempts=0,writes=0;const waits=[],pendingBytes=[];
    fs.writeFile=async(...args)=>{if(args[0]===pending)writes++;return originalWrite(...args);};
    fs.rename=async(...args)=>{
      if(args[0]!==pending)return originalRename(...args);
      attempts++;pendingBytes.push(await fs.readFile(pending));
      if(attempts<=2){assert.deepEqual(await fs.readFile(main),originalMain);throw Object.assign(new Error('synthetic Windows sharing denial'),{code});}
      return originalRename(...args);
    };
    globalThis.setTimeout=(fn,ms,...args)=>{waits.push(ms);return originalTimer(fn,0,...args);};
    try {
      if(process.platform==='win32') {
        const result=await endBmxWindowRecording(value);
        check(result.completed&&value.footerPersisted&&attempts===3&&writes===1);
        assert.deepEqual(waits,[20,40]);checks++;
        check(pendingBytes.every(bytes=>bytes.equals(pendingBytes[0])));
        const closed=JSON.parse(await fs.readFile(main,'utf8'));
        check(closed.completed&&closed.samples.length===2&&closed.capture_ended_utc===value.manifest.capture_ended_utc);
      } else {
        await assert.rejects(endBmxWindowRecording(value),error=>error.code===code);checks++;
        check(attempts===1&&waits.length===0&&!value.footerPersisted);
      }
    } finally {fs.rename=originalRename;fs.writeFile=originalWrite;globalThis.setTimeout=originalTimer;}
  }
  {
    const value=await renameSession('exhausted-final-rename');
    const main=path.join(value.directory,'capture-manifest.json'),pending=path.join(value.directory,'capture-manifest.pending.json');
    const originalMain=await fs.readFile(main),originalRename=fs.rename,originalTimer=globalThis.setTimeout;
    const waits=[];let attempts=0;
    fs.rename=async(...args)=>{if(args[0]!==pending)return originalRename(...args);attempts++;throw Object.assign(new Error('persistent synthetic sharing denial'),{code:'EPERM'});};
    globalThis.setTimeout=(fn,ms,...args)=>{waits.push(ms);return originalTimer(fn,0,...args);};
    let ended;
    try {
      await assert.rejects(endBmxWindowRecording(value),error=>error.code==='EPERM');checks++;
      ended=value.manifest.capture_ended_utc;
      check(value.closed&&!value.footerPersisted&&value.manifest.completed&&ended);
      check(attempts===(process.platform==='win32'?6:1));
      assert.deepEqual(waits,process.platform==='win32'?[20,40,80,160,320]:[]);checks++;
      assert.deepEqual(await fs.readFile(main),originalMain);checks++;
      const stranded=JSON.parse(await fs.readFile(pending,'utf8'));
      check(stranded.completed&&stranded.samples.length===2&&stranded.capture_ended_utc===ended);
      // A second end while the lock persists must reject, not report the
      // in-memory completed flag as a successfully published final manifest.
      await assert.rejects(endBmxWindowRecording(value),error=>error.code==='EPERM');checks++;
      check(attempts===(process.platform==='win32'?12:2)&&!value.footerPersisted);
      assert.deepEqual(await fs.readFile(main),originalMain);checks++;
    } finally {fs.rename=originalRename;globalThis.setTimeout=originalTimer;}
    const recovered=await endBmxWindowRecording(value);
    check(recovered.completed&&value.footerPersisted&&value.manifest.capture_ended_utc===ended);
    const final=JSON.parse(await fs.readFile(main,'utf8'));
    check(final.completed&&final.capture_ended_utc===ended&&final.samples.length===2);
    await assert.rejects(fs.stat(pending),error=>error.code==='ENOENT');checks++;
  }
  {
    const value=await renameSession('non-sharing-rename-error');
    const pending=path.join(value.directory,'capture-manifest.pending.json'),originalRename=fs.rename;
    const sentinel=Object.assign(new Error('synthetic disk failure'),{code:'EIO'});let attempts=0;
    fs.rename=async(...args)=>{if(args[0]!==pending)return originalRename(...args);attempts++;throw sentinel;};
    try {
      await assert.rejects(endBmxWindowRecording(value),error=>error===sentinel);checks++;
      check(attempts===1&&!value.footerPersisted);
    } finally {fs.rename=originalRename;}
  }
  console.log(`Sampled recorder offline mocks PASS: ${checks} checks; no real capture/input/game was run.`);
}finally{
  const checked=await fs.realpath(fixture);
  assert.equal(path.dirname(checked),path.join(root,'build','bmx','sampled-capture-fixtures'));
  await fs.rm(checked,{recursive:true,force:true});
}
