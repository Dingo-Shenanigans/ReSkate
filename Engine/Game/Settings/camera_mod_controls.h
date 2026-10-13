#pragma once

namespace dingosdk {
struct CameraModControls {
    bool available = false;
    bool low_variant = true;
    float low_x = -1.0f;
    float low_y = -0.4f;
    float low_z = 1.2f;
    bool light_enabled = true;
    float light_red = 0.6290379f;
    float light_green = 0.7490119f;
    float light_blue = 0.9115763f;
    float light_power = 3982.0f;
    float light_radius = 7.0f;
};
}
