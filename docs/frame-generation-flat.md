# Flat frame generation

## Status

Flat only, on NVIDIA and AMD, through AMD's signed frame-generation provider. The key is `fix.frame_generation`: `off`, `auto`, or `on`. Default `off`. 2x only.

`on` is temporary. It forces one generated frame per rendered frame for comparison. `auto` is the behaviour to keep: one generated frame only while the monitor period has been read, the rendered frame missed that period, and one real frame's GPU time plus the dispatch fits in two periods with 1 ms of slack.

A VR session logs `frame generation: not started: VR session, flat only` and does not create a context or present a generated frame. XeSS Frame Generation keeps its own keys. While XeSS owns the swap chain this row stands down.

The flat session's default resolve is the internal HDR route. On an 8-bit window that route's HUD-less image is the game's R11G11B10 target, and a generated frame is made from it. An fp16 presenting chain logs `frame generation: flat HDR route has no luminance range yet` and presents only the real frame.

Open: a headset flight has not confirmed the generated frame. A provider that rejects the null-swapchain probe logs `not started` and presents only the real frame.
