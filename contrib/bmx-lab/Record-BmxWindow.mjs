// Import this module from node_repl and pass its initialized @oai/sky client.
// Only sky captures windows. Filesystem access solely persists the requested
// sampled-live video source frames and their audit trail; no OS input is sent.
import fs from 'node:fs/promises';
import path from 'node:path';
import { performance } from 'node:perf_hooks';
import { createHash } from 'node:crypto';

const PNG = Buffer.from([137,80,78,71,13,10,26,10]);
const utc = () => new Date().toISOString();
// Accept UTC instants emitted by JavaScript (3 fractional digits) and
// PowerShell DateTime.ToString('o') (7), without Date.parse's local-time or
// invalid-calendar-date normalization. Sub-millisecond digits remain in the
// stored receipt; comparisons use the capture ledger's millisecond precision.
function utcMilliseconds(value) {
  const match=typeof value==='string' && /^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,7}))?Z$/.exec(value);
  if(!match)return NaN;
  const normalized=`${match[1]}.${(match[2]??'').padEnd(3,'0').slice(0,3)}Z`;
  const result=Date.parse(normalized);
  return Number.isFinite(result)&&new Date(result).toISOString()===normalized ? result : NaN;
}
const clampNumber = (value, minimum, maximum, name) => {
  if (!Number.isFinite(value) || value < minimum || value > maximum) throw new Error(`${name} must be ${minimum}..${maximum}`);
  return value;
};
const timeout = async (promise, milliseconds, label) => {
  let timer;
  try { return await Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(`${label} timed out; capture stopped`)), milliseconds); })]); }
  finally { clearTimeout(timer); }
};
const sameWindow = (a,b) => a && b && Number.isSafeInteger(a.id) && a.id === b.id && a.app === b.app;
function imagePayload(url) {
  if (typeof url !== 'string') throw new Error('Expected a returned image data URL');
  if (url.startsWith('data:image/png;base64,')) {
    const bytes=Buffer.from(url.slice('data:image/png;base64,'.length),'base64');
    if (bytes.length < 33 || !bytes.subarray(0,8).equals(PNG) || bytes.toString('ascii',12,16) !== 'IHDR') throw new Error('Returned PNG header is invalid');
    return {bytes,width:bytes.readUInt32BE(16),height:bytes.readUInt32BE(20),extension:'png',mime:'image/png'};
  }
  if (url.startsWith('data:image/jpeg;base64,')) {
    const bytes=Buffer.from(url.slice('data:image/jpeg;base64,'.length),'base64');
    if (bytes.length < 4 || bytes[0]!==0xff || bytes[1]!==0xd8 || bytes.at(-2)!==0xff || bytes.at(-1)!==0xd9) throw new Error('Returned JPEG header/end is invalid');
    let offset=2;
    while (offset+4 < bytes.length) {
      if (bytes[offset++]!==0xff) throw new Error('Returned JPEG marker is invalid');
      while (bytes[offset]===0xff) offset++;
      const marker=bytes[offset++];
      if (marker===0xda || marker===0xd9) break;
      if (marker===0x01 || (marker>=0xd0 && marker<=0xd8)) continue;
      const length=bytes.readUInt16BE(offset);
      if (length < 2 || offset+length>bytes.length) throw new Error('Returned JPEG segment is invalid');
      if ([0xc0,0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc9,0xca,0xcb,0xcd,0xce,0xcf].includes(marker)) {
        if (length < 8) throw new Error('Returned JPEG frame header is invalid');
        return {bytes,width:bytes.readUInt16BE(offset+5),height:bytes.readUInt16BE(offset+3),extension:'jpg',mime:'image/jpeg'};
      }
      offset+=length;
    }
    throw new Error('Returned JPEG frame dimensions are unavailable');
  }
  throw new Error('Expected a returned PNG or JPEG data URL');
}

async function workspacePath(root, requested) {
  const full = path.resolve(root,requested);
  const relation = path.relative(root, full);
  if (!relation || relation.startsWith('..') || path.isAbsolute(relation)) throw new Error('Capture output must be a child of the workspace');
  let cursor = full;
  while (cursor !== root) {
    try { if ((await fs.lstat(cursor)).isSymbolicLink()) throw new Error(`Capture path contains a junction/symlink: ${cursor}`); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    const parent = path.dirname(cursor); if (parent === cursor) throw new Error('Capture path escaped workspace'); cursor = parent;
  }
  return full;
}

// Windows readers/AV may briefly deny replacement of an existing manifest.
// Keep the same pending bytes and atomic rename; never unlink/copy the old
// evidence as a fallback. Six attempts add at most620ms of requested backoff.
const manifestRenameBackoffMs = [20,40,80,160,320];
async function publishManifest(pending, destination) {
  for (let attempt=0;;attempt++) {
    try { await fs.rename(pending,destination); return; }
    catch (error) {
      if (process.platform!=='win32' || !['EPERM','EACCES','EBUSY'].includes(error?.code) ||
          attempt>=manifestRenameBackoffMs.length) throw error;
      await new Promise(resolve=>setTimeout(resolve,manifestRenameBackoffMs[attempt]));
    }
  }
}

async function persist(session) {
  // A closed in-memory session is not a published final footer. If publication
  // fails, a later explicit end() may retry it but cannot return false success.
  session.footerPersisted=false;
  const samples = session.manifest.samples;
  const gaps = samples.slice(1).map((sample,i) => sample.presentation_monotonic_ms - samples[i].presentation_monotonic_ms);
  session.manifest.audit = {
    sample_count: samples.length, bytes: session.totalBytes,
    sample_rate_hz: samples.length > 1 ? (samples.length-1)*1000/(samples.at(-1).presentation_monotonic_ms-samples[0].presentation_monotonic_ms) : null,
    maximum_sample_gap_ms: gaps.length ? Math.max(...gaps) : null,
    minimum_sample_gap_ms: gaps.length ? Math.min(...gaps) : null,
    maximum_capture_latency_ms: samples.length ? Math.max(...samples.map(s => s.capture_ended_monotonic_ms-s.capture_started_monotonic_ms)) : null,
  };
  const pending = path.join(session.directory, 'capture-manifest.pending.json');
  await fs.writeFile(pending, JSON.stringify(session.manifest,null,2));
  await publishManifest(pending,path.join(session.directory,'capture-manifest.json'));
  session.footerPersisted=session.closed;
}

export async function beginBmxWindowRecording({ sky, window, workspaceRoot, outputDirectory,
  ownedProcess, durationMs=120000, maxFrames=240, minIntervalMs=500, captureTimeoutMs=15000, maximumBytes=536870912 }) {
  if (!sky || typeof sky.list_windows !== 'function' || typeof sky.get_window_state !== 'function') throw new Error('Pass the initialized @oai/sky client from node_repl');
  if (!window || !Number.isSafeInteger(window.id) || typeof window.app !== 'string') throw new Error('Pass exactly one actual returned window');
  const root = await fs.realpath(path.resolve(workspaceRoot));
  const isolatedExe = path.join(root,'build','bmx-game','Skate.exe');
  const expectedApp = `process:${isolatedExe}`;
  if (window.app.toLowerCase() !== expectedApp.toLowerCase()) throw new Error('Selected app is not the exact isolated Skate executable');
  if (!ownedProcess || !Number.isSafeInteger(ownedProcess.process_id) || ownedProcess.process_id <= 0 ||
      !Number.isFinite(utcMilliseconds(ownedProcess.process_created_utc)) || !/^[a-f0-9]{64}$/i.test(ownedProcess.runtime_sha256 ?? '') ||
      path.resolve(ownedProcess.game_exe ?? '').toLowerCase() !== isolatedExe.toLowerCase()) throw new Error('Pass the externally verified supervisor PID, creation time, executable and runtime hash');
  clampNumber(durationMs,1000,600000,'durationMs'); clampNumber(maxFrames,2,2000,'maxFrames');
  clampNumber(minIntervalMs,100,10000,'minIntervalMs'); clampNumber(captureTimeoutMs,1000,30000,'captureTimeoutMs');
  clampNumber(maximumBytes,1048576,2147483648,'maximumBytes');
  if (!Number.isInteger(maxFrames)) throw new Error('maxFrames must be an integer');
  const candidates = (await timeout(sky.list_windows(),captureTimeoutMs,'Window inventory')).filter(candidate => sameWindow(candidate,window));
  if (candidates.length !== 1) throw new Error(`Expected exactly one live returned game window; found ${candidates.length}`);
  const directory = await workspacePath(root,outputDirectory);
  await fs.mkdir(path.dirname(directory),{recursive:true});
  await fs.mkdir(directory); // Fresh directory only; EEXIST never overwrites a run.
  const start = performance.now();
  const session = {
    sky, window:candidates[0], directory, workspaceRoot:root, start, lastRequest:null, totalBytes:0, busy:false, closed:false,
    durationMs,maxFrames,minIntervalMs,captureTimeoutMs,maximumBytes,footerPersisted:false,
    manifest: {
      schema:1,event:'bmx_sampled_live_window_capture',capture_method:'@oai/sky sampled live game window',
      capture_started_utc:utc(),capture_ended_utc:null,completed:false,failure:null,termination_reason:null,
      source_window:{app:candidates[0].app,id:candidates[0].id,title:candidates[0].title ?? null},
      owned_process:{...ownedProcess,sky_pid_binding:'External supervisor identity supplied by caller; sky independently revalidates exact executable app and window id only.'},
      limits:{duration_ms:durationMs,max_frames:maxFrames,min_interval_ms:minIntervalMs,capture_timeout_ms:captureTimeoutMs,maximum_bytes:maximumBytes},
      timing_basis:'Each image was captured within its sky request/return interval; VFR presentation uses the interval midpoint. Native simulation time and exact screen scanout time are not exposed.',
      sampled_live:true,full_motion_60fps:false,visual_acceptance:false,os_input_injected:false,samples:[],audit:null,
    },
  };
  await persist(session);
  return session;
}

export async function stepBmxWindowRecording(session,{frames=1,chunkDurationMs=15000}={}) {
  if (session.closed || session.busy) throw new Error('Capture session is closed or already capturing');
  if (!Number.isInteger(frames) || frames < 1 || frames > 100) throw new Error('frames must be 1..100 per chunk');
  clampNumber(chunkDurationMs,100,30000,'chunkDurationMs');
  const chunkStart=performance.now(); const captured=[]; session.busy=true;
  try {
    while (captured.length < frames && performance.now()-chunkStart < chunkDurationMs) {
      if (session.manifest.samples.length >= session.maxFrames || performance.now()-session.start >= session.durationMs) {
        session.manifest.termination_reason=session.manifest.samples.length >= session.maxFrames ? 'maximum_frames' : 'duration_elapsed'; break;
      }
      if (session.lastRequest !== null) {
        const remaining=session.minIntervalMs-(performance.now()-session.lastRequest);
        if (remaining > 0) await new Promise(resolve => setTimeout(resolve,remaining));
      }
      if (performance.now()-session.start >= session.durationMs) { session.manifest.termination_reason='duration_elapsed'; break; }
      const requestAt=performance.now(); const requestUtc=utc(); session.lastRequest=requestAt;
      const state=await timeout(session.sky.get_window_state({window:session.window,include_screenshot:true,include_text:false}),
        Math.min(session.captureTimeoutMs,Math.max(1,session.durationMs-(requestAt-session.start))),'Game window capture');
      const returnAt=performance.now(); const returnedUtc=utc();
      if (!sameWindow(state?.window,session.window)) throw new Error('Returned app/window identity changed; no desktop or alternate window fallback');
      if (!Array.isArray(state.screenshots) || state.screenshots.length !== 1) throw new Error('Expected exactly one bounded game-window image; transient/ambiguous screenshots rejected');
      const screenshot=state.screenshots[0];
      const {bytes,width,height,extension,mime}=imagePayload(screenshot.url);
      if (width < 16 || height < 16 || width > 8192 || height > 8192 || bytes.length > 33554432) throw new Error('Returned game image dimensions/size exceed bounded capture limits');
      if (session.totalBytes+bytes.length > session.maximumBytes) throw new Error('Maximum capture storage reached');
      const index=session.manifest.samples.length;
      const file=`frame-${String(index).padStart(6,'0')}.${extension}`;
      await fs.writeFile(path.join(session.directory,file),bytes,{flag:'wx'});
      const sample={index,file,mime,sha256:createHash('sha256').update(bytes).digest('hex'),bytes:bytes.length,width,height,
        screenshot_id:screenshot.id,window_id:state.window.id,app:state.window.app,
        capture_started_utc:requestUtc,capture_ended_utc:returnedUtc,
        capture_started_monotonic_ms:requestAt-session.start,capture_ended_monotonic_ms:returnAt-session.start,
        presentation_monotonic_ms:(requestAt+returnAt)/2-session.start};
      session.totalBytes+=bytes.length; session.window=state.window; session.manifest.samples.push(sample); captured.push(sample);
      await persist(session);
    }
    return {captured,done:session.manifest.termination_reason!==null,audit:session.manifest.audit,
      manifest_path:path.join(session.directory,'capture-manifest.json')};
  } catch (error) {
    session.manifest.failure=String(error.message ?? error); session.manifest.termination_reason='capture_failed';
    session.manifest.capture_ended_utc=utc(); session.closed=true; await persist(session); throw error;
  } finally { session.busy=false; }
}

export async function endBmxWindowRecording(session,{reason='caller_finished'}={}) {
  if (session.busy) throw new Error('Await the active capture chunk before ending');
  if (!session.closed) {
    session.closed=true;session.manifest.capture_ended_utc=utc();
    session.manifest.termination_reason ??= reason;
    session.manifest.completed=session.manifest.failure === null && session.manifest.samples.length >= 2;
  }
  if(!session.footerPersisted)await persist(session);
  return {manifest_path:path.join(session.directory,'capture-manifest.json'),completed:session.manifest.completed,
    termination_reason:session.manifest.termination_reason,audit:session.manifest.audit,sampled_live:true,full_motion_60fps:false,visual_acceptance:false};
}

// Run this as one awaited node_repl cell. Returning between small chunks can
// leave long gaps while the model reviews other tools. The supervisor publishes
// its atomic data-only receipt BEFORE its capture grace period and teardown.
export async function recordBmxWindowUntilTerminal(session,{terminalPath,scenario}={}) {
  if(session.closed||session.busy)throw new Error('Capture session is closed or already capturing');
  try {
    if(typeof terminalPath!=='string'||!terminalPath)throw new Error('Expected the supervisor terminal receipt path');
    const terminal=await workspacePath(session.workspaceRoot,terminalPath);
    if(typeof scenario!=='string'||!scenario)throw new Error('Expected the supervisor scenario');
    for(;;) {
      let receipt=null,receiptBytes=null;
      try {
        const file=await fs.open(terminal,'r');
        try {
          const stat=await file.stat();
          if(!stat.isFile()||stat.size>65536)throw new Error('Native terminal receipt is not a bounded regular file');
          // Bound the read itself, including a file that grows after stat.
          const buffer=Buffer.alloc(65537);let size=0;
          while(size<buffer.length) {
            const {bytesRead}=await file.read(buffer,size,buffer.length-size,size);
            if(!bytesRead)break;
            size+=bytesRead;
          }
          if(size>65536)throw new Error('Native terminal receipt exceeds bounded read size');
          receiptBytes=buffer.subarray(0,size);receipt=JSON.parse(receiptBytes.toString('utf8'));
        } finally {await file.close();}
      } catch(error) {if(error.code!=='ENOENT')throw error;}
      if(receiptBytes!==null) {
        const owner=session.manifest.owned_process;
        const at=utcMilliseconds(receipt?.terminal_utc);
        if(!receipt||typeof receipt!=='object'||Array.isArray(receipt)||
           receipt.schema_version!==1||receipt.event!=='bmx_native_test_terminal'||receipt.terminal!==true||
           receipt.scenario!==scenario||receipt.owned_pid!==owner.process_id||
           receipt.owned_created_utc!==owner.process_created_utc||receipt.process_identity_verified!==true||
           typeof receipt.runtime_sha256!=='string'||receipt.runtime_sha256.toUpperCase()!==owner.runtime_sha256.toUpperCase()||
           typeof receipt.passed!=='boolean'||receipt.phase!==(receipt.passed?'native_evidence_complete':'native_evidence_failed')||
           !Number.isFinite(at)||at<utcMilliseconds(session.manifest.capture_started_utc)||at>Date.now())
          throw new Error('Native terminal receipt identity, time or phase mismatch');
        const observedMonotonic=performance.now()-session.start;
        // Preserve a validated native failure/success even if its following
        // capture fails. A null final index never establishes final coverage.
        session.manifest.native_terminal={path:terminal,sha256:createHash('sha256').update(receiptBytes).digest('hex'),
          receipt,observed_utc:utc(),observed_monotonic_ms:observedMonotonic,final_sample_index:null};
        await persist(session);
        // This request begins after receipt observation. A prior frame cannot
        // be relabeled as evidence covering terminal state.
        const final=await stepBmxWindowRecording(session,{frames:1});
        if(final.captured.length!==1)throw new Error('Capture limit reached before the terminal frame');
        if(final.captured[0].capture_started_monotonic_ms<observedMonotonic||
           utcMilliseconds(final.captured[0].capture_started_utc)<at)
          throw new Error('Final game capture predates the native terminal observation');
        session.manifest.native_terminal.final_sample_index=final.captured[0].index;
        return await endBmxWindowRecording(session,{reason:'native_terminal_observed'});
      }
      const progress=await stepBmxWindowRecording(session,{frames:1});
      if(progress.done||progress.captured.length!==1)throw new Error('Capture limit reached before native terminal receipt');
    }
  } catch(error) {
    const captureFailed=session.manifest.failure!==null;
    session.manifest.failure??=String(error.message??error);
    session.manifest.completed=false;
    if(!captureFailed)session.manifest.termination_reason='terminal_capture_failed';
    session.manifest.capture_ended_utc??=utc();session.closed=true;
    await persist(session);throw error;
  }
}
