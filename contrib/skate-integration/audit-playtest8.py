"""Extract compact, anonymized regression evidence from the four supplied test archives.
Usage: python audit-playtest8.py sammy.zip john.zip jish.zip jish2.zip output.json
No game assets or complete logs are copied into the fixture.
"""
import collections,datetime,difflib,json,re,statistics,sys,zipfile
from pathlib import Path

clients={};attempts=[]
for label,path in zip(["A","B","C","D"],sys.argv[1:5]):
    with zipfile.ZipFile(path) as z:
        lines=z.read("logs/ReSkate.log").decode(errors="replace").splitlines()
        pid=next(re.search(r"\[(\d+):\d+\]",s)[1] for s in lines if "detector outcome:" in s and re.search(r"\[(\d+):\d+\]",s))
        own=[];native=[]
        for line in lines:
            m=re.search(r"detector outcome: turn (\d+), landed (true|false), result '(.*)'",line)
            if m:own.append((datetime.datetime.fromisoformat(line[1:24]),int(m[1]),m[2]=="true",m[3]))
            m=re.search(r"native player (0x[0-9a-f]+), (setter|follower), (accepted|failed); target: (.*)",line)
            if m:native.append((datetime.datetime.fromisoformat(line[1:24]),m[2]+"|"+m[3]+"|"+m[4],m[2],m[3],m[4]))
        outcomes=[];sets=[];samples=[]
        for name in z.namelist():
            if not name.endswith(".jsonl") or "-"+pid+"-" not in name:continue
            with z.open(name) as f:
                for raw in f:
                    try:j=json.loads(raw)
                    except json.JSONDecodeError:continue # A client stopped mid-write at the end of one recording.
                    kind=j["kind"]
                    if kind=="detected_attempt" and j["result"]["rule_version"]==7:outcomes.append((j["uptime_ms"],j["result"]))
                    if kind=="trick_set":sets.append(j)
                    if kind=="sample" and j.get("ready"):
                        samples.append({"now":j["uptime_ms"],"steps":j.get("native_physics_steps",0),"sequence_ends":j.get("native_hud_sequence_ends",0),"wipeouts":j.get("native_step_wipeouts",0),"state":j.get("selected_physics_state",0),"yaw":j.get("body_forward_yaw_deg",0),"valid":j.get("riding_body_candidate_known",False),"goofy":j.get("natural_goofy",False)})
        aligned=[];used=set()
        for time,turn,landed,display in own:
            found=next(((i,uptime,r) for i,(uptime,r) in enumerate(outcomes) if i not in used and r["turn"]==turn and r["landed"]==landed and r["display"]==display),None)
            if not found:continue
            i,uptime,result=found;used.add(i)
            rawset=next((s for s in sets if s["launch_ms"]<=uptime<=s["end_ms"]),None) if not landed else next((s for s in reversed(sets) if 0<=uptime-s["end_ms"]<=1000 and s.get("landed")==True),None)
            replay=None
            if rawset:
                selected=[s for s in samples if rawset["launch_ms"]-32<=s["now"]<=rawset["end_ms"]]
                following=next((s for s in samples if s["now"]>rawset["end_ms"]),None)
                if following:selected.append(following)
                row_keys=["name","name_resolved","observation_id","first_seen_ms","preexisting","raw_labels","identity_labels","labels","candidate_segment_id"]
                replay={"samples":selected,"rows":[{k:r[k] for k in row_keys if k in r} for r in rawset["ticker_candidates"]]}
            aligned.append((time,{"client":label,"result":result,"replay":replay}))
        clients[label]={"native":native,"own":aligned}
ref=clients["A"]["native"];offsets={"A":0,"D":0}
for label in ["B","C"]:
    other=clients[label]["native"];deltas=[]
    for block in difflib.SequenceMatcher(None,[e[1] for e in ref],[e[1] for e in other],autojunk=False).get_matching_blocks():
        if block.size>=2:deltas.extend((ref[block.a+i][0]-other[block.b+i][0]).total_seconds() for i in range(block.size))
    offsets[label]=statistics.median(deltas)
for label,client in clients.items():
    for time,evidence in client["own"]:attempts.append((time+datetime.timedelta(seconds=offsets[label]),evidence))
cases=[]
for label in ["A","D"]:
    target=None
    for time,_,role,outcome,display in clients[label]["native"]:
        time+=datetime.timedelta(seconds=offsets[label])
        near=sorted([(abs((t-time).total_seconds()),a) for t,a in attempts if abs((t-time).total_seconds())<0.6],key=lambda x:x[0])
        if role=="setter" and outcome=="accepted":target=next((a for _,a in near if a["result"]["landed"] and a["result"]["display"]==display),None)
        if role!="follower" or outcome!="failed":continue
        defense=next((a for _,a in near if a is not target),None)
        cases.append({"id":len(cases)+1,"time":time.isoformat(),"target_display":display,"target":target,"defense":defense})
fixture={"schema":1,"source_rules":7,"clock_corrections_seconds":offsets,"local_outcome_counts":{k:len(v["own"]) for k,v in clients.items()},"cases":cases}
Path(sys.argv[5]).write_text(json.dumps(fixture,separators=(",",":"))+"\n",encoding="utf-8")
print("Extracted",len(cases),"unique failed defenses; paired landed:",sum(bool(c["target"] and c["defense"] and c["defense"]["result"]["landed"]) for c in cases))
print("Own outcomes:",fixture["local_outcome_counts"],"clock corrections:",offsets)
