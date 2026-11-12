# Frame generation replay capture: the format (shared by the writer and the bench)

Written by the d3d11 side when the existing eye dump fires (hotkey.dump_eyes /
the settings menu's "Dump both eyes as seen") AND `fix.frame_generation_shadow
= on`; read by `tools/framegen_synth_bench`.

Folder: the eye dump's own folder, subfolder `fgreplay_<stamp>\` (same stamp
as the eye dump's files).

Per captured real frame k = 0..5 (6 CONSECUTIVE real frames of the LEFT eye,
the ones the synth processed; frames the synth skipped break the run: stop
and say so in manifest.json):
- `f<k>_out.bin`  : B's upscaled output before the UI (`dlOut`), output size,
  raw texels in dlOut's own DXGI format, row-major, no padding.
- `f<k>_depth.bin`: B's depth as the synth reads it (`dlDepth`, render size),
  raw R32_FLOAT (or its own format), row-major.
- `f<k>_mv.bin`   : B's DLSS vectors (`dlMv`, render size, R16G16_FLOAT raw).
- `f<k>.json`     : everything else the synth's constant block is built from
  for that frame, as numbers: render w/h, output w/h, formats (DXGI enum and
  name), tanNow, tanPrev, the depth knobs (A, B), split metres, the backward
  head delta Hd (3x3 row-major) and th (3), world W (3x3) and tvCam (3),
  worldValid, cameraJump, the pass frame index, QPC time and frequency, and
  the synth's own variant/f for that frame.
- `manifest.json`: stamp, build describe (EDVR version string), eye, frame
  count, file list with byte sizes and SHA-256, and any reason the run
  stopped early.

Writing: stage with CopyResource into staging textures over the frames, map
and write on later frames or a worker thread; never block the render thread
on a Map of the current frame (use the existing eye-dump staging pattern).
A run is about 6 x (98 + 27 + 27) MB at 3756x3264 / 2817x2448; that is
accepted for a one-off dump. Nothing is written without the dump trigger.
