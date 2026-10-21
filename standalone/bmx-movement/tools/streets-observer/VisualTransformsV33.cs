using System;
using System.Collections.Generic;
using UnityEngine;

namespace BmxMovementObserver {
public sealed partial class Observer {
    readonly Dictionary<int,Transform> visualTransformCache=new Dictionary<int,Transform>();
    readonly Dictionary<int,string> visualTransformRoles=new Dictionary<int,string>();
    void CacheVisualTransform(Transform transform,string role) {
        if(transform==null)return;int id=transform.GetInstanceID();visualTransformCache[id]=transform;
        if(!visualTransformRoles.ContainsKey(id))visualTransformRoles[id]=role;
    }
    void CacheRendererVisualTransforms(Renderer renderer) {
        if(renderer==null)return;
        CacheVisualTransform(renderer.transform,"renderer_transform");
        // Parent poses retain independent visual skeleton articulation without exporting assets.
        var root=vehicle.transform;
        for(var t=renderer.transform.parent;t!=null&&t!=root;t=t.parent){CacheVisualTransform(t,"renderer_ancestor");if(visualTransformCache.Count>=512)break;}
        var skin=renderer.TryCast<SkinnedMeshRenderer>();
        if(skin!=null){CacheVisualTransform(skin.rootBone,"skin_root");var bones=skin.bones;for(int i=0;i<bones.Length&&i<256&&visualTransformCache.Count<512;++i)CacheVisualTransform(bones[i],"skin_bone");}
    }
    object VisualTransformsV33() {
        var rows=new List<object>();
        foreach(var pair in visualTransformCache){var t=pair.Value;string role=visualTransformRoles[pair.Key];rows.Add(SafeChannel("visual_transform",()=>new{
            available=t!=null,id=pair.Key,role,transform=Target(t),
            chassis_relative_position=t!=null&&bindingAvailable&&body!=null?Vec(Quaternion.Inverse(body.rotation)*(t.position-body.worldCenterOfMass)):null,
            chassis_relative_rotation=t!=null&&bindingAvailable&&body!=null?Q(Quaternion.Inverse(body.rotation)*t.rotation):null}));}
        return new{available=rows.Count>0,selection_current=bindingAvailable,reference_role="Chassis worldCenterOfMass/rotation; Unity axes; XYZW",
            renderer_binding_inventory=articulationInventoryKey,transforms=rows,parent_constraints=VisualConstraintStates(),
            limitation="Observed visual transforms and ancestor chains only; no mesh vertices, skin weights, materials or textures exported"};
    }
}
}
