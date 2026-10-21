using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.Json;
using MelonLoader;
using UnityEngine;
using VC = Il2CppMashBox.BMX_Physics_Development.VehicleController;
using IK = Il2CppMashBox.BMX_Physics_Development.IKTargetManager;
using PI = Il2CppMashBox.BMX_Physics_Development.NewShit.PlayerInputBehaviour;
using TB = Il2CppMashBox.BMX_Physics_Development.Animancer_Test.Trick_System.TrickSystemBrain;
using TV2 = Il2CppMashBox.BMX_Physics_Development.Animancer_Test.Trick_System.v2.TrickSystemBrainV2;
using CM = Il2CppMashBox.Character.CharacterManager;
using Object = UnityEngine.Object;

[assembly: MelonInfo(typeof(BmxMovementObserver.Observer), "BMX Movement Observer", "0.3.3", "Local analysis")]
[assembly: MelonGame(null, null)]

namespace BmxMovementObserver {
public sealed partial class Observer : MelonMod {
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    readonly Stopwatch clock = new Stopwatch();
    StreamWriter telemetry, frames, rider;
    string output, gate, controlFolder, lastRequestHash;
    bool enabled, started, done, capturePending;
    int ticks, frameCount, errors;
    double duration = 14, interval = .1, lastCapture = -1, nextDiscovery, lastDiagnostic = -1;
    VC vehicle;
    Rigidbody body;
    IK ik;
    Animator animator;
    PI localInput;
    TB trickBrain;
    TV2 trickBrainV2;
    DateTime startedUtc;
    int captureSequence;
    readonly Dictionary<string, PropertyInfo> propertyCache = new Dictionary<string, PropertyInfo>();

    public override void OnInitializeMelon() {
        output = Environment.GetEnvironmentVariable("BMX_OBSERVER_OUTPUT");
        gate = Environment.GetEnvironmentVariable("BMX_OBSERVER_START_GATE");
        if (String.IsNullOrWhiteSpace(output)) { LoggerInstance.Msg("Inactive: BMX_OBSERVER_OUTPUT was not supplied."); return; }
        output = Path.GetFullPath(output);
        string expected = Path.GetFullPath(@"C:\Users\Jeffr\Desktop\reskate\standalone\bmx-movement\recordings");
        if (!output.StartsWith(expected + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) {
            LoggerInstance.Error("Refused output outside the designated comparison recording folder."); return;
        }
        if (Directory.Exists(output)) { LoggerInstance.Error("Refused existing capture folder: " + output); return; }
        if (String.IsNullOrWhiteSpace(gate)) gate = Path.Combine(output, "start-gate.json");
        gate = Path.GetFullPath(gate);
        if (!gate.StartsWith(output + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) {
            LoggerInstance.Error("Refused start marker outside the capture folder."); return;
        }
        double number;
        if (Double.TryParse(Environment.GetEnvironmentVariable("BMX_OBSERVER_DURATION"), NumberStyles.Float, Inv, out number)) duration = Math.Clamp(number, 2, 300);
        if (Double.TryParse(Environment.GetEnvironmentVariable("BMX_OBSERVER_FPS"), NumberStyles.Float, Inv, out number)) interval = 1 / Math.Clamp(number, 1, 10);
        if (Double.TryParse(Environment.GetEnvironmentVariable("BMX_OBSERVER_BONES_FPS"), NumberStyles.Float, Inv, out number)) boneInterval = 1 / Math.Clamp(number, 1, 30);
        if (Double.TryParse(Environment.GetEnvironmentVariable("BMX_OBSERVER_PHYSICS_HZ"), NumberStyles.Float, Inv, out number)) physicsInterval = 1 / Math.Clamp(number, 1, 100);
        Directory.CreateDirectory(output);
        controlFolder = output;
        enabled = true;
        WriteJson("observer-initialized.json", new { schema = 1, observer = "0.3.3", initialized_utc = DateTime.UtcNow.ToString("o"),
            output, gate, duration_seconds = duration, capture_interval_seconds = interval,
            observation_only = true, changes_physics = false, callback_phase = "MelonMod.OnFixedUpdate", screenshot_phase = "WaitForEndOfFrame" });
        LoggerInstance.Msg("Observation-only recorder initialized: " + output);
    }

    object Read(object instance, string name) {
        if (instance == null) return null;
        try {
            Type type = instance.GetType(); string key = type.FullName + ":" + name;
            PropertyInfo property;
            if (!propertyCache.TryGetValue(key, out property)) {
                property = type.GetProperty(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static);
                propertyCache[key] = property;
            }
            if(property != null)return property.GetValue(instance);
            var field=type.GetField(name,BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.Static);
            return field==null?null:field.GetValue(instance);
        } catch { return null; }
    }
    double Number(object value) {
        if (value == null) return Double.NaN;
        try { return Convert.ToDouble(value, Inv); } catch { return Double.NaN; }
    }
    double N(object value, string name) { return Number(Read(value, name)); }
    string F(double n) { return n.ToString("R", Inv); }
    static string Esc(string value) { return "\"" + (value ?? "").Replace("\"", "\"\"") + "\""; }
    void WriteJson(string name, object value) {
        File.WriteAllText(Path.Combine(output, name), JsonSerializer.Serialize(value, new JsonSerializerOptions { WriteIndented = true, NumberHandling = System.Text.Json.Serialization.JsonNumberHandling.AllowNamedFloatingPointLiterals }));
    }
    void Discover() {
        try {
            vehicle = VC.LocalDriverVehicle;
            if (vehicle == null) { body = null; bindingLostSinceDiscovery = true; return; }
            if(bindingLostSinceDiscovery || (selectedVehicleId!=0&&selectedVehicleId!=vehicle.GetInstanceID())) { ClearBindingCaches(); bindingLostSinceDiscovery=false; }
            body = vehicle.ChassisBody;
            if (body == null || !body.gameObject.activeInHierarchy) return;
            var concrete = vehicle._characterManager == null ? null : vehicle._characterManager.TryCast<CM>();
            if (concrete != null) {
                if (concrete.InputBehaviour != null) {var input=concrete.InputBehaviour.TryCast<PI>();if(input!=null)localInput=input;}
                if (concrete._trickSystemBrain != null) trickBrainV2 = concrete._trickSystemBrain;
                var a = Read(concrete._humanAnimator,"Animator") as Animator;
                if (a != null) animator = a;
                if (ik == null && concrete.Root != null) {
                    var all = concrete.Root.GetComponentsInChildren<IK>();
                    for(int i=0;i<all.Length;++i)if(all[i]!=null){ik=all[i];break;}
                }
            }
            if (localInput == null) {
                var all = Resources.FindObjectsOfTypeAll<PI>(); PI nearest = null; float distance = 6;
                for(int i=0;i<all.Length;++i) {
                    var candidate=all[i];if(candidate==null||!candidate.gameObject.activeInHierarchy)continue;
                    if(candidate._hasInputAuthority || candidate._simulateAsLocalPlayer){localInput=candidate;break;}
                    float d=Vector3.Distance(candidate.transform.position,body.position);
                    if(d<distance){distance=d;nearest=candidate;}
                }
                if(localInput==null)localInput=nearest;
            }
            if(trickBrain==null) {
                var all=Resources.FindObjectsOfTypeAll<TB>();
                for(int i=0;i<all.Length;++i)if(all[i]!=null&&all[i].gameObject.activeInHierarchy&&Vector3.Distance(all[i].transform.position,body.position)<4){trickBrain=all[i];break;}
            }
            if(trickBrainV2==null) {
                var all=Resources.FindObjectsOfTypeAll<TV2>();
                for(int i=0;i<all.Length;++i)if(all[i]!=null&&all[i].gameObject.activeInHierarchy&&Vector3.Distance(all[i].transform.position,body.position)<4){trickBrainV2=all[i];break;}
            }
            if (ik == null) {
                var all = Resources.FindObjectsOfTypeAll<IK>();
                for (int i = 0; i < all.Length; ++i) {
                    var candidate = all[i];
                    if (candidate != null && candidate.gameObject.activeInHierarchy &&
                        Vector3.Distance(candidate.transform.position, body.position) < 4) { ik = candidate; break; }
                }
            }
            if (animator == null && vehicle._characterManager != null) {
                var root = vehicle._characterManager.Root;
                if (root != null) {
                    var all = root.GetComponentsInChildren<Animator>();
                    for (int i=0; i<all.Length; ++i) if (all[i] != null && all[i].isHuman) { animator = all[i]; break; }
                }
            }
            if(animator==null) {
                var all=Resources.FindObjectsOfTypeAll<Animator>();
                for(int i=0;i<all.Length;++i)if(all[i]!=null&&all[i].gameObject.activeInHierarchy&&all[i].isHuman&&Vector3.Distance(all[i].transform.position,body.position)<4){animator=all[i];break;}
            }
            DiscoverV3();
            if (!started) {
                WriteJson("observer-ready.json", new { ready = true, utc = DateTime.UtcNow.ToString("o"), vehicle = vehicle.name,
                    body = body.name, body_id = body.GetInstanceID(), vehicle_type = Convert.ToString(Read(vehicle, "_vehicleTypeName"), Inv),
                    fixed_delta_time = Time.fixedDeltaTime, gravity = Vec(Physics.gravity), gate,
                    input = localInput==null?null:localInput.name, trick_brain=trickBrain==null?null:trickBrain.name, trick_brain_v2=trickBrainV2==null?null:trickBrainV2.name,
                    animator=animator==null?null:animator.name });
            }
        } catch (Exception ex) { LoggerInstance.Warning("Discovery: " + ex.Message); }
    }
    static double[] Vec(Vector3 v) { return new double[] { v.x, v.y, v.z }; }
    static double[] Q(Quaternion q) { return new double[] { q.x, q.y, q.z, q.w }; }
    object Primitive(object value) {
        if (value == null) return null;
        if (value is Vector3) return Vec((Vector3)value);
        if (value is Quaternion) return Q((Quaternion)value);
        if (value is Vector2) {var v=(Vector2)value;return new double[]{v.x,v.y};}
        if (value is AnimationCurve) {
            var curve=(AnimationCurve)value;var keys=curve.keys;var list=new List<object>();
            for(int i=0;i<keys.Length;++i) {var key=keys[i];list.Add(new{time=key.time,value=key.value,in_tangent=key.inTangent,out_tangent=key.outTangent,
                in_weight=key.inWeight,out_weight=key.outWeight,weighted_mode=key.weightedMode.ToString()});}
            return new{keys=list,pre_wrap_mode=curve.preWrapMode.ToString(),post_wrap_mode=curve.postWrapMode.ToString()};
        }
        Type t = value.GetType();
        if (t.IsPrimitive || t.IsEnum || value is string || value is decimal) return value.ToString();
        if(t.FullName.Contains("Array"))return Deep(value,2);
        if (value is Object) { var obj = (Object)value; return new { name = obj.name, type = t.FullName, id = obj.GetInstanceID() }; }
        return new { type = t.FullName };
    }
    object Properties(object value) {
        Dictionary<string, object> result = new Dictionary<string, object>();
        if (value == null) return result;
        foreach (var p in value.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance)) {
            if (p.GetIndexParameters().Length != 0 || !p.CanRead) continue;
            try { result[p.Name] = Primitive(p.GetValue(value)); } catch { }
        }
        return result;
    }
    void Inventory() {
        var bodies = Resources.FindObjectsOfTypeAll<Rigidbody>();
        var list = new List<object>();
        for (int i=0; i<bodies.Length; ++i) {
            var b = bodies[i]; if (b == null || !b.gameObject.activeInHierarchy) continue;
            bool belongs = b.transform.IsChildOf(vehicle.transform);
            if (!belongs && vehicle._characterManager != null && vehicle._characterManager.Root != null)
                belongs = b.transform.IsChildOf(vehicle._characterManager.Root);
            if (!belongs) continue;
            var joints=b.GetComponents<Joint>();var jointList=new List<object>();
            for(int j=0;j<joints.Length;++j) {var joint=joints[j];if(joint==null)continue;jointList.Add(new{type=joint.GetIl2CppType().FullName,
                connected_body=joint.connectedBody==null?0:joint.connectedBody.GetInstanceID(),anchor=Vec(joint.anchor),connected_anchor=Vec(joint.connectedAnchor),
                enable_collision=joint.enableCollision,enable_preprocessing=joint.enablePreprocessing,mass_scale=joint.massScale,connected_mass_scale=joint.connectedMassScale});}
            list.Add(new { name = b.name, id = b.GetInstanceID(), mass = b.mass, inertia = Vec(b.inertiaTensor),
                inertia_rotation = Q(b.inertiaTensorRotation), center_of_mass = Vec(b.centerOfMass), world_center_of_mass = Vec(b.worldCenterOfMass),
                position = Vec(b.position), orientation = Q(b.rotation), is_kinematic = b.isKinematic, drag = b.drag, angular_drag = b.angularDrag,joints=jointList });
        }
        var settings = vehicle.MotorVehicleSettings;
        var nested = new Dictionary<string, object>();
        if (settings != null) foreach (var p in settings.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance)) {
            if (!p.CanRead || p.GetIndexParameters().Length != 0) continue;
            try {
                object value = p.GetValue(settings);
                if (value != null && (p.Name.IndexOf("Settings", StringComparison.OrdinalIgnoreCase)>=0 || p.Name.IndexOf("Data", StringComparison.OrdinalIgnoreCase)>=0)) nested[p.Name] = Properties(value);
            } catch { }
        }
        var userValues=new Dictionary<string,object>();
        foreach(var name in new[]{"_userGravitySmartData","_userGravitySmartDataNew","_userQuickHopHeightSmartData","_userSideHopSmartData","_manualOrPegHopHeightMultiplier","_jumpForceMult"}) {
            var value=Read(vehicle,name);userValues[name]=new{reference=Primitive(value),value=Primitive(Read(value,"Value")),definition_default=Primitive(Read(Read(value,"_definition"),"DefaultValue"))};
        }
        foreach(var name in new[]{"_userFlipAxisTypeSmartData","_userFlipMultSmartData","_userSpinMultTypeSmartData","_torqueMult","_quickSpinExtra"}) {
            var value=Read(vehicle._spinSystem,name);userValues["spin."+name]=new{reference=Primitive(value),value=Primitive(Read(value,"Value"))};
        }
        WriteJson("runtime-inventory.json", new { utc = DateTime.UtcNow.ToString("o"), fixed_delta_time = Time.fixedDeltaTime,
            maximum_delta_time = Time.maximumDeltaTime, time_scale = Time.timeScale, gravity = Vec(Physics.gravity),
            physics_auto_simulation = Physics.autoSimulation, vehicle_name = vehicle.name, vehicle_id = vehicle.GetInstanceID(),
            chassis_body_id = body.GetInstanceID(), bodies = list, settings = Properties(settings), nested_settings = nested,
            vehicle_scalars = Properties(vehicle), spin_settings = Properties(Read(vehicle._spinSystem, "_airControlSettings")),
            user_values=userValues,
            spin_scalars = Properties(vehicle._spinSystem), limitations = new[] { "Observed runtime getters, not proof of publisher/default settings.",
                "OnFixedUpdate callback order relative to vehicle scripts is not established.", "Screenshot export can affect frame pacing; timestamps are retained." } });
    }
    void StartCapture() {
        if(started||done||body==null||startRejected||Time.realtimeSinceStartup<nextStartAttemptRealtime)return;
        string markerContents;
        if(!TryReadStartMarker(out markerContents))return;
        var channelNames=new[]{"telemetry.csv","frames.csv","rider.jsonl","body-physics.jsonl","trick-states.jsonl","channel-errors.jsonl"};
        var opened=new List<StreamWriter>();var createdPaths=new List<string>();
        try {
            foreach(string name in channelNames)if(File.Exists(Path.Combine(output,name)))throw new IOException("Refused existing channel file: "+name);
            foreach(string name in channelNames) {
                string path=Path.Combine(output,name);var stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.Read);
                createdPaths.Add(path);try{opened.Add(new StreamWriter(stream,new UTF8Encoding(false)));}catch{stream.Dispose();throw;}
            }
            telemetry=opened[0];frames=opened[1];rider=opened[2];bodyPhysics=opened[3];trickStates=opened[4];channelErrors=opened[5];
            telemetry.WriteLine("time,utc,simulation_seconds,fixed_dt,gravity_y,time_scale,valid,grounded,x,y,z,vx,vy,vz,wx,wy,wz,qx,qy,qz,qw,body_id,body_name,vehicle_name,pedal,brake,steer,flip,manny,nosey,preload,spin,trick_id,front_load,rear_load,front_grounded,rear_grounded,vehicle_com_x,vehicle_com_y,vehicle_com_z,mass,trick_v2_set,trick_v2_slot,trick_v2_time_since_fire,fixed_index,binding_reason,current_vehicle_id");
            frames.WriteLine("index,file,elapsed_seconds,simulation_seconds,width,height");
            telemetry.Flush();frames.Flush();
        } catch(Exception ex) {
            foreach(var writer in opened)try{writer.Dispose();}catch{}
            foreach(string path in createdPaths)try{File.Delete(path);}catch{}
            telemetry=frames=rider=bodyPhysics=trickStates=channelErrors=null;
            RejectStart("channel_open_transaction_failed",ex);return;
        }
        startedUtc=DateTime.UtcNow;clock.Restart();nextDiscovery=0;
        boneCount=physicsCount=validTicks=invalidTicks=0;lastBones=lastPhysics=-1;channelErrorCounts.Clear();
        started=true;
        try {WriteJson("capture-start.json",new{schema=2,capture_started_utc=startedUtc.ToString("o"),gate,marker_contents=markerContents,
            marker_read_attempts=startReadAttempts,all_channels_opened=true,observer_only=true,process_id=Environment.ProcessId,
            body_id=body.GetInstanceID(),screenshot_backend="Unity ScreenCapture.CaptureScreenshotAsTexture at WaitForEndOfFrame"});}
            catch(Exception ex){ChannelError("capture_start_metadata",ex);}
        try {StartV3();}catch(Exception ex){ChannelError("capture_schema_inventory",ex);}
        try {Inventory();}catch(Exception ex){ChannelError("runtime_inventory",ex);}
        LoggerInstance.Msg("Capture started; duration "+duration.ToString(Inv));
    }
    public override void OnUpdate() {
        if (!enabled) return;
        if (done) { TryNextCapture(); return; }
        double t = started ? clock.Elapsed.TotalSeconds : Time.realtimeSinceStartup;
        if (t >= nextDiscovery) { nextDiscovery = t + 1; Discover(); }
        if (!started && !startRejected && body != null && File.Exists(gate)) StartCapture();
        if (started && clock.Elapsed.TotalSeconds >= duration) Finish(true, null);
    }
    void TryNextCapture() {
        string request=Path.Combine(controlFolder,"next-capture.json");
        if(!File.Exists(request))return;
        try {
            byte[] bytes=File.ReadAllBytes(request);string hash=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes));
            if(hash==lastRequestHash)return;lastRequestHash=hash;
            using(var document=JsonDocument.Parse(bytes)) {
                var node=document.RootElement;string name=node.GetProperty("name").GetString();
                if(String.IsNullOrWhiteSpace(name)||name!=Path.GetFileName(name)||name=="."||name==".."||name.IndexOfAny(Path.GetInvalidFileNameChars())>=0)throw new InvalidDataException("Next capture name must be one plain child-directory name");
                string next=Path.GetFullPath(Path.Combine(Path.GetDirectoryName(controlFolder),name));
                if(Directory.Exists(next)||File.Exists(next))throw new IOException("Next capture destination already exists");
                double seconds=duration;JsonElement value;
                if(node.TryGetProperty("duration_seconds",out value))seconds=Math.Clamp(value.GetDouble(),2,300);
                string nextGate=Path.Combine(next,"start-gate.json");
                Directory.CreateDirectory(next);
                output=next;gate=nextGate;duration=seconds;started=false;done=false;ticks=frameCount=errors=0;lastCapture=lastDiagnostic=-1;nextDiscovery=0;catalogIdentity=null;
                clock.Reset();startRejected=false;startReadAttempts=0;firstStartAttemptRealtime=-1;nextStartAttemptRealtime=0;++captureSequence;
                WriteJson("observer-initialized.json",new{schema=1,observer="0.3.3",initialized_utc=DateTime.UtcNow.ToString("o"),output,gate,duration_seconds=duration,
                    capture_interval_seconds=interval,observation_only=true,changes_physics=false,control_folder=controlFolder,request_sha256=hash});
                File.WriteAllText(Path.Combine(controlFolder,"next-capture-receipt-"+captureSequence.ToString("D3",Inv)+".json"),
                    JsonSerializer.Serialize(new{accepted_utc=DateTime.UtcNow.ToString("o"),output,gate,request_sha256=hash},new JsonSerializerOptions{WriteIndented=true}));
                LoggerInstance.Msg("Armed fresh capture folder: "+output);
            }
        } catch(Exception ex) {LoggerInstance.Warning("Next capture refused: "+ex.Message);}
    }
    double WheelLoad(object wheels) {
        if (wheels == null) return Double.NaN;
        try {
            int length = Convert.ToInt32(Read(wheels, "Length"), Inv);
            var index = wheels.GetType().GetProperty("Item"); double sum = 0;
            for (int i=0;i<length;++i) { var w = index.GetValue(wheels, new object[] { i }); sum += N(w, "_wheelLoad"); }
            return sum;
        } catch { return Double.NaN; }
    }
    public override void OnFixedUpdate() {
        if (!started || done) return;
        BeginFixedSample();
        bool written=false;
        if(bindingAvailable) try { WriteTelemetryV31(); written=true; ++validTicks; }
            catch(Exception ex) { ChannelError("telemetry",ex); }
        if(!written) { ++invalidTicks; try { WriteInvalidTelemetry(); } catch(Exception ex) { ChannelError("telemetry_write",ex); } }
        try { FixedV3(); } catch(Exception ex) { ChannelError("fixed_dispatch",ex); }
        if(ticks%100==0) { try { telemetry.Flush(); } catch(Exception ex) { ChannelError("telemetry_flush",ex); } }
    }
    void WriteTelemetryV31() {
            Vector3 p = body.worldCenterOfMass, v = body.velocity, w = body.angularVelocity; Quaternion q = body.rotation;
            Vector3 combined = vehicle.WorldCenterOfMass;
            var character = vehicle._characterManager; object input = localInput;
            object brain = trickBrain;
            double trickId=N(brain,"CurrentTrickID"),setId=N(trickBrainV2,"setId"),slotId=N(trickBrainV2,"slotId");
            if(Double.IsNaN(trickId)&&setId>=0&&setId<=7&&slotId>=0&&slotId<8)trickId=setId*10+slotId;
            object collision = vehicle._bmxCollisionHandler;
            var row = new List<string> { F(fixedElapsed), Esc(fixedUtc), F(fixedSimulation), F(Time.fixedDeltaTime), F(Physics.gravity.y), F(Time.timeScale), "1", F(N(collision,"Grounded")),
                F(p.x),F(p.y),F(p.z),F(v.x),F(v.y),F(v.z),F(w.x),F(w.y),F(w.z),F(q.x),F(q.y),F(q.z),F(q.w),body.GetInstanceID().ToString(Inv),Esc(body.name),Esc(vehicle.name),
                F(N(input,"AccelerationInput")),F(N(input,"BrakeInput")),F(vehicle.SteerInput),F(vehicle.FlipInput),F(N(input,"MannyInput")),F(N(input,"NoseyInput")),F(N(input,"PreloadInput")),F(vehicle.SpinInput),
                F(trickId),F(WheelLoad(Read(vehicle,"_frontWheels"))),F(WheelLoad(Read(vehicle,"_rearWheels"))),F(N(collision,"_frontEndGrounded")),F(N(collision,"_backEndGrounded")),F(combined.x),F(combined.y),F(combined.z),F(body.mass),F(setId),F(slotId),F(N(trickBrainV2,"TimeSinceLastFire")),(ticks-1).ToString(Inv),Esc(bindingReason),currentVehicleId.ToString(Inv) };
            telemetry.WriteLine(String.Join(",",row));
    }
    object Target(Transform t) { return t == null ? null : new { id=t.GetInstanceID(),name=t.name,parent_id=t.parent==null?0:t.parent.GetInstanceID(),
        position = Vec(t.position), rotation = Q(t.rotation),local_position=Vec(t.localPosition),local_rotation=Q(t.localRotation) }; }
    object Bone(HumanBodyBones bone) { try { return animator == null ? null : Target(animator.GetBoneTransform(bone)); } catch { return null; } }
    object AllBones() {
        var result=new Dictionary<string,object>();
        for(int i=0;i<(int)HumanBodyBones.LastBone;++i){var bone=(HumanBodyBones)i;result[bone.ToString()]=Bone(bone);}
        return result;
    }
    public override void OnLateUpdate() {
        if (!enabled) return;
        if(started&&!done){try{BonesV3();}catch(Exception ex){ChannelError("bones",ex);}}
        if(capturePending)return;
        if (!started || done) {
            if (Time.realtimeSinceStartup - lastDiagnostic < 1) return;
            lastDiagnostic = Time.realtimeSinceStartup; capturePending = true; MelonCoroutines.Start(CaptureDiagnostic()); return;
        }
        if (clock.Elapsed.TotalSeconds - lastCapture < interval || frameCount >= Math.Ceiling(duration/interval)+2) return;
        lastCapture = clock.Elapsed.TotalSeconds; capturePending = true;
        MelonCoroutines.Start(CaptureFrame());
    }
    IEnumerator CaptureDiagnostic() {
        yield return new WaitForEndOfFrame();
        Texture2D texture = null;
        try {
            texture = ScreenCapture.CaptureScreenshotAsTexture(1);
            var encoded = ImageConversion.EncodeToJPG(texture, 86);
            byte[] bytes = new byte[encoded.Length]; for(int i=0;i<bytes.Length;++i)bytes[i]=encoded[i];
            File.WriteAllBytes(Path.Combine(output,"diagnostic.jpg"),bytes);
            WriteJson("diagnostic.json",new {utc=DateTime.UtcNow.ToString("o"),simulation_seconds=Time.time,width=texture.width,height=texture.height,
                vehicle=vehicle==null?null:vehicle.name,body=body==null?null:body.name,ready=body!=null,started,done,
                body_position=body==null?null:Vec(body.worldCenterOfMass),body_rotation=body==null?null:Q(body.rotation),body_velocity=body==null?null:Vec(body.velocity),
                body_angular_velocity=body==null?null:Vec(body.angularVelocity),vehicle_com=vehicle==null?null:Vec(vehicle.WorldCenterOfMass),
                fixed_delta_time=Time.fixedDeltaTime,gravity=Vec(Physics.gravity),source="actual Unity framebuffer; no desktop capture"});
        } catch(Exception ex) { if(++errors<6)LoggerInstance.Warning("Diagnostic frame: "+ex.Message); }
        finally {if(texture!=null)Object.Destroy(texture);capturePending=false;}
    }
    IEnumerator CaptureFrame() {
        yield return new WaitForEndOfFrame();
        Texture2D texture = null;
        try {
            if (done) yield break;
            double elapsed = clock.Elapsed.TotalSeconds;
            texture = ScreenCapture.CaptureScreenshotAsTexture(1);
            var encoded = ImageConversion.EncodeToJPG(texture, 88);
            byte[] bytes = new byte[encoded.Length]; for (int i=0;i<bytes.Length;++i) bytes[i] = encoded[i];
            string filename = "frame-" + frameCount.ToString("D5",Inv) + ".jpg";
            File.WriteAllBytes(Path.Combine(output,filename),bytes);
            frames.WriteLine(frameCount.ToString(Inv)+","+filename+","+F(elapsed)+","+F(Time.time)+","+texture.width.ToString(Inv)+","+texture.height.ToString(Inv));
            frames.Flush();
            ++frameCount;
        } catch (Exception ex) { if (++errors<6) LoggerInstance.Warning("Frame: " + ex.Message); }
        finally { if (texture != null) Object.Destroy(texture); capturePending = false; }
    }
    void Finish(bool complete, string failure) {
        if (done || !started) return; done = true;
        telemetry?.Dispose(); frames?.Dispose(); rider?.Dispose();bodyPhysics?.Dispose();trickStates?.Dispose();channelErrors?.Dispose();
        WriteJson("capture-summary.json", new { schema = 1, completed = complete, failure, frames = frameCount, telemetry_ticks = ticks, errors,bone_samples=boneCount,physics_samples=physicsCount,valid_fixed_ticks=validTicks,invalid_fixed_ticks=invalidTicks,channel_errors=channelErrorCounts,
            rendered_live = true, window_capture = false, scripted = false, source = "BMX Streets actual Unity framebuffer",
            started_utc = startedUtc.ToString("o"), start_utc_seconds = new DateTimeOffset(startedUtc).ToUnixTimeMilliseconds()/1000.0,
            ended_utc=DateTime.UtcNow.ToString("o"), elapsed_seconds=clock.Elapsed.TotalSeconds,
            input_source = "external controller; correlate with driver receipt", observation_only = true });
        LoggerInstance.Msg("Capture closed: " + frameCount.ToString(Inv) + " frames, " + ticks.ToString(Inv) + " physics samples.");
    }
    public override void OnApplicationQuit() { Finish(false, "Game exited before requested duration"); }
}
}
