#pragma once
// Build-only registry: the frame-generation synthesizer (fix.frame_generation_shadow, TEMPORARY; src/d3d11/framegen_synth_shader.h, docs/design-frame-generation-2026-10-26.md,
// Phase 2.1): one text, seven entry points (splat, source, resolve, holes, resample, luma, score; the clear before them is the API's). The text is the temporal pass's carry functions
// (cut from kTemporalCsHlsl between the two EDVR_TEMPORAL_CARRY markers) followed by the synthesizer's own, assembled once by frameGenSynthShaderSource(). New, so held to its own
// contract (symbol, source name, entry, profile, the text, the 8x8 group and the bindings each entry reflects) in temporal_shader_build's self-test; tools/framegen_synth_test runs
// the compiled bytes on WARP.
static std::vector<Variant> frameGenSynthVariants() {
    const char* text = edvr::frameGenSynthShaderSource().c_str();
    return {
        {"kFrameGenSynthSplatBytecode", "framegen synth", "fgSynSplat", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthSrcBytecode", "framegen synth", "fgSynSrc", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthResolveBytecode", "framegen synth", "fgSynResolve", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthHolesBytecode", "framegen synth", "fgSynHoles", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthResampleBytecode", "framegen synth", "fgSynResample", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthLumaBytecode", "framegen synth", "fgSynLuma", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthScoreBytecode", "framegen synth", "fgSynScore", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthFinishBytecode", "framegen synth", "fgSynFinish", nullptr, {}, false, text, "cs_5_0"},   // Phase 2.3b: S finished like B (the UI layer by the head's step)
    };
}
