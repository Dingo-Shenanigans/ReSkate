using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Animations;

namespace BmxMovementObserver {
public sealed partial class Observer {
    readonly Dictionary<int,ParentConstraint> visualConstraints=new Dictionary<int,ParentConstraint>();
    string SourceBodyRole(Transform source) {
        for(var t=source;t!=null;t=t.parent)foreach(var p in selectedBodies)if(p.Value!=null&&p.Value.transform.GetInstanceID()==t.GetInstanceID())return p.Key;
        return null;
    }
    object ParentConstraintState(ParentConstraint c) {
        if(c==null)return new{available=false};
        var sources=new List<object>();
        for(int i=0;i<c.sourceCount&&i<32;++i){var source=c.GetSource(i);sources.Add(new{index=i,transform=Target(source.sourceTransform),path=TransformPath(source.sourceTransform),
            selected_body_role=SourceBodyRole(source.sourceTransform),weight=source.weight,translation_offset=Vec(c.GetTranslationOffset(i)),rotation_offset_degrees=Vec(c.GetRotationOffset(i))});}
        return new{available=true,reference=Reference(c),transform=Target(c.transform),path=TransformPath(c.transform),enabled=c.enabled,
            active_in_hierarchy=c.gameObject.activeInHierarchy,constraint_active=c.constraintActive,locked=c.locked,weight=c.weight,
            source_count=c.sourceCount,translation_axis=c.translationAxis.ToString(),rotation_axis=c.rotationAxis.ToString(),
            translation_at_rest=Vec(c.translationAtRest),rotation_at_rest_degrees=Vec(c.rotationAtRest),sources};
    }
    object ParentConstraintsForRenderer(Transform transform) {
        var result=new List<object>();
        for(var t=transform;t!=null;t=t.parent){var all=t.GetComponents<ParentConstraint>();for(int i=0;i<all.Length;++i){var c=all[i];if(c==null)continue;visualConstraints[c.GetInstanceID()]=c;result.Add(Reference(c));}if(t==vehicle.transform)break;}
        return result;
    }
    object VisualConstraintStates() {
        var result=new List<object>();foreach(var pair in visualConstraints)result.Add(SafeChannel("parent_constraint",()=>ParentConstraintState(pair.Value)));return result;
    }
}
}
