using System;
using System.Collections.Generic;
using UnityEngine;
using WP = Il2CppMashBox.Core.Runtime.Physics.Vehicle.WheelPhysics;
using BC = Il2CppMashBox.BMX_Physics_Development.BMXWheelContactInfo;

namespace BmxMovementObserver {
public sealed partial class Observer {
    readonly Dictionary<int,List<Collider>> shapeCache=new Dictionary<int,List<Collider>>();
    readonly Dictionary<int,WP> wheelCache=new Dictionary<int,WP>();
    readonly Dictionary<int,BC> wheelContactCache=new Dictionary<int,BC>();
    void CacheWheelGeometry() {
        shapeCache.Clear();wheelCache.Clear();wheelContactCache.Clear();
        foreach(var b in selectedBodies.Values)if(b!=null) {
            var list=new List<Collider>();var all=b.GetComponentsInChildren<Collider>();
            for(int i=0;i<all.Length;++i)if(all[i]!=null&&all[i].attachedRigidbody==b)list.Add(all[i]);shapeCache[b.GetInstanceID()]=list;
        }
        var wheels=vehicle.GetComponentsInChildren<WP>();
        for(int i=0;i<wheels.Length;++i){var w=wheels[i];if(w==null)continue;var b=Read(w,"_physicsBody") as Rigidbody;if(b!=null)wheelCache[b.GetInstanceID()]=w;}
        var contacts=vehicle.GetComponentsInChildren<BC>();
        for(int i=0;i<contacts.Length;++i){var c=contacts[i];if(c==null)continue;var b=Read(c,"_body") as Rigidbody;if(b!=null)wheelContactCache[b.GetInstanceID()]=c;}
    }
    object CollisionShapes(Rigidbody b) {
        var result=new List<object>();List<Collider> shapes;if(b==null||!shapeCache.TryGetValue(b.GetInstanceID(),out shapes))return result;
        foreach(var c in shapes)result.Add(SafeChannel("collider_geometry",()=> {
            if(c==null)return new{available=false};
            var bounds=c.bounds;var sphere=c.TryCast<SphereCollider>();var capsule=c.TryCast<CapsuleCollider>();var box=c.TryCast<BoxCollider>();var mesh=c.TryCast<MeshCollider>();
            return new{available=true,reference=Reference(c),type=c.GetIl2CppType().FullName,transform=Target(c.transform),attached_body=Reference(c.attachedRigidbody),
                enabled=c.enabled,is_trigger=c.isTrigger,contact_offset=c.contactOffset,world_bounds_center=Vec(bounds.center),world_bounds_size=Vec(bounds.size),
                local_shape=sphere!=null?(object)new{kind="sphere",center=Vec(sphere.center),radius=sphere.radius}:capsule!=null?new{kind="capsule",center=Vec(capsule.center),radius=capsule.radius,height=capsule.height,direction=capsule.direction}:box!=null?new{kind="box",center=Vec(box.center),size=Vec(box.size)}:null,
                mesh_convex=mesh==null?(bool?)null:mesh.convex,mesh_identity=mesh==null?null:Reference(mesh.sharedMesh)};
        }));return result;
    }
    object WheelGeometry(Rigidbody b) {
        WP w;BC contact;if(b==null||!wheelCache.TryGetValue(b.GetInstanceID(),out w)||w==null)return null;
        wheelContactCache.TryGetValue(b.GetInstanceID(),out contact);
        return new{reference=Reference(w),body=Reference(b),
            state=Fields(w,"WheelRadius","NominalContactPoint","NominalContactDown","Point","Normal","HasPhysicalContact","HasHit","_wheelRadius","_wheelWidth","_hasContact","_hasHitPhysical","_wheelPointVel","_groundPointVel","_forwardSlipVel","_frictionForcePosition","_contactNormal","_lastContactNormal","_wheelLoad"),
            collider=Reference(Read(w,"_wheelCollider")),base_transform=Target(Read(w,"_wheelBaseTrans") as Transform),visual_center=Target(Read(w,"_visualWheelCenter") as Transform),
            contact_provider=Reference(Read(w,"_contactInfoCached")),bmx_contact=Reference(contact),
            bmx_contact_state=Fields(contact,"Point","Normal","HasHit","NumberOfRays","Radius","_castDist","_castUpStart","_preferUp","_point","_internalNormal","_wheelInZeroContactMode","_hasContact","_inflated")};
    }
}
}
