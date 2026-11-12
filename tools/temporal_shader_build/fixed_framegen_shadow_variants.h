#pragma once
// Build-only registry: the frame-generation shadow scorer (fix.frame_generation_shadow, TEMPORARY; src/d3d11/framegen_shadow_shader.h, docs/design-frame-generation-2026-10-26.md,
// Phase 1): one text, five entry points (capture, clear, splat, score, score B). The text is the temporal pass's carry functions (cut from kTemporalCsHlsl between the two
// EDVR_TEMPORAL_CARRY markers) followed by the scorer's own, assembled once by frameGenShadowShaderSource(). New, so held to its own contract (symbol, source name, entry, profile,
// the text, the 8x8 group and the bindings each entry reflects) in temporal_shader_build's self-test; tools/framegen_shadow_test runs the compiled bytes on WARP.
static std::vector<Variant> frameGenShadowVariants() {
    const char* text = edvr::frameGenShadowShaderSource().c_str();
    return {
        {"kFrameGenShadowCaptureBytecode", "framegen shadow", "fgShadowCapture", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenShadowClearBytecode", "framegen shadow", "fgShadowClear", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenShadowSplatBytecode", "framegen shadow", "fgShadowSplat", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenShadowScoreBytecode", "framegen shadow", "fgShadowScore", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenShadowScoreBBytecode", "framegen shadow", "fgShadowScoreB", nullptr, {}, false, text, "cs_5_0"},
    };
}
