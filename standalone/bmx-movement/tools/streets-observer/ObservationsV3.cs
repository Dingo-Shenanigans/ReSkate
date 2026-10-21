using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using UnityEngine;
using Sync = Il2CppMashBox.Core.Runtime.TrickSystem.SyncTrickAnimationData;
using NativeObject = Il2CppInterop.Runtime.InteropTypes.Il2CppObjectBase;
using SpringBehaviour = Il2CppMashBox.BMX_Physics_Development.SpringDynamicsBehaviour;
using ManagedSystem = Il2CppMashBox.Core.Runtime.Physics.ManagedRigidbodySystem;
using FullMotion = Il2CppMashBox.BMX_Physics_Development.Animancer_Test.Animator_Motion_Systems.FullBodyMotionComponent;

namespace BmxMovementObserver {
public sealed partial class Observer {
    static readonly JsonSerializerOptions Json = new JsonSerializerOptions { NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals };
    StreamWriter bodyPhysics, trickStates;
    double boneInterval=.1, physicsInterval=.01, lastBones=-1, lastPhysics=-1;
    int boneCount, physicsCount, selectedVehicleId;
    string catalogIdentity;
    readonly Dictionary<string,Rigidbody> selectedBodies=new Dictionary<string,Rigidbody>();
    readonly Dictionary<int,Joint[]> selectedJoints=new Dictionary<int,Joint[]>();
    SpringBehaviour springBehaviour;
    FullMotion fullMotion;
    static readonly string[] BodyNames={"Spring Body","Chassis Body","Front Wheel Front","Back Wheel Back","Headset","BB Body","Left Pedal Body","Right Pedal Body"};

    object Fields(object value,params string[] names) {
        var result=new Dictionary<string,object>();
        foreach(string name in names)result[name]=Deep(Read(value,name),1);
        return result;
    }
    object Reference(object value) {
        if(value==null)return null;
        var u=value as UnityEngine.Object;
        return new{type=value.GetType().FullName,id=u==null?0:u.GetInstanceID(),name=u==null?null:u.name,
            native_pointer=Convert.ToString(Read(value,"Pointer"),Inv)};
    }
    object Clip(object value) {
        var clip=value as AnimationClip;
        if(clip==null)return Reference(value);
        return new{reference=Reference(clip),length_seconds=clip.length,frame_rate=clip.frameRate,is_looping=clip.isLooping,
            is_human_motion=clip.isHumanMotion,legacy=clip.legacy,wrap_mode=clip.wrapMode.ToString()};
    }
    List<object> Items(object value,int maximum=256) {
        var result=new List<object>();if(value==null)return result;
        double count=N(value,"Length");if(Double.IsNaN(count))count=N(value,"Count");
        if(Double.IsNaN(count)||count<0)return result;
        PropertyInfo item=null;
        foreach(var candidate in value.GetType().GetProperties(BindingFlags.Instance|BindingFlags.Public)) {
            var pars=candidate.GetIndexParameters();
            if(candidate.Name=="Item"&&pars.Length==1&&(pars[0].ParameterType==typeof(int)||pars[0].ParameterType==typeof(long))){item=candidate;break;}
        }
        if(item==null)return result;
        for(int i=0;i<Math.Min(maximum,(int)count);++i)try {
            object index=item.GetIndexParameters()[0].ParameterType==typeof(long)?(object)(long)i:i;
            result.Add(item.GetValue(value,new[]{index}));
        }catch{result.Add(null);}
        return result;
    }
    object Deep(object value,int depth) {
        if(value==null)return null;
        if(value is Vector3)return Vec((Vector3)value);
        if(value is Vector2){var v=(Vector2)value;return new double[]{v.x,v.y};}
        if(value is Quaternion)return Q((Quaternion)value);
        if(value is AnimationClip)return Clip(value);
        Type type=value.GetType();
        if(type.IsEnum)return value.ToString();
        if(type.IsPrimitive||value is string||value is decimal)return value;
        if(depth>0) {
            var values=Items(value);if(values.Count>0){var list=new List<object>();foreach(var v in values)list.Add(Deep(v,depth-1));return list;}
        }
        return Reference(value);
    }
    object Drive(JointDrive d) {return new{spring=d.positionSpring,damper=d.positionDamper,maximum_force=d.maximumForce,use_acceleration=d.useAcceleration};}
    object Limit(SoftJointLimit d){return new{limit=d.limit,bounciness=d.bounciness,contact_distance=d.contactDistance};}
    object LimitSpring(SoftJointLimitSpring d){return new{spring=d.spring,damper=d.damper};}
    object JointState(Joint joint) {
        if(joint==null)return null;
        var c=joint.TryCast<ConfigurableJoint>();
        object configuration=null;
        if(c!=null)configuration=new{
            configured_in_world_space=c.configuredInWorldSpace,swap_bodies=c.swapBodies,
            x_motion=c.xMotion.ToString(),y_motion=c.yMotion.ToString(),z_motion=c.zMotion.ToString(),
            angular_x_motion=c.angularXMotion.ToString(),angular_y_motion=c.angularYMotion.ToString(),angular_z_motion=c.angularZMotion.ToString(),
            secondary_axis=Vec(c.secondaryAxis),linear_limit=Limit(c.linearLimit),linear_limit_spring=LimitSpring(c.linearLimitSpring),
            low_angular_x_limit=Limit(c.lowAngularXLimit),high_angular_x_limit=Limit(c.highAngularXLimit),
            angular_y_limit=Limit(c.angularYLimit),angular_z_limit=Limit(c.angularZLimit),
            angular_x_limit_spring=LimitSpring(c.angularXLimitSpring),angular_yz_limit_spring=LimitSpring(c.angularYZLimitSpring),
            x_drive=Drive(c.xDrive),y_drive=Drive(c.yDrive),z_drive=Drive(c.zDrive),angular_x_drive=Drive(c.angularXDrive),
            angular_yz_drive=Drive(c.angularYZDrive),slerp_drive=Drive(c.slerpDrive),rotation_drive_mode=c.rotationDriveMode.ToString(),
            target_position=Vec(c.targetPosition),target_velocity=Vec(c.targetVelocity),target_rotation=Q(c.targetRotation),
            target_angular_velocity=Vec(c.targetAngularVelocity),projection_mode=c.projectionMode.ToString(),
            projection_distance=c.projectionDistance,projection_angle=c.projectionAngle};
        return new{reference=Reference(joint),connected_body=Reference(joint.connectedBody),anchor=Vec(joint.anchor),
            connected_anchor=Vec(joint.connectedAnchor),axis=Vec(joint.axis),auto_configure_connected_anchor=joint.autoConfigureConnectedAnchor,
            mass_scale=joint.massScale,connected_mass_scale=joint.connectedMassScale,enable_collision=joint.enableCollision,
            enable_preprocessing=joint.enablePreprocessing,current_force=Vec(joint.currentForce),current_torque=Vec(joint.currentTorque),
            break_force=joint.breakForce,break_torque=joint.breakTorque,configuration};
    }
    object BodyState(string role,Rigidbody b) {
        if(b==null)return new{role,available=false};
        var joints=new List<object>();Joint[] cached;
        if(selectedJoints.TryGetValue(b.GetInstanceID(),out cached))foreach(var j in cached)joints.Add(SafeChannel("joint",()=>JointState(j)));
        return new{role,available=true,reference=Reference(b),transform=Target(b.transform),position=Vec(b.position),rotation=Q(b.rotation),
            local_center_of_mass=Vec(b.centerOfMass),world_center_of_mass=Vec(b.worldCenterOfMass),velocity=Vec(b.velocity),angular_velocity=Vec(b.angularVelocity),
            collision_shapes=SafeChannel("collision_shapes",()=>CollisionShapes(b)),wheel_geometry=SafeChannel("wheel_geometry",()=>WheelGeometry(b)),
            mass=b.mass,inertia=Vec(b.inertiaTensor),inertia_rotation=Q(b.inertiaTensorRotation),use_gravity=b.useGravity,
            drag=b.drag,angular_drag=b.angularDrag,is_kinematic=b.isKinematic,is_sleeping=b.IsSleeping(),constraints=b.constraints.ToString(),
            max_angular_velocity=b.maxAngularVelocity,solver_iterations=b.solverIterations,solver_velocity_iterations=b.solverVelocityIterations,joints};
    }
    void DiscoverV3() {
        if(vehicle==null||body==null)return;
        int id=vehicle.GetInstanceID();
        if(selectedVehicleId!=id||selectedBodies.Count==0||!selectedBodies.ContainsKey("Chassis Body")||selectedBodies["Chassis Body"]!=body) {
            selectedVehicleId=id;selectedBodies.Clear();selectedJoints.Clear();springBehaviour=null;
            foreach(string name in BodyNames)selectedBodies[name]=null;
            var candidates=vehicle.GetComponentsInChildren<Rigidbody>();
            for(int i=0;i<candidates.Length;++i){var b=candidates[i];if(b!=null&&selectedBodies.ContainsKey(b.name)&&selectedBodies[b.name]==null)selectedBodies[b.name]=b;}
            selectedBodies["Chassis Body"]=body;
            if(vehicle.DriverSeatBody!=null)selectedBodies["Spring Body"]=vehicle.DriverSeatBody;
            foreach(var b in selectedBodies.Values)if(b!=null){var array=b.GetComponents<Joint>();var list=new List<Joint>();for(int i=0;i<array.Length;++i)if(array[i]!=null)list.Add(array[i]);selectedJoints[b.GetInstanceID()]=list.ToArray();}
            var springs=vehicle.GetComponentsInChildren<SpringBehaviour>();if(springs.Length>0)springBehaviour=springs[0];
        }
        if(animator!=null&&fullMotion==null){var motion=animator.GetComponentsInParent<FullMotion>();if(motion.Length>0)fullMotion=motion[0];}
        if(animator!=null&&fullMotion==null){var root=animator.transform.root;var motion=root.GetComponentsInChildren<FullMotion>();for(int i=0;i<motion.Length;++i)if(Read(motion[i],"_animator") as Animator==animator){fullMotion=motion[i];break;}}
        DiscoverLimbBindingsV32();
        DiscoverArticulationV32();
        ExportCatalog(false);
    }
    Sync AsSync(object value) {try{return (value as NativeObject)?.TryCast<Sync>();}catch{return null;}}
    object SyncData(object raw) {
        var data=AsSync(raw);if(data==null)return new{available=false,reference=Reference(raw)};
        var clips=new Dictionary<string,object>();
        foreach(string who in new[]{"Player","Vehicle"})foreach(string phase in new[]{"Enter","Tweak","Loop","Exit"})foreach(string mirror in new[]{"","Mirror"}) {
            string name=who+phase+"Clip"+mirror;clips[name]=Clip(Read(data,name));
        }
        return new{available=true,reference=Reference(data),trick_name=data.TrickName,points=data.Points,
            speed=Fields(data,"OverallSpeedMult","EnterSpeedMult","LoopSpeedMult","TweakSpeedMult","ExitSpeedMult","TweakBeginBlendNormalizedTime"),
            only_fire_if_in_air=data.OnlyFireIfInAir,allow_landing_holding=data.AllowLandingHolding,
            player_animation_data=Reference(Read(data,"_playerAnimationData")),vehicle_animation_data=Reference(Read(data,"_vehicleAnimationData")),
            mirror_sync_data=Reference(Read(data,"_mirrorSyncData")),clips};
    }
    object CatalogTable(object data,string role) {
        var sets=Items(Read(data,"TrickSets"),8);var rows=new List<object>();
        string[] modifiers={"LB+RT","RB+LT","LB+RB","LB","RB","LT+RT","LT","RT"};
        for(int set=0;set<8;++set){object definition=set<sets.Count?sets[set]:null;var slots=Items(Read(definition,"_dataList"),8);
            for(int slot=0;slot<8;++slot)rows.Add(new{set_id=set,slot_id=slot,modifier=modifiers[set],set_definition=Reference(definition),
                input_action=Deep(Read(definition,"InputActionType"),0),input_action_name=Deep(Read(definition,"InputActionName"),0),
                data=SyncData(slot<slots.Count?slots[slot]:null)});
        }
        return new{role,reference=Reference(data),set_count_observed=sets.Count,slots=rows};
    }
    List<object> CanonicalSlots(object data) {
        var sets=Items(Read(data,"TrickSets"),8);var rows=new List<object>();
        for(int set=0;set<8;++set){var slots=Items(set<sets.Count?Read(sets[set],"_dataList"):null,8);
            for(int slot=0;slot<8;++slot){var candidate=AsSync(slot<slots.Count?slots[slot]:null);rows.Add(new{set,slot,available=candidate!=null,name=candidate==null?null:candidate.TrickName,object_id=candidate==null?0:candidate.GetInstanceID()});}}
        return rows;
    }
    void ExportCatalog(bool force) {
        if(trickBrainV2==null)return;
        object fallback=Read(trickBrainV2,"_trickDataSets");
        object provider=Read(trickBrainV2,"_vehicleTrickSettingsProvider");
        object active=Read(provider,"VehicleTrickDataSets");
        object settings=Read(vehicle.MotorVehicleSettings,"TrickDataSets");
        string key=selectedVehicleId+":"+Convert.ToString(Read(fallback,"Pointer"))+":"+Convert.ToString(Read(active,"Pointer"));
        if(!force&&key==catalogIdentity)return;catalogIdentity=key;
        WriteJson("trick-catalog.json",new{schema=3,utc=DateTime.UtcNow.ToString("o"),source="Observed active Unity objects and read-only getters",
            source_gameassembly_sha256="22a4ac9cbec7d270e96dcc90b9870f69f91596dd974fbb1c874b1b5e081a94b8",
            source_metadata_sha256="7e1af5100b90a78c907999239bc6952dfdce2461fc4321626658f24f2815d7ea",
            vehicle=Reference(vehicle),brain=Reference(trickBrainV2),provider=Reference(provider),
            canonical_role=active!=null?"vehicle_provider":"brain_fallback",slots=CanonicalSlots(active??fallback),
            tables=new[]{CatalogTable(active,"vehicle_provider"),CatalogTable(settings,"vehicle_settings"),CatalogTable(fallback,"brain_fallback")},
            brain_settings=Fields(trickBrainV2,"requireZeroBeforeChange","brakeTriggerThreshold","brakeJumpTopSlotGuardWindow"),
            controller_settings=Fields(Read(trickBrainV2,"_trickController"),"layerBringUp","layerBringDown","launchFade","autoTweak","autoTweakAt","useLoopPhase","holdIfNoLoop","exitStartBiasFromHold","exitFadeFromHold"),
            animator_settings=Fields(Read(trickBrainV2,"_trickAnimator"),"baseLayerIndex","trickLayerIndex","baseSpeed","defaultEnterFade","defaultTweakFade","defaultLoopFade","defaultExitFade","MasterSpeed","Mirror"),
            limitations=new[]{"Raw tables preserve their roles; slot resolution may use per-vehicle overrides. No Fire/Resolve/Play/animation setters are invoked.","Clip identity, duration and live playback state do not provide extracted animation curves.","No release weights or successful scoring completion are inferred from a selected slot."}});
    }
    object AnimancerState(object value) {
        if(value==null)return null;
        return new{reference=Reference(value),clip=Clip(Read(value,"Clip")),playback=Fields(value,"Time","NormalizedTime","Length","Speed","EffectiveSpeed","Weight","EffectiveWeight","IsPlaying","IsLooping","NormalizedEndTime"),parent=Reference(Read(value,"Parent"))};
    }
    object Dual(object value){return value==null?null:new{player=AnimancerState(Read(value,"Player")),vehicle=AnimancerState(Read(value,"Vehicle"))};}
    object AnimationState() {
        object controller=Read(trickBrainV2,"_trickController"),character=Read(trickBrainV2,"_character"),anim=Read(trickBrainV2,"_trickAnimator");
        return new{brain=Reference(trickBrainV2),brain_state=Fields(trickBrainV2,"setId","slotId","_lastFiredSetId","_lastFiredSlotId","_nextSlotID","_timeAtLastFire","TimeSinceLastFire","_zeroedSinceLastFire","_enteredGrounded","_wasGrounded","_airLaunchGuardUntil","_requireNeutralAfterBrakeJumpTopSlot"),
            controller=Reference(controller),controller_state=Fields(controller,"_phase","_firedSetId","_firedSlotId","_curSetId","_curSlotId","_loopCount","_tweaked","_exiting","_loopRequestedEnd","_zerodSinceFire","_requestedTweak","IsBusy","IsExiting","IsHoldingTrick"),
            data=Reference(Read(controller,"_data")),enter=Dual(Read(controller,"_enter")),tweak=Dual(Read(controller,"_tweak")),loop=Dual(Read(controller,"_loop")),exit=Dual(Read(controller,"_exit")),
            character=Fields(character,"MasterSpeedMult","SlotTrickSpeedMultiplier","_mirrorTricks","_canFireTricks","_enteredAir","_allowTrickLanders","_canBail"),
            character_current=Reference(Read(character,"_currentTrick")),character_last=Reference(Read(character,"_lastTrick")),animator=Reference(anim)};
    }
    void StartV3() {
        articulationInventoryKey=null;
        boneCount=physicsCount=0;validTicks=invalidTicks=0;channelErrorCounts.Clear();lastBones=lastPhysics=-1;



        DiscoverV3();ExportCatalog(true);
        WriteJson("observation-schema.json",new{schema=3,fixed_hook="MelonMod.OnFixedUpdate",clock_origin_utc=startedUtc.ToString("o"),
            start_utc_seconds=new DateTimeOffset(startedUtc).ToUnixTimeMilliseconds()/1000.0,physics_hz=1/physicsInterval,bone_hz=1/boneInterval,frame_hz=1/interval,
            body_roles=BodyNames,body_coordinates="Unity world-space SI; quaternion XYZW; local joint anchors and inertia rotation explicitly identified",
            shared_alignment="fixed_index counts every OnFixedUpdate callback including invalid binding rows and matches telemetry row index; elapsed_seconds uses capture Stopwatch; simulation_seconds uses Time.fixedTime",
            animation_phase_enum=new{Idle=0,Launch=1,Tweak=2,Loop=3,Hold=4,Exit=5},
            limitations=new[]{"Joint.currentForce/currentTorque are Unity-reported reaction values, not reconstructed force decomposition.","Callback order relative to game scripts is not established; selection uses active local vehicle ownership.","All observations are read-only. Missing data remain null; no state transitions are synthesized as game scoring events."}});
    }
    void FixedV3() {
        double t=fixedElapsed,simulation=fixedSimulation;
        if(physicsInterval>Time.fixedDeltaTime+1e-6&&simulation-lastPhysics+.0001<physicsInterval)return;lastPhysics=simulation;
        string utc=fixedUtc;
        var bodies=new List<object>();foreach(string role in BodyNames) {
            Rigidbody selected;selectedBodies.TryGetValue(role,out selected);
            bodies.Add(bindingAvailable?SafeChannel("body:"+role,()=>BodyState(role,selected)):new{role,available=false,reason=bindingReason});
        }
        var managed=bindingAvailable?(Read(vehicle,"_managedRigidbody") as NativeObject)?.TryCast<ManagedSystem>():null;
        var collision=bindingAvailable?Read(vehicle,"_bmxCollisionHandler"):null;
        try {
            bodyPhysics.WriteLine(JsonSerializer.Serialize(new{fixed_index=ticks-1,elapsed_seconds=t,utc,simulation_seconds=simulation,
                sample_valid=bindingAvailable,binding_reason=bindingReason,current_vehicle_id=currentVehicleId,selection_vehicle_id=selectedVehicleId,
                bodies,bike_control=SafeChannel("bike_control",()=>BikeControlV32()),drive_connection_joint=bindingAvailable?SafeChannel("drive_joint",()=>JointState(Read(vehicle,"_driveConnectionJoint") as Joint)):null,
                spring=bindingAvailable?Fields(springBehaviour,"_linearLimitGrounded","_linearLimitAir","_linearYDownDamper","_linearYUpDamper","_lerpDampSpeed","_linearYUpVelDampThreshold","_currentLinearYDamperValue","_wantedLinearYDamperValue","_currentSlerpDamperValue","_wantedSlerpDamperValue","_currentLinearLimitValue","_wantedLinearLimitValue"):null,
                vehicle_state=bindingAvailable?Fields(vehicle,"PreloadInput","ExtraLeanValue","_lastJumpForce","_lastJumpDir","_lastSideHopForce","_timeAtBeginJump","_timeAtLastJump","_userGravitySmartDataNew","_disableTicks","_driverInertiaBlend","_inertiaBlend","_timeSinceGrounded"):null,
                input=Fields(localInput,"MoveInput","MoveInputRaw","TrickStickInput","JumpInput","AccelerationInput","BrakeInput","FrontBrakeInput","RearBrakeInput","PreloadInput","MannyInput","NoseyInput","PumpInput","_Buttons","_OneShots","_localControlSampleSequence","_standUpPressed","_reSpawnPressed","_placeMarkerPressed","_canInput","_restrictManny","_restrictPreload","_restrictNosey"),
                input_binding_current=bindingAvailable,grounded=Fields(collision,"Grounded","_frontEndGrounded","_backEndGrounded","_timeSinceGrounded"),
                managed_rigidbody=bindingAvailable?Reference(Read(vehicle,"_managedRigidbody")):null,managed_body_list=Deep(Read(managed,"_systembodies"),1)},Json));
            ++physicsCount;
        } catch(Exception ex) {ChannelError("body_physics_write",ex);}
        try {trickStates.WriteLine(JsonSerializer.Serialize(new{fixed_index=ticks-1,elapsed_seconds=t,utc,simulation_seconds=simulation,
            selection_current=bindingAvailable,binding_reason=bindingReason,selection_vehicle_id=selectedVehicleId,current_vehicle_id=currentVehicleId,
            animation=SafeChannel("animation_state",()=>AnimationState())},Json));}catch(Exception ex){ChannelError("trick_states_write",ex);}
        if(ticks%100==0){bodyPhysics.Flush();trickStates.Flush();channelErrors.Flush();}
    }
    void BonesV3() {
        double t=clock.Elapsed.TotalSeconds;if(t-lastBones<boneInterval)return;lastBones=t;
        rider.WriteLine(JsonSerializer.Serialize(new{index=boneCount,elapsed_seconds=t,utc=DateTime.UtcNow.ToString("o"),simulation_seconds=Time.time,
            fixed_index=ticks-1,selection_current=bindingAvailable,binding_reason=bindingReason,current_vehicle_id=currentVehicleId,chassis_body_id=body==null?0:body.GetInstanceID(),vehicle_id=vehicle==null?0:vehicle.GetInstanceID(),
            animator_id=animator==null?0:animator.GetInstanceID(),animator_transform=animator==null?null:Target(animator.transform),
            pose_reference=SafeChannel("pose_reference",()=>PoseReference()),pose_reference_candidates=SafeChannel("pose_candidates",()=>PoseCandidates()),
            motion_component=Reference(fullMotion),motion_fields=Fields(fullMotion,"_effectWeight","_goalWeight","_effectiveWeightOverride","_mirrorWeight","_hipsPositionOffset","_hipsRotationOffset","_additiveHipsPositionOffset","_hipsPositionOffsetOverride","_hipTwistYawOverride","_hipsXRotationOverride","_hipsYRotationOverride","_hipsZRotationOverride","_spine0XRotationOverride","_spine0YRotationOverride","_spine0ZRotationOverride"),
            visual_transforms=SafeChannel("visual_transforms",()=>VisualTransformsV33()),
            limb_observations=SafeChannel("limb_observations",()=>LimbObservationsV32()),
            bike_articulation=SafeChannel("bike_articulation",()=>BikeArticulationV32()),
            helper_transforms=SafeChannel("motion_helpers",()=>MotionHelpers()),
            anchors=ik==null?null:new{reference=Reference(ik),left_hand=Target(ik._leftHandAnchor),right_hand=Target(ik._rightHandAnchor),left_foot=Target(ik._leftFootAnchor),right_foot=Target(ik._rightFootAnchor),
                left_hand_target=Target(ik._leftHandTarget),right_hand_target=Target(ik._rightHandTarget),left_foot_target=Target(ik._leftFootTarget),right_foot_target=Target(ik._rightFootTarget)},
            hips=Bone(HumanBodyBones.Hips),head=Bone(HumanBodyBones.Head),left_hand=Bone(HumanBodyBones.LeftHand),right_hand=Bone(HumanBodyBones.RightHand),left_foot=Bone(HumanBodyBones.LeftFoot),right_foot=Bone(HumanBodyBones.RightFoot),
            left_knee=Bone(HumanBodyBones.LeftLowerLeg),right_knee=Bone(HumanBodyBones.RightLowerLeg),left_elbow=Bone(HumanBodyBones.LeftLowerArm),right_elbow=Bone(HumanBodyBones.RightLowerArm),bones=AllBones(),animation=SafeChannel("bone_animation",()=>AnimationState())},Json));
        ++boneCount;if(boneCount%10==0)rider.Flush();
    }
    object PoseBody(Rigidbody holder,string kind,bool bound) {return holder==null?null:new{id=holder.GetInstanceID(),name=holder.name,
        position=Vec(holder.worldCenterOfMass),rotation=Q(holder.rotation),kind,skeleton_bound=bound,
        binding_evidence=bound?"Observed Animator transform is this body's transform or its descendant":"Spring reference is not an ancestor of the selected Animator",
        transform=Target(holder.transform),position_role="Rigidbody.worldCenterOfMass"};}
    object PoseReference() {
        if(animator==null)return null;
        for(var t=animator.transform;t!=null;t=t.parent){var holder=t.GetComponent<Rigidbody>();if(holder!=null)return PoseBody(holder,"nearest_rigidbody_ancestor_of_humanoid_animator",true);}
        return null;
    }
    object PoseCandidates() {
        var list=new List<object>();
        if(animator!=null)for(var t=animator.transform;t!=null;t=t.parent){var holder=t.GetComponent<Rigidbody>();if(holder!=null)list.Add(PoseBody(holder,"rigidbody_ancestor",true));}
        Rigidbody spring;if(selectedBodies.TryGetValue("Spring Body",out spring)&&spring!=null)list.Add(PoseBody(spring,"selected_spring_body",animator!=null&&animator.transform.IsChildOf(spring.transform)));
        return list;
    }
    object MotionHelpers() {
        var result=new Dictionary<string,object>();
        foreach(string name in new[]{"_leftHandIKWeightTrans","_leftHandIKTargetIDTrans","_rightHandIKWeightTrans","_rightHandIKTargetIDTrans","_leftFootIKWeightTrans","_leftFootIKTargetIDTrans","_rightFootIKWeightTrans","_rightFootIKTargetIDTrans"})result[name]=Target(Read(fullMotion,name) as Transform);
        return result;
    }
}
}
