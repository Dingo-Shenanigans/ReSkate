using System;
using System.Collections.Generic;
using UnityEngine;
using FM = Il2CppMashBox.BMX_Physics_Development.Animancer_Test.Animator_Motion_Systems.FullBodyMotionComponent;
using Limb = Il2CppMashBox.Core.Runtime.InverseKinematics.UnityIKLimb;

namespace BmxMovementObserver {
public sealed partial class Observer {
    readonly List<Limb> selectedLimbs=new List<Limb>();
    object motionBinding;
    void DiscoverLimbBindingsV32() {
        selectedLimbs.Clear();fullMotion=null;
        var all=Resources.FindObjectsOfTypeAll<FM>();var candidates=new List<object>();
        Transform hips=animator==null?null:animator.GetBoneTransform(HumanBodyBones.Hips);
        int matched=0;
        for(int i=0;i<all.Length&&i<4096;++i) {
            var candidate=all[i];if(candidate==null)continue;
            var direct=Read(candidate,"_animator") as Animator;
            var throughAnimancer=Read(Read(candidate,"_Animancer"),"Animator") as Animator;
            var candidateHips=Read(candidate,"_hipsBone") as Transform;
            bool directMatch=animator!=null&&direct!=null&&direct.GetInstanceID()==animator.GetInstanceID();
            bool animancerMatch=animator!=null&&throughAnimancer!=null&&throughAnimancer.GetInstanceID()==animator.GetInstanceID();
            bool hipsMatch=hips!=null&&candidateHips!=null&&hips.GetInstanceID()==candidateHips.GetInstanceID();
            bool match=directMatch||animancerMatch||hipsMatch;
            candidates.Add(new{reference=Reference(candidate),active=candidate.gameObject.activeInHierarchy,enabled=candidate.enabled,
                animator=Reference(direct),animancer_animator=Reference(throughAnimancer),hips=Reference(candidateHips),
                direct_match=directMatch,animancer_match=animancerMatch,hips_match=hipsMatch});
            if(match){++matched;if(fullMotion==null||(candidate.enabled&&candidate.gameObject.activeInHierarchy))fullMotion=candidate;}
        }
        var limbs=Resources.FindObjectsOfTypeAll<Limb>();
        for(int i=0;i<limbs.Length&&i<4096;++i){var limb=limbs[i];if(limb==null)continue;var a=Read(limb,"_animator") as Animator;
            if(animator!=null&&a!=null&&a.GetInstanceID()==animator.GetInstanceID())selectedLimbs.Add(limb);}
        motionBinding=new{available=fullMotion!=null,reason=fullMotion==null?"no_strong_binding_match_observed":null,
            search="Resources.FindObjectsOfTypeAll including inactive objects; exact assigned Animator or exact humanoid Hips only",
            selected_animator=Reference(animator),selected_human_hips=Reference(hips),global_motion_count=all.Length,matching_motion_count=matched,
            selected_motion=Reference(fullMotion),selected_motion_active=fullMotion!=null&&fullMotion.enabled&&fullMotion.gameObject.activeInHierarchy,
            global_ik_limb_count=limbs.Length,matching_ik_limb_count=selectedLimbs.Count,candidates};
    }
    object ContactTargetV32(object target) {
        if(target==null)return new{available=false,reason="target_missing"};
        return new{available=true,reference=Reference(target),transform=Target(Read(target,"transform") as Transform),
            state=Fields(target,"Active","ReferenceLimbPositionWeight","TargetID","GoalType","ExtraWristRoll","WristRollForwardOffset","_hasWristRollParent","_isLeftWrist"),
            reference_limb=Reference(Read(target,"ReferenceLimbIK")),wrist_roll_parent=Target(Read(target,"WristRollParent") as Transform)};
    }
    object LimbObservationsV32() {
        var limbs=new List<object>();foreach(var limb in selectedLimbs)limbs.Add(SafeChannel("ik_limb",()=>new{
            available=limb!=null,reference=Reference(limb),active=limb!=null&&limb.enabled&&limb.gameObject.activeInHierarchy,
            state=Fields(limb,"_Type","PositionWeight","RotationWeight","_PositionWeight","_OverridePositionWeight","_RotationWeight","_OverrideRotationWeight","_finalPositionWeight","_currentTargetID","_targetLerp","_timeSinceChangeTarget","_goalPosition","_goalRotation","Pole","_weightTransScale","_invertWeightValue","_mirrored"),
            end_joint=Target(Read(limb,"EndJoint") as Transform),weight_transform=Target(Read(limb,"_weightTrans") as Transform),
            mirrored_weight_transform=Target(Read(limb,"_mirroredWeightTrans") as Transform),working_weight_transform=Target(Read(limb,"workingWeightTrans") as Transform),target_id_transform=Target(Read(limb,"_targetIDTrans") as Transform),
            target=ContactTargetV32(Read(limb,"_target")),next_target=ContactTargetV32(Read(limb,"_nextTarget")),last_target=Reference(Read(limb,"_lastTarget"))}));
        var vehicleTargets=new Dictionary<string,object>();foreach(string field in new[]{"_leftFootIKTarget","_rightFootIKTarget","_leftHandIKTarget","_rightHandIKTarget"})vehicleTargets[field]=SafeChannel("vehicle_ik_target:"+field,()=>ContactTargetV32(Read(tuck,field)));
        return new{available=selectedLimbs.Count>0,motion_binding=motionBinding,limbs,vehicle_targets=vehicleTargets,
            limitation="Observed getter weights and target identities; no IK/animation setters or target activation events invoked"};
    }
}
}
