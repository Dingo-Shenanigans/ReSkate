using System;
using System.IO;
using System.Text.Json;
using UnityEngine;

namespace BmxMovementObserver {
public sealed partial class Observer {
    bool startRejected;
    int startReadAttempts;
    double firstStartAttemptRealtime=-1,nextStartAttemptRealtime;
    bool TryReadStartMarker(out string contents) {
        contents=null;double now=Time.realtimeSinceStartup;
        if(firstStartAttemptRealtime<0)firstStartAttemptRealtime=now;
        ++startReadAttempts;
        try {
            contents=File.ReadAllText(gate);
            using(var parsed=JsonDocument.Parse(contents))if(parsed.RootElement.ValueKind!=JsonValueKind.Object)throw new JsonException("Start marker must be one complete JSON object");
            return true;
        }catch(Exception ex)when(ex is IOException||ex is JsonException) {
            nextStartAttemptRealtime=now+.05;
            if(startReadAttempts>=40||now-firstStartAttemptRealtime>=2)RejectStart("start_marker_unavailable_after_bounded_retry",ex);
            return false;
        }catch(Exception ex){RejectStart("start_marker_read_failed",ex);return false;}
    }
    void RejectStart(string reason,Exception error) {
        startRejected=true;
        LoggerInstance.Error("Capture did not start: "+reason+": "+error.Message);
        try{WriteJson("capture-start-failure.json",new{schema=1,completed=false,started=false,reason,utc=DateTime.UtcNow.ToString("o"),gate,
            attempts=startReadAttempts,type=error.GetType().FullName,message=error.Message,channels_active=false});}catch{}
    }
}
}
