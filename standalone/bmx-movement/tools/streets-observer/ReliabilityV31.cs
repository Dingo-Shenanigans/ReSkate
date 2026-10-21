using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using UnityEngine;
using VC = Il2CppMashBox.BMX_Physics_Development.VehicleController;

namespace BmxMovementObserver {
public sealed partial class Observer {
    StreamWriter channelErrors;
    readonly Dictionary<string,int> channelErrorCounts=new Dictionary<string,int>();
    bool bindingAvailable,bindingLostSinceDiscovery;
    string bindingReason="not_sampled",fixedUtc;
    int currentVehicleId,validTicks,invalidTicks;
    double fixedElapsed,fixedSimulation;

    void ClearBindingCaches() {
        ik=null;animator=null;localInput=null;trickBrain=null;trickBrainV2=null;
        visualTransformCache.Clear();visualTransformRoles.Clear();visualConstraints.Clear();
        selectedLimbs.Clear();motionBinding=null;
        tuck=null;articulationInventoryKey=null;shapeCache.Clear();wheelCache.Clear();wheelContactCache.Clear();
        springBehaviour=null;fullMotion=null;catalogIdentity=null;selectedVehicleId=0;
        selectedBodies.Clear();selectedJoints.Clear();
    }
    void BeginFixedSample() {
        ++ticks;
        fixedElapsed=clock.Elapsed.TotalSeconds;fixedSimulation=Time.fixedTime;
        fixedUtc=DateTime.UtcNow.ToString("o");
        bindingAvailable=false;currentVehicleId=0;bindingReason="local_driver_vehicle_missing";
        try {
            var current=VC.LocalDriverVehicle;
            if(current==null) {bindingLostSinceDiscovery=true;return;}
            currentVehicleId=current.GetInstanceID();
            if(bindingLostSinceDiscovery||vehicle==null||body==null||selectedVehicleId!=currentVehicleId)Discover();
            if(vehicle==null||vehicle.GetInstanceID()!=currentVehicleId){bindingReason="vehicle_reacquisition_pending";return;}
            if(body==null){bindingReason="chassis_missing";return;}
            if(!body.gameObject.activeInHierarchy){bindingReason="chassis_inactive";return;}
            if(selectedVehicleId!=currentVehicleId){bindingReason="body_selection_pending";return;}
            bindingAvailable=true;bindingReason="current_local_vehicle";
        } catch(Exception ex) {bindingReason="binding_read_failed";bindingLostSinceDiscovery=true;ChannelError("binding",ex);}
    }
    object SafeChannel(string channel,Func<object> read) {
        try{return read();}catch(Exception ex){ChannelError(channel,ex);return null;}
    }
    void ChannelError(string channel,Exception ex) {
        ++errors;int count;channelErrorCounts.TryGetValue(channel,out count);channelErrorCounts[channel]=++count;
        if(count<=3||count%1000==0)LoggerInstance.Warning(channel+": "+ex.GetType().Name+": "+ex.Message);
        try {channelErrors?.WriteLine(JsonSerializer.Serialize(new{fixed_index=ticks-1,elapsed_seconds=clock.Elapsed.TotalSeconds,
            utc=DateTime.UtcNow.ToString("o"),channel,count,type=ex.GetType().FullName,message=ex.Message},Json));}catch{}
    }
    void WriteInvalidTelemetry() {
        var row=new string[47];for(int i=0;i<row.Length;++i)row[i]="NaN";
        row[0]=F(fixedElapsed);row[1]=Esc(fixedUtc);row[2]=F(fixedSimulation);row[3]=F(Time.fixedDeltaTime);
        row[4]=F(Physics.gravity.y);row[5]=F(Time.timeScale);row[6]="0";
        row[21]="0";row[22]=row[23]=Esc("");row[44]=(ticks-1).ToString(Inv);
        row[45]=Esc(bindingAvailable?"telemetry_read_failed":bindingReason);row[46]=currentVehicleId.ToString(Inv);
        telemetry.WriteLine(String.Join(",",row));
    }
}
}
