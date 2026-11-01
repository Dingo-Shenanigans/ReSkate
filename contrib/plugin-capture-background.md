# Background capture processing

`reskate.capture.start({background = true, ...})` opts into a bounded CPU worker path behind the existing `capture` permission. The default is false, preserving synchronous capture for existing callers.

Colour and matching depth rectangles are copied into owned memory before the render callback returns. At most three jobs and 64 MiB of pending raw data are retained. A full queue drops a new incoming frame rather than blocking presentation; no duplicate frame is invented. Oversized single jobs fail with an explicit memory-limit error.

`capture.state()` adds `pending` and `dropped`. `taken` counts accepted source frames as they are copied, allowing a plugin to associate pose observations while processing is pending. `kept` counts completed retained pictures. `capture.compare` returns -1 until both requested thumbnails are processed. The normal frame numbering starts at zero and remains ordered.

`capture.stop()` and `capture.save()` drain outstanding work before frame selection or encoding. Restart and discard retire the old generation, so late results cannot enter another recording. Depth frames can store only their nontransparent rectangle with its placement metadata, then reconstruct the original canvas for encoding; cut-out pixels, colour grading and output placement are preserved. This prevents transparent backdrop storage from consuming the frame budget at higher rates.

This is a throughput option, not a guarantee of a particular FPS. Callers must inspect retained counts and dropped frames. APNG/WebM support 60 FPS; Fit Check currently caps GIF at 50 FPS for playback compatibility. Depth cut-outs still require valid game depth; background processing never substitutes a colour cut-out.
