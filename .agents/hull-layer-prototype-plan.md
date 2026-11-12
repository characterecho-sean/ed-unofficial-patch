# Own hull as a separate layer: prototype plan (2026-11-08)

Evidence: `docs/research-own-hull-layer-2026-11-08.md` (Status, "The eye
pass's draw order", "The five checks", "The binding map"). Branch
`claude/hull-research-94395e`. The user approved the direction on
2026-11-08: the layer is worth its cost for DLSS and frame generation.

This supersedes the motion-mask approach in
`.agents/framegen-hull-layer-plan.md` for frame generation. Once a real
hull layer exists, the synthesizer warps two layers and needs no edge
heuristics. Keep that plan's Phase 0 edge census only as a baseline to
measure against.

## Facts the prototype builds on (all measured)

- The own ship is one contiguous block of 37-40 G-buffer draws an eye, of
  the families AACF, DE54, 66DE and 61AE. It comes after the cockpit shell
  (`BFE5` x15) and before the terrain (`6C320453`, `72BDD292`). The block
  holds 54-57 records. 100% of the keyed hull pixels at 10-150 m are
  these records.
- Record indices are reassigned every frame, so the own list must be
  rebuilt every frame from the block.
- Its decals are `BBE5`/`DB3E`, a run after each eye's G-buffer. The
  block's 11-13 records are among them.
- The terrain has a depth-only prepass (`ACE405F4`, null PS) and is drawn
  after the hull. If the block is left out, the terrain fills the hull's
  pixels.
- G-buffer targets: RT0 f23, RT1 f27, RT2 f27, RT3 = the HDR target
  itself (f26, emissive). Depth is f19, GEQUAL.
- After each eye's G-buffer:
  - depth copy `DEF19B03`/`CB95394B` to f39;
  - AO apply `DEF19B03`/`06CD42A9`, which blends a 704x612 compute AO
    into RT2;
  - shadow mask `53211E8C` x2 to f60, stencil 10. It reads the depth
    copy, RT1, RT2 and a 1024x512 lookup.
- Lighting: 4-11 draws an eye.
  - `7E38A6AA` is the full-screen sun and ambient. It reads RT0-2 and the
    shadow mask.
  - The rest are instanced volumes: `8289669D`, `963B52C7`, `24DE25E4`,
    `B121A79E` and `0357BBB2`. They read the depth copy and RT0-2.
    `24DE25E4` and `B121A79E` also read a 1536x1536 shadow map and a
    light-list buffer.
  - The lighting writes RT0 = HDR, additive, and RT1 = an eye-size R16
    that nothing reads in-frame.
- Forward pass, after the lighting:
  - sky `F8FA801F`;
  - atmosphere `41E245D4` (writes depth) and `D95905C1`. Both read the
    depth copy.
  - six own-ship draws: `88DCF116`, `025B4B9F`, `0B5981F2`, `F464E27C`,
    `6DB587D2` and `2B3F53DD`. They have no screen-space inputs.
- Tonemap: `2D78DC3F`/`99C21CEB`, a 3-vertex triangle. It reads t1 = HDR
  and t0 = (most likely) exposure, and writes the 8-bit eye. EDVR's DLSS
  runs after it, on the tonemapped eye.
- Shadow maps are drawn outside the eye pass and stay untouched.

## Step 1: the hull layer in the dump only (no visual change)

Behind a temporary key (list it in the doc's Status; it is removed when
the arc closes).

1. Recognise the block per eye: the hull-family run after the cockpit
   shell. Record its draw indices and records.
2. Re-issue each block draw and its decals immediately into private
   targets:
   - RT0-2 in the game's formats;
   - RT3 = a private HDR target cleared to 0;
   - private depth, cleared like the game's.
   
   Leave the game's own draw as it is.
3. After the game's lighting, re-issue the following against the private
   targets: the depth copy, the shadow mask, and the sun/ambient draw
   `7E38A6AA` only, with a scratch R16 as RT1. Point and spot volumes come
   in step 1b.
4. Write the private HDR (and depth) into the eye dump beside the game's
   frame.
5. Add a log line: the block's length and records, the re-issue count, and
   GPU time from a census scope.

**Verdict:** on hull pixels away from point lights, the private HDR
matches the game's HDR within the AO difference. **Refuted if** the layer
is black, unlit or offset: then the shadow-mask or lighting bindings are
wrong.

## Step 2: leave the block out of the main pass

The main G-buffer gets no block or decals, and the six own forward draws
move to the layer. Read the dump: the terrain behind the wing is drawn,
and DLSS's input has no hull. Measure the frame cost against step 1.

## Step 3: composite and anti-aliasing

- Re-issue the tonemap on the layer's HDR.
- Composite after DLSS by depth against the main depth, so the cockpit
  stays in front.
- Anti-aliasing: TAA on the layer (static in view) or a second DLSS
  feature.
- Atmosphere on the layer: after its own forward draws.

## Frame generation

The synthesizer gets the world layer, with world MVs and depth, and the
hull layer, with head-only MVs. Holes at the hull edge fill from the real
world layer.

## Open

- AO for the hull: re-dispatch the compute chain (`ch=D31E7812`, from a
  176x153 depth) on the layer's depth, or none.
- At a station, the point lights on the hull matter. Before step 1b, the
  layer can be limited to flight.
- Census dumps at a station need `advanced.census_lines = 16384`.
