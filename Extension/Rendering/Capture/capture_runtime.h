#pragma once
#include "capture_recorder.h"

namespace dingosdk::overlay { struct FrameGrabHost; }

// The process's one screen recorder (reskate.capture), fed by the overlay's frame grab, and the menu's
// fonts its cards are drawn in (built into ReSkate.dll as FONT_HEADING, FONT_BODY and FONT_BRUSH).
namespace dingosdk::capture {
Recorder& recorder() noexcept;
// What the overlay asks before drawing each frame (overlay::set_frame_grab); process lifetime.
const overlay::FrameGrabHost* frame_grab() noexcept;
PanelFonts menu_fonts() noexcept;
} // namespace dingosdk::capture
