using System;
using System.Collections.Generic;
using UnityEngine;
using Tuck = Il2CppMashBox.BMX_Physics_Development.TuckUpTest;

namespace BmxMovementObserver {
public sealed partial class Observer {
    Tuck tuck;
    string articulationInventoryKey;
    int articulationInventorySerial;
    static readonly string[] BikeTransformFields={"_animatedVehicleRootJoint","_rootBikeJoint","_BBAnimationTarget","_leftPedalAnimationTarget","_rightPedalAnimationTarget"};

    string TransformPath(Transform t) {
        var names=new List<string>();for(int i=0;t!=null&&i<64;++i,t=t.parent)names.Add(t.name+"#"+t.GetInstanceID());
        names.Reverse();return String.Join("/",names);
    }
    Transform BikeTransform(string field){return Read(tuck,field) as Transform;}
    object ArticulatedTransform(Transform t) {
        if(t==null)return new{available=false,reason="transform_missing"};
        bool current=bindingAvailable&&body!=null;
        return new{available=true,transform=Target(t),path=TransformPath(t),
            local_scale=Vec(t.localScale),lossy_scale=Vec(t.lossyScale),
            chassis_reference=current?Reference(body):null,
            chassis_reference_origin=current?Vec(body.worldCenterOfMass):null,
            chassis_reference_rotation=current?Q(body.rotation):null,
            chassis_relative_position=current?Vec(Quaternion.Inverse(body.rotation)*(t.position-body.worldCenterOfMass)):null,
            chassis_relative_rotation=current?Q(Quaternion.Inverse(body.rotation)*t.rotation):null,
            position_reference_role="Chassis Rigidbody.worldCenterOfMass; unreflected Unity axes"};
    }
    object TuckFields() {
        return Fields(tuck,"_runTranlationAnimation","_wantedYPos","_YPos","_controlledAnchor","_curentRootJointPos","_lastRootJointPos","_initRootBikeJointLocalPos",
            "_additivePos","_additiveRot","_shralpPosition","_shralpRotation","_antiCaseContractAmount","_maxAntiCaseContractAmount","_antiCaseContractSpeed","_caseContract","_isDead","_isMirrored",
            "_forceSetDriveTrainBodiesToJointTargets","_forceDriveTrainBodyAngularVelocityToTargetDelta","_maxForcedDriveTrainAngularVelocity");
    }
    object BikeControlV32() {
        if(!bindingAvailable||tuck==null)return new{available=false,reason=bindingAvailable?"tuck_component_missing":"local_vehicle_binding_unavailable"};
        var transforms=new Dictionary<string,object>();foreach(string field in BikeTransformFields)transforms[field]=SafeChannel("bike_transform:"+field,()=>Target(BikeTransform(field)));
        return new{available=true,component=Reference(tuck),binding="TuckUpTest._configurableJoint matches selected vehicle._driveConnectionJoint",
            fields=TuckFields(),transforms,
            grind=Fields(Read(tuck,"_bikeGrindPoser"),"RootPosition","RootRotation"),
            joint=Reference(Read(tuck,"_configurableJoint")),
            balance=Fields(Read(tuck,"_VehicleBalancePid"),"_vehicleUpNormal","_humanUpNormal","_upRightNormal","_lerpedNormal")};
    }
    object BikeArticulationV32() {
        var transforms=new Dictionary<string,object>();foreach(string field in BikeTransformFields)transforms[field]=SafeChannel("articulation_transform:"+field,()=>ArticulatedTransform(BikeTransform(field)));
        return new{schema=1,available=tuck!=null,reason=tuck==null?"matching_tuck_component_missing":null,selection_current=bindingAvailable,
            component=Reference(tuck),transforms,fields=TuckFields(),
            joint=Reference(Read(tuck,"_configurableJoint")),
            renderer_binding_inventory=articulationInventoryKey,
            role="Actual animation control roots and targets read through the selected Spring joint consumer; no animation assets exported"};
    }
    void DiscoverArticulationV32() {
        if(vehicle==null||body==null)return;
        var expected=Read(vehicle,"_driveConnectionJoint") as Joint;
        if(tuck==null) {
            var all=vehicle.GetComponentsInChildren<Tuck>();
            for(int i=0;i<all.Length;++i) {
                var candidate=all[i];if(candidate==null)continue;
                var candidateJoint=Read(candidate,"_configurableJoint") as Joint;
                if(expected!=null&&candidateJoint!=null&&expected.GetInstanceID()==candidateJoint.GetInstanceID()){tuck=candidate;break;}
            }
        }
        string key=selectedVehicleId+":"+(tuck==null?0:tuck.GetInstanceID())+":"+(animator==null?0:animator.GetInstanceID());
        if(articulationInventoryKey!=null&&articulationInventoryKey.StartsWith(key+"|",StringComparison.Ordinal))return;
        string filename="bike-render-bindings-"+(++articulationInventorySerial).ToString("D3",Inv)+".json";
        visualTransformCache.Clear();visualTransformRoles.Clear();visualConstraints.Clear();
        var roots=new Dictionary<string,object>();foreach(string field in BikeTransformFields)roots[field]=ArticulatedTransform(BikeTransform(field));
        var renderers=new List<object>();var seen=new HashSet<int>();
        var vehicleRenderers=vehicle.GetComponentsInChildren<Renderer>();
        for(int i=0;i<vehicleRenderers.Length&&i<512;++i)AddRendererBinding(vehicleRenderers[i],"selected_vehicle_descendant",seen,renderers);
        var characterRoot=vehicle._characterManager==null?null:vehicle._characterManager.Root;
        if(characterRoot!=null){var rr=characterRoot.GetComponentsInChildren<Renderer>();for(int i=0;i<rr.Length&&i<512;++i)AddRendererBinding(rr[i],"selected_driver_character_descendant",seen,renderers);}
        CacheWheelGeometry();
        articulationInventoryKey=key+"|"+filename;
        WriteJson(filename,new{schema=1,utc=DateTime.UtcNow.ToString("o"),simulation_seconds=Time.time,vehicle=Reference(vehicle),chassis=Reference(body),
            motion_binding=motionBinding,tuck=Reference(tuck),tuck_binding_confirmed=tuck!=null,expected_spring_joint=Reference(expected),roots,renderers,visual_parent_constraints=VisualConstraintStates(),
            limitations=new[]{"Only current vehicle/driver descendant renderers are queried; inactive children may be absent.","Nearest-body and animation-root parent/bone relationships establish transform binding, not exact mesh geometry or animation curve extraction.","Transform coordinates are unreflected Unity world/local values. No meshes, materials, textures or clips are serialized."}});
    }
    void AddRendererBinding(Renderer r,string ownership,HashSet<int> seen,List<object> rows) {
        if(r==null||!seen.Add(r.GetInstanceID()))return;
        CacheRendererVisualTransforms(r);
        rows.Add(SafeChannel("renderer_binding",()=> {
            Rigidbody nearest=null;for(var t=r.transform;t!=null;t=t.parent){nearest=t.GetComponent<Rigidbody>();if(nearest!=null)break;}
            string bodyRole=null;foreach(var pair in selectedBodies)if(pair.Value!=null&&nearest!=null&&pair.Value.GetInstanceID()==nearest.GetInstanceID()){bodyRole=pair.Key;break;}
            var relations=new Dictionary<string,object>();foreach(string field in BikeTransformFields){var root=BikeTransform(field);relations[field]=new{root=Reference(root),renderer_transform_is_descendant=root!=null&&r.transform.IsChildOf(root)};}
            var bones=new List<object>();var skinned=r.TryCast<SkinnedMeshRenderer>();
            if(skinned!=null){var bb=skinned.bones;for(int i=0;i<bb.Length&&i<512;++i){var bone=bb[i];bones.Add(bone==null?null:new{reference=Reference(bone),path=TransformPath(bone),parent=Reference(bone.parent)});}}
            return new{renderer=Reference(r),ownership,enabled=r.enabled,transform=Target(r.transform),path=TransformPath(r.transform),
                nearest_rigidbody_ancestor=Reference(nearest),selected_body_role=bodyRole,parent_constraint_ancestors=ParentConstraintsForRenderer(r.transform),animation_root_relations=relations,
                skinned=skinned!=null,skin_root=skinned==null?null:Reference(skinned.rootBone),skin_bones=bones};
        }));
    }
}
}
