#pragma once
#include "client_source_spawn_internal.h"
#include "free_flight.h"

namespace dingosdk::client_source::detail {
// Input capture follows the lease, but placement needs proof that the native
// view still selects it. A late flag-picker camera reset can preserve every
// object identity while handing the local view back to mode zero.
inline bool verified_free_camera_view(const InteractiveDebug& debug, const SourceCameraSnapshot& camera) {
    return debug.camera_owned && !debug.camera_ambiguous && !debug.first_person &&
        camera.identity == debug.camera_identity && camera.mode == 1 &&
        camera.active == camera.identity.camera && camera.input_active && camera.view_enabled;
}

inline bool activate_free_camera(SourceTrial& trial, std::uintptr_t client, SourceCameraSnapshot& camera,
    DWORD error) {
    auto& debug = trial.debug;
    const bool recovering = debug.camera_owned;
    source_require(!debug.camera_ambiguous, "Native camera result is uncertain; restart before changing camera mode.");
    if (recovering) {
        source_require(camera.identity == debug.camera_identity, "The owned free camera changed.");
        if (camera.mode == 1) {
            source_require(camera.active == camera.identity.camera && camera.input_active && camera.view_enabled,
                "Another native camera has taken the local view.");
            return false;
        }
        source_require(!debug.first_person, "First person lost its native camera; exit it before enabling Freecam.");
    }
    source_require(camera.mode == 0 && camera.view_enabled && !camera.input_active &&
        camera.active != camera.identity.camera, "An active local view with no other camera override is required.");
    // Keep the last owned flight pose when native placement closes late. It
    // must not drag an already-flying route camera back to the skater.
    const auto pose = recovering ? debug.flight_matrix : debug_view_matrix(trial, camera);
    source_require(valid_flight_transform(pose), "Freecam pose is unavailable.");
    if (!recovering) debug.camera_identity = camera.identity;
    debug.camera_owned = true;
    debug.camera_ambiguous = true;
    SetLastError(error);
    trial.camera_mode(camera.identity.controller, 1);
    debug.camera_ambiguous = false;
    camera = source_camera_snapshot(trial, client);
    source_require(camera.identity == debug.camera_identity && camera.mode == 1 &&
        camera.active == camera.identity.camera && camera.input_active && camera.view_enabled,
        "Freecam activation could not be verified; disable Freecam to restore the view.");
    debug.flight_ready = false;
    SetLastError(error);
    trial.camera_transform(camera.identity.camera, &pose);
    debug.flight_matrix = pose;
    debug.flight_ready = true;
    return true;
}
}
