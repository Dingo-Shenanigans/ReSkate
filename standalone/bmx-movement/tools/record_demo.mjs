// Import in node_repl; pass its initialized @oai/sky client and a returned Window.
// Captures only the exact standalone demo. Does not send input or start processes.
import fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { performance } from 'node:perf_hooks';

function dimensions(bytes, mime) {
  if (mime === 'image/png' && bytes.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10])))
    return [bytes.readUInt32BE(16),bytes.readUInt32BE(20)];
  if (mime === 'image/jpeg' && bytes[0] === 255 && bytes[1] === 216) {
    let at=2;
    while (at+4<bytes.length) {
      if(bytes[at++]!==255) throw new Error('Invalid JPEG marker');
      while(bytes[at]===255) at++;
      const marker=bytes[at++];
      if(marker===217 || marker===218) break;
      if(marker===1 || (marker>=208 && marker<=215)) continue;
      const n=bytes.readUInt16BE(at);
      if(n<2 || at+n>bytes.length) throw new Error('Invalid JPEG segment');
      if([192,193,194,195,197,198,199,201,202,203,205,206,207].includes(marker))
        return [bytes.readUInt16BE(at+5),bytes.readUInt16BE(at+3)];
      at+=n;
    }
  }
  throw new Error('Unsupported or malformed captured image');
}
const same=(a,b)=>a?.id===b?.id && a?.app===b?.app;
async function persist(s) {
  await fs.writeFile(path.join(s.directory,'capture-manifest.pending.json'),JSON.stringify(s.manifest,null,2));
  for(let attempt=0;;attempt++) {
    try {await fs.rename(path.join(s.directory,'capture-manifest.pending.json'),path.join(s.directory,'capture-manifest.json'));break;}
    catch(e) {
      if(process.platform!=='win32'||!['EPERM','EACCES','EBUSY'].includes(e.code)||attempt>=5) throw e;
      await new Promise(r=>setTimeout(r,20*2**attempt));
    }
  }
}

export async function beginDemoRecording({sky,window,packageRoot,outputDirectory,
    executable='build/bmx_demo.exe',durationMs=20000,minIntervalMs=200,maxFrames=160}) {
  if(!sky?.get_window_state || !sky?.list_windows) throw new Error('Pass initialized @oai/sky from node_repl');
  if(!Number.isSafeInteger(window?.id) || typeof window?.app!=='string') throw new Error('Pass an actual returned Window');
  if(!Number.isFinite(durationMs)||durationMs<1000||durationMs>120000||
     !Number.isFinite(minIntervalMs)||minIntervalMs<100||minIntervalMs>10000||
     !Number.isInteger(maxFrames)||maxFrames<2||maxFrames>600) throw new Error('Capture bounds invalid');
  const root=await fs.realpath(path.resolve(packageRoot));
  const exe=await fs.realpath(path.resolve(root,executable));
  if(!exe.toLowerCase().startsWith(root.toLowerCase()+path.sep) || path.basename(exe).toLowerCase()!=='bmx_demo.exe')
    throw new Error('Executable must be the standalone package bmx_demo.exe');
  if(window.app.toLowerCase()!==('process:'+exe).toLowerCase()) throw new Error('Window is not the exact standalone demo executable');
  const candidates=(await sky.list_windows()).filter(w=>same(w,window));
  if(candidates.length!==1) throw new Error('Demo window changed or closed');
  const directory=path.resolve(root,outputDirectory);
  const relative=path.relative(root,directory);
  if(!relative||relative.startsWith('..')||path.isAbsolute(relative)) throw new Error('Output must be a child of the standalone package');
  let cursor=directory;
  while(cursor!==root) {
    try {if((await fs.lstat(cursor)).isSymbolicLink()) throw new Error('Capture path contains symlink/junction');}
    catch(e) {if(e.code!=='ENOENT') throw e;}
    cursor=path.dirname(cursor);
  }
  await fs.mkdir(path.dirname(directory),{recursive:true});
  await fs.mkdir(directory); // Existing recordings are never overwritten.
  const s={sky,window:candidates[0],directory,start:performance.now(),last:null,busy:false,closed:false,
    durationMs,minIntervalMs,maxFrames,bytes:0,manifest:{schema:1,event:'bmx_standalone_sampled_live_capture',
    sampled_live:true,completed:false,failure:null,capture_started_utc:new Date().toISOString(),capture_ended_utc:null,
    source_window:candidates[0],executable:exe,executable_sha256:createHash('sha256').update(await fs.readFile(exe)).digest('hex'),
    limits:{duration_ms:durationMs,min_interval_ms:minIntervalMs,max_frames:maxFrames},
    timing_basis:'Source frames captured from the actual demo window via @oai/sky. Midpoint of request/return interval; not simulation clock or native 60fps capture.',
    os_input_injected:false,gameplay_parity_proven:false,samples:[]}};
  await persist(s); return s;
}

export async function stepDemoRecording(s,{frames=12,chunkDurationMs=15000}={}) {
  if(s.closed||s.busy) throw new Error('Recording closed or busy');
  if(!Number.isInteger(frames)||frames<1||frames>100||!Number.isFinite(chunkDurationMs)||chunkDurationMs<100||chunkDurationMs>30000)
    throw new Error('Chunk bounds invalid');
  const begin=performance.now(); s.busy=true;
  try {
    while(s.manifest.samples.length<s.maxFrames && performance.now()-s.start<s.durationMs &&
          performance.now()-begin<chunkDurationMs && frames-->0) {
      if(s.last!==null) {const delay=s.minIntervalMs-(performance.now()-s.last); if(delay>0) await new Promise(r=>setTimeout(r,delay));}
      const a=performance.now(); s.last=a;
      const state=await s.sky.get_window_state({window:s.window,include_screenshot:true,include_text:false});
      const b=performance.now();
      if(!same(state?.window,s.window)||state.screenshots?.length!==1) throw new Error('Capture changed window or returned ambiguous screenshots');
      const shot=state.screenshots[0],match=/^data:(image\/(?:png|jpeg));base64,(.+)$/s.exec(shot.url);
      if(!match) throw new Error('Expected PNG or JPEG screenshot');
      const bytes=Buffer.from(match[2],'base64'),[width,height]=dimensions(bytes,match[1]);
      if(width<16||height<16||width>8192||height>8192||bytes.length>33554432||s.bytes+bytes.length>536870912)
        throw new Error('Image/storage bounds exceeded');
      const first=s.manifest.samples[0];
      if(first && (first.width!==width || first.height!==height)) throw new Error('Demo dimensions changed during recording');
      const index=s.manifest.samples.length,file=`frame-${String(index).padStart(6,'0')}.${match[1]==='image/png'?'png':'jpg'}`;
      await fs.writeFile(path.join(s.directory,file),bytes,{flag:'wx'});
      s.bytes+=bytes.length; s.window=state.window;
      s.manifest.samples.push({index,file,width,height,mime:match[1],bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex'),
        window_id:state.window.id,app:state.window.app,capture_started_monotonic_ms:a-s.start,
        capture_ended_monotonic_ms:b-s.start,presentation_monotonic_ms:(a+b)/2-s.start});
    }
    await persist(s);
    return {frames:s.manifest.samples.length,elapsed_ms:performance.now()-s.start,closed:s.closed,directory:s.directory};
  } catch(e) {s.manifest.failure=String(e); throw e;}
  finally {s.busy=false;}
}

export async function endDemoRecording(s) {
  if(s.busy) throw new Error('Wait for current capture chunk');
  s.closed=true; s.manifest.capture_ended_utc=new Date().toISOString();
  const samples=s.manifest.samples,gaps=samples.slice(1).map((x,i)=>x.presentation_monotonic_ms-samples[i].presentation_monotonic_ms);
  s.manifest.completed=!s.manifest.failure && samples.length>=2;
  s.manifest.audit={sample_count:samples.length,source_bytes:s.bytes,
    sample_rate_hz:samples.length>1?(samples.length-1)*1000/(samples.at(-1).presentation_monotonic_ms-samples[0].presentation_monotonic_ms):null,
    maximum_sample_gap_ms:gaps.length?Math.max(...gaps):null};
  await persist(s); return {directory:s.directory,...s.manifest.audit,completed:s.manifest.completed,failure:s.manifest.failure};
}
