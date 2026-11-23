# Flat frame generation

## Status

Flat only, on NVIDIA and AMD, through AMD's signed frame-generation provider. The key is `fix.frame_generation`: `off`, `auto`, or `on`. Default `off`. 2x only.

`on` is temporary. It forces one generated frame per rendered frame for comparison. `auto` is the behaviour to keep: one generated frame only while the monitor period has been read, the rendered frame missed that period, and one real frame's GPU time plus the dispatch fits in two periods with 1 ms of slack.

A VR session logs `frame generation: not started: VR session, flat only` and does not create a context or present a generated frame. XeSS Frame Generation keeps its own keys. While XeSS owns the swap chain this row stands down.

The flat session's default resolve is the internal HDR route. On an 8-bit window that route's HUD-less image is the game's R11G11B10 target. Dispatch interpolates that image: the context is high-dynamic-range, its back-buffer format is the image's format, configure passes the image as HUD-less colour, and the transfer is scRGB at 0 and 80 nits so the provider's conversion leaves the radiance unchanged. The generated frame is written into the 8-bit presenter. An fp16 presenting chain logs `frame generation: flat HDR route has no luminance range yet` and presents only the real frame.

The flat anti-aliasing mode is read with `requestedTemporalMode`. In a flat process `getString` answers off for `fix.temporal_aa`, and the 10:40 Epic flight (`edup_logs\edvr_gfx_20261122_104018`, `v0.23.0-6-g3b9cd978`) logged `frame generation: not started: fix.temporal_aa is off` while DLAA was on.

The bridge is made only while the first swap chain of the process is created, and only when the key is already `on` or `auto`. After that, Present reads the key each frame. A menu change once that chain exists does not create the bridge, and a later chain in the same process is not bridged. The 10:22 Epic flight on the same build installed the Present hook with the key still off and logged no `frame generation:` line. The hook keeps one bridge table. XeSS is asked first and still presents through its proxy swap chain. Flat frame generation is the other row and still dispatches AMD's provider on a null swap chain.

Ruled out: the black menu highlights are a generated frame. The 11:48 Epic flight (`edup_logs\edvr_gfx_20261122_114845`, `v0.23.0-18-g2515257c`) stayed at `applied 0` through 11:50:42, skipping on `no world camera basis` and `no scene`, while every real frame was copied onto the windowed flip presenter. That presenter is `DXGI_SWAP_EFFECT_FLIP_DISCARD` with `DXGI_ALPHA_MODE_UNSPECIFIED`. Elite's own chain never reached the window. At 11:50:43 `fg bridge: exclusive fullscreen off (0x887A0001)` left it windowed.

Open: those highlights are pixels whose alpha the game leaves at 0, which a windowed flip present can show as black. The log does not record the chain format or a pixel's alpha. A process started with the key off does not create the presenter; the same DLL's other changes stay. If the highlights are gone on that start, the presenter is the cause. A headset flight has still not confirmed a generated frame.
