# Flat frame generation

## Status

Flat only, on NVIDIA and AMD, through AMD's signed frame-generation provider. The key is `fix.frame_generation`: `off`, `auto`, or `on`. Default `off`. 2x only.

`on` is temporary. It forces one generated frame per rendered frame for comparison. `auto` is the behaviour to keep: one generated frame only while the monitor period has been read, the rendered frame missed that period, and one real frame's GPU time plus the dispatch fits in two periods with 1 ms of slack.

A VR session logs `frame generation: not started: VR session, flat only` and does not create a context or present a generated frame. XeSS Frame Generation keeps its own keys. While XeSS owns the swap chain this row stands down.

The flat session's default resolve is the internal HDR route. On an 8-bit window that route's HUD-less image is the game's R11G11B10 target. Dispatch interpolates that image: the context is high-dynamic-range, its back-buffer format is the image's format, configure passes the image as HUD-less colour, and the transfer is scRGB at 0 and 80 nits so the provider's conversion leaves the radiance unchanged. The generated frame is written into the 8-bit presenter. An fp16 presenting chain logs `frame generation: flat HDR route has no luminance range yet` and presents only the real frame.

The flat anti-aliasing mode is read with `requestedTemporalMode`. In a flat process `getString` answers off for `fix.temporal_aa`, and the 10:40 Epic flight (`edup_logs\edvr_gfx_20261122_104018`, `v0.23.0-6-g3b9cd978`) logged `frame generation: not started: fix.temporal_aa is off` while DLAA was on.

The bridge is made only while the first swap chain of the process is created, and only when the key is already `on` or `auto`. After that, Present reads the key each frame. A menu change once that chain exists does not create the bridge, and a later chain in the same process is not bridged. The 10:22 Epic flight on the same build installed the Present hook with the key still off and logged no `frame generation:` line. The hook keeps one bridge table. XeSS is asked first and still presents through its proxy swap chain. Flat frame generation is the other row and still dispatches AMD's provider on a null swap chain.

Open: a headset flight has not confirmed the generated frame. A provider that rejects the null-swapchain probe logs `not started` and presents only the real frame.
