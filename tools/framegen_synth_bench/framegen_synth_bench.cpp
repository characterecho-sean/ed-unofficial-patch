// framegen_synth_bench: the frame-generation synthesizer (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1) run OFFLINE on the
// hardware GPU, so it can be timed pass by pass and scored against captured real frames without a flight.
//
//   --bench [--render WxH] [--output WxH] [--frame FOLDER[:k]] [--iters N] [--warmup N] [--adapter N] [--json FILE]
//       Synthetic inputs (a textured scene with a depth step, a cockpit band, the far plane, vectors with a ship screen velocity and a head turn), or with --frame the frame k
//       (default 0) of an fgreplay capture, the workload the game gives the synth (its holes, guard and vectors; its sizes and S format replace --render/--output). The synth's
//       per-frame dispatch sequence for both variants (f = 0.5: clear splat src resolve resample; f = 1: clear score splat src resolve resample luma), every pass timed with D3D11
//       timestamp queries inside one disjoint query. Defaults: render 2817x2448, output 3756x3264, RGBA16F output, 300 iterations after 30 warm-up. Median and p95 ms per eye,
//       and the VRAM the synth allocates.
//   --replay FOLDER [--png-dir DIR] [--no-png] [--adapter N] [--json FILE] [--dry-run]
//       FOLDER is an fgreplay_<stamp> capture (.agents/framegen-replay-format.md). For each consecutive pair (B = k, C = k + 1): the f = 1 S is built from B, kept, and scored against C's
//       output exactly as the DLL scores it (raw, and turned by the residual head rotation, per class; the |id: fields of the line are the no-warp baseline); the f = 0.5 S is built
//       from B too. B, S(f = 0.5) and S(f = 1) are written as PNGs (8-bit, clamped to 0..1, sRGB encoded) into DIR, its missing parents made (default %TEMP%\edvr_bench_out\<FOLDER's
//       name>, else .\bench_out\<that name>, never inside FOLDER: a capture sits in the game's folder); --dry-run writes nothing.
//       Each pair also prints S's sharpness against B's (Sharpness: the mean |Laplacian| of luma per class, S over B) for both S, with the count of render pixels that got a key
//       but no read offer (orphan: fgSynSrc's recompute of a landing disagreeing with fgSynSplat's, the resolve's stats word that makes them holes; 0 so far), and the hole region's error (HoleError: the mean |luma| of the
//       f = 1 S against C over the output pixels whose render pixel was a hole, the fill's, with B against C beside it).
//   --define NAME=V (with --bench or --replay): the shader's `#define NAME 1` compiled as V instead, for a comparison (FG_SHARP 0 bilinear, 2 Lanczos-2, 3 unclamped, 5 the
//       Catmull-Rom as 16 loads, 6 with its corners in 12 fetches).
//   --self-test
//       The format reader and writer round trip, the constant block rebuilt from a frame's JSON against the production fill (memcmp), a synthetic capture written, read back and
//       replayed on WARP (a scene scrolling at the vectors' rate is predicted by the f = 1 S), and a tiny bench on WARP. Needs no real GPU; WARP missing: the GPU parts are skipped.
//   --list-adapters
//
// Shared with production, not copied: the shader TEXT (frameGenSynthShaderSource, compiled with the arguments tools\temporal_shader_build uses: cs_5_0, no macros, flags 0, the entry
// names fgSyn*, so the DXBC is the production DXBC), the constant block and its fill (frameGenSynthFillCb, frameGenSynthResidual), the stats decode and the log line
// (frameGenSynthDecodeStats, formatFrameGenSynthReport), and the dispatch sequence with its binding table (src\d3d11\framegen_synth_pass.h frameGenSynthRunPasses). Nothing is
// mirrored.
//
// f<k>.json keys (names compare case- and punctuation-insensitively; nested objects and the aliases in the readers below are accepted): render_w render_h out_w out_h out_format
// (DXGI enum) depth_format mv_format tan_now[4] (left right bottom top) tan_prev[4] depth_a depth_b split_metres hd[9] th[3] world_w[9] tv_cam[3] world_valid camera_jump frame qpc
// qpc_freq variant f. Required: the sizes, out_format, tan_now, depth_a, depth_b, split_metres, hd, th.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "framegen_synth_shader.h"
#include "framegen_synth_pass.h"
#include "../../src/common/framegen_synth_math.h"

using Microsoft::WRL::ComPtr;
using namespace edvr;

namespace {

// ---- small utilities -------------------------------------------------------------------------------------------------------------------------------------

uint16_t floatToHalf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int e = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    const uint32_t m = u & 0x7FFFFFu;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;
    return static_cast<uint16_t>(sign | h);
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
    uint32_t u;
    if (e == 0) {
        if (m == 0) u = sign;
        else {
            float v = std::ldexp(static_cast<float>(m), -24);
            std::memcpy(&u, &v, 4);
            u |= sign;
        }
    } else if (e == 31) {
        u = sign | 0x7F800000u | (m << 13);
    } else {
        u = sign | ((e + 112u) << 23) | (m << 13);
    }
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

float smallFloat(uint32_t bits, int mantissaBits) {   // an unsigned 5-bit-exponent float (R11G11B10's)
    const uint32_t e = bits >> mantissaBits, m = bits & ((1u << mantissaBits) - 1u);
    if (e == 0) return std::ldexp(static_cast<float>(m), -14 - mantissaBits);
    if (e == 31) return m ? 0.0f : 65504.0f;
    return std::ldexp(1.0f + static_cast<float>(m) / static_cast<float>(1u << mantissaBits), static_cast<int>(e) - 15);
}

void rotY(float deg, float m[9]) {
    const float r = deg * 3.14159265358979f / 180.0f, c = std::cos(r), s = std::sin(r);
    const float v[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    std::memcpy(m, v, sizeof(v));
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(q * static_cast<double>(v.size()));
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const void* data, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = n == 0 || std::fwrite(data, 1, n, f) == n;
    return std::fclose(f) == 0 && ok;
}

bool writeText(const std::string& path, const std::string& s) { return writeFile(path, s.data(), s.size()); }

std::string fmtStr(const char* f, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// ---- a tolerant JSON reader: numbers (and booleans as 0/1) flattened under a normalised key ---------------------------------------------------------------------

struct Json {
    std::map<std::string, std::vector<double>> num;
    std::map<std::string, std::string> str;
};

std::string normKey(const std::string& k) {
    std::string r;
    for (char c : k) {
        if (std::isalnum(static_cast<unsigned char>(c))) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

struct JsonParser {
    const char* p;
    const char* end;
    std::string err;
    Json* out;

    void ws() { while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }
    bool parseString(std::string& s) {
        if (p >= end || *p != '"') return fail("a string was expected");
        ++p;
        s.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) { ++p; s += (*p == 'n' ? '\n' : *p == 't' ? '\t' : *p); }
            else s += *p;
            ++p;
        }
        if (p >= end) return fail("an unterminated string");
        ++p;
        return true;
    }
    // `key` is the normalised flat key this value is stored under ("" at the root).
    bool value(const std::string& key) {
        ws();
        if (p >= end) return fail("a value was expected");
        if (*p == '{') {
            ++p;
            ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!parseString(k)) return false;
                ws();
                if (p >= end || *p != ':') return fail("a ':' was expected");
                ++p;
                if (!value(key + normKey(k))) return false;
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("a ',' or '}' was expected");
            }
        }
        if (*p == '[') {
            ++p;
            ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                if (!value(key)) return false;   // an array's numbers (nested arrays too) append, in order, to the one key
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("a ',' or ']' was expected");
            }
        }
        if (*p == '"') {
            std::string s;
            if (!parseString(s)) return false;
            out->str[key] = s;
            return true;
        }
        if (!std::strncmp(p, "true", 4)) { p += 4; out->num[key].push_back(1.0); return true; }
        if (!std::strncmp(p, "false", 5)) { p += 5; out->num[key].push_back(0.0); return true; }
        if (!std::strncmp(p, "null", 4)) { p += 4; return true; }
        char* e = nullptr;
        const double v = std::strtod(p, &e);
        if (e == p) return fail("a number was expected");
        p = e;
        out->num[key].push_back(v);
        return true;
    }
};

bool parseJson(const std::string& text, Json& out, std::string& err) {
    out = Json{};
    JsonParser jp{text.data(), text.data() + text.size(), "", &out};
    if (!jp.value("")) { err = jp.err; return false; }
    return true;
}

const std::vector<double>* jfind(const Json& j, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        const auto it = j.num.find(n);
        if (it != j.num.end() && !it->second.empty()) return &it->second;
    }
    return nullptr;
}

// ---- the capture ----------------------------------------------------------------------------------------------------------------------------------------

struct FrameMeta {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN, depthFmt = DXGI_FORMAT_R32_FLOAT, mvFmt = DXGI_FORMAT_R16G16_FLOAT;
    float tanNow[4] = {}, tanPrev[4] = {};
    float depthA = 0, depthB = 0, split = 0;
    FrameGenDeltas d;
    bool worldValid = false, cameraJump = false;
    int64_t frame = -1;
    double qpc = 0, qpcFreq = 0;
    int variant = 1;
    float f = 1.0f;
};

DXGI_FORMAT concreteFormat(DXGI_FORMAT f) {   // a typeless or depth format the writer may name -> the one the synth reads it as
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        default: return f;
    }
}

uint32_t bytesPerTexel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_FLOAT: return 4;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: return 2;
        default: return 0;
    }
}

bool metaFromJson(const Json& j, FrameMeta& m, std::string& err) {
    const auto need = [&](const char* what) { if (err.empty()) err = std::string("missing key: ") + what; };
    const auto dim = [&](std::initializer_list<const char*> scalar, std::initializer_list<const char*> pair, int idx, uint32_t& out, const char* what) {
        if (const auto* v = jfind(j, scalar)) { out = static_cast<uint32_t>((*v)[0]); return; }
        if (const auto* v = jfind(j, pair)) if (v->size() > static_cast<size_t>(idx)) { out = static_cast<uint32_t>((*v)[idx]); return; }
        need(what);
    };
    dim({"renderw", "renderwidth", "rendersizew"}, {"render", "rendersize"}, 0, m.rw, "render_w");
    dim({"renderh", "renderheight", "rendersizeh"}, {"render", "rendersize"}, 1, m.rh, "render_h");
    dim({"outw", "outputw", "outwidth", "outputwidth", "outsizew"}, {"out", "output", "outsize", "outputsize"}, 0, m.ow, "out_w");
    dim({"outh", "outputh", "outheight", "outputheight", "outsizeh"}, {"out", "output", "outsize", "outputsize"}, 1, m.oh, "out_h");
    if (const auto* v = jfind(j, {"outformat", "outputformat", "outdxgi", "outputdxgi", "outfmt", "outformatdxgi", "dloutformat"})) m.outFmt = static_cast<DXGI_FORMAT>(static_cast<int>((*v)[0]));
    else need("out_format");
    if (const auto* v = jfind(j, {"depthformat", "depthdxgi", "depthfmt"})) m.depthFmt = static_cast<DXGI_FORMAT>(static_cast<int>((*v)[0]));
    if (const auto* v = jfind(j, {"mvformat", "mvdxgi", "mvfmt", "vectorformat"})) m.mvFmt = static_cast<DXGI_FORMAT>(static_cast<int>((*v)[0]));
    const auto arr = [&](std::initializer_list<const char*> names, size_t n, float* out, const char* what, bool required) {
        const auto* v = jfind(j, names);
        if (!v || v->size() < n) { if (required) need(what); return false; }
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<float>((*v)[i]);
        return true;
    };
    arr({"tannow", "tan", "tangents", "tanb"}, 4, m.tanNow, "tan_now", true);
    arr({"tanprev", "tana"}, 4, m.tanPrev, "tan_prev", false);
    const auto scalar = [&](std::initializer_list<const char*> names, float& out, const char* what, bool required) {
        if (const auto* v = jfind(j, names)) { out = static_cast<float>((*v)[0]); return; }
        if (required) need(what);
    };
    float a2[2] = {};
    if (arr({"depthknobs", "knobs", "depthab"}, 2, a2, "depth_a", false)) { m.depthA = a2[0]; m.depthB = a2[1]; }
    scalar({"deptha", "knobsa", "knobs0"}, m.depthA, "depth_a", !jfind(j, {"depthknobs", "knobs", "depthab"}));
    scalar({"depthb", "knobsb", "knobs2"}, m.depthB, "depth_b", !jfind(j, {"depthknobs", "knobs", "depthab"}));
    scalar({"splitmetres", "split", "splitm", "splitmeters"}, m.split, "split_metres", true);
    arr({"hd", "headdelta", "headhd", "dr", "headdr"}, 9, m.d.Hd, "hd", true);
    arr({"th", "headth", "headtranslation", "tvused"}, 3, m.d.th, "th", true);
    arr({"worldw", "w", "world", "worlddelta"}, 9, m.d.W, "world_w", false);
    arr({"tvcam", "tv"}, 3, m.d.tv, "tv_cam", false);
    float t = 0;
    t = 0; scalar({"worldvalid"}, t, "world_valid", false); m.worldValid = t != 0;
    t = 0; scalar({"camerajump", "jump"}, t, "camera_jump", false); m.cameraJump = t != 0;
    t = -1; scalar({"frame", "passframe", "frameindex"}, t, "frame", false); m.frame = static_cast<int64_t>(t);
    if (const auto* v = jfind(j, {"qpc", "qpctime"})) m.qpc = (*v)[0];
    if (const auto* v = jfind(j, {"qpcfreq", "qpcfrequency", "qpcfreqhz"})) m.qpcFreq = (*v)[0];
    t = 1; scalar({"variant"}, t, "variant", false); m.variant = static_cast<int>(t);
    scalar({"f", "synthf", "synthfraction"}, m.f, "f", false);
    if (!err.empty()) return false;
    if (!m.rw || !m.rh || !m.ow || !m.oh) { err = "a zero size"; return false; }
    m.outFmt = concreteFormat(m.outFmt);
    m.depthFmt = concreteFormat(m.depthFmt);
    m.mvFmt = concreteFormat(m.mvFmt);
    return true;
}

std::string metaToJson(const FrameMeta& m, const char* outFormatName) {
    std::string s = "{\n";
    const auto a = [&](const char* k, const float* v, int n) {
        s += fmtStr("  \"%s\": [", k);
        for (int i = 0; i < n; ++i) s += fmtStr("%s%.9g", i ? ", " : "", static_cast<double>(v[i]));
        s += "],\n";
    };
    s += fmtStr("  \"render_w\": %u, \"render_h\": %u, \"out_w\": %u, \"out_h\": %u,\n", m.rw, m.rh, m.ow, m.oh);
    s += fmtStr("  \"out_format\": %d, \"out_format_name\": \"%s\", \"depth_format\": %d, \"mv_format\": %d,\n", static_cast<int>(m.outFmt), outFormatName, static_cast<int>(m.depthFmt),
                static_cast<int>(m.mvFmt));
    a("tan_now", m.tanNow, 4);
    a("tan_prev", m.tanPrev, 4);
    s += fmtStr("  \"depth_a\": %.9g, \"depth_b\": %.9g, \"split_metres\": %.9g,\n", static_cast<double>(m.depthA), static_cast<double>(m.depthB), static_cast<double>(m.split));
    a("hd", m.d.Hd, 9);
    a("th", m.d.th, 3);
    a("world_w", m.d.W, 9);
    a("tv_cam", m.d.tv, 3);
    s += fmtStr("  \"world_valid\": %d, \"camera_jump\": %d, \"frame\": %lld, \"qpc\": %.0f, \"qpc_freq\": %.0f, \"variant\": %d, \"f\": %.9g\n}\n", m.worldValid ? 1 : 0, m.cameraJump ? 1 : 0,
                static_cast<long long>(m.frame), m.qpc, m.qpcFreq, m.variant, static_cast<double>(m.f));
    return s;
}

// The synth's constant block for a frame, as the DLL fills it. `residual`/`tanKept` null: not a scoring call.
FrameGenSynthCb fillCb(const FrameMeta& m, float f, const float* residual, const float* tanKept) {
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, m.rw, m.rh, m.ow, m.oh, m.tanNow, m.depthA, m.depthB, m.split, f, m.d, residual, tanKept);
    return cb;
}

struct FrameData {
    FrameMeta meta;
    std::vector<uint8_t> out, depth, mv;
};

bool loadFrame(const std::string& dir, int k, FrameData& fd, std::string& err) {
    const std::string base = dir + "\\f" + std::to_string(k);
    std::vector<uint8_t> js;
    if (!readFile(base + ".json", js)) { err = "cannot read " + base + ".json"; return false; }
    Json j;
    std::string perr;
    if (!parseJson(std::string(js.begin(), js.end()), j, perr)) { err = base + ".json: " + perr; return false; }
    if (!metaFromJson(j, fd.meta, perr)) { err = base + ".json: " + perr; return false; }
    if (!readFile(base + "_out.bin", fd.out) || !readFile(base + "_depth.bin", fd.depth) || !readFile(base + "_mv.bin", fd.mv)) { err = "cannot read the .bin files of frame " + std::to_string(k); return false; }
    const auto sizeOk = [&](const std::vector<uint8_t>& v, uint32_t w, uint32_t h, DXGI_FORMAT f, const char* what) {
        const uint32_t bpp = bytesPerTexel(f);
        if (!bpp || v.size() != static_cast<size_t>(w) * h * bpp) {
            err = fmtStr("frame %d: %s is %llu bytes, %ux%u at format %d needs %llu", k, what, static_cast<unsigned long long>(v.size()), w, h, static_cast<int>(f),
                         static_cast<unsigned long long>(static_cast<size_t>(w) * h * bpp));
            return false;
        }
        return true;
    };
    return sizeOk(fd.out, fd.meta.ow, fd.meta.oh, fd.meta.outFmt, "out.bin") && sizeOk(fd.depth, fd.meta.rw, fd.meta.rh, fd.meta.depthFmt, "depth.bin") &&
           sizeOk(fd.mv, fd.meta.rw, fd.meta.rh, fd.meta.mvFmt, "mv.bin");
}

// How many consecutive frames the folder holds: the manifest's count when it has one, else the f<k>.json files present.
int captureFrameCount(const std::string& dir, std::string& note) {
    std::vector<uint8_t> mf;
    int n = 0;
    while (GetFileAttributesA((dir + "\\f" + std::to_string(n) + ".json").c_str()) != INVALID_FILE_ATTRIBUTES) ++n;
    if (readFile(dir + "\\manifest.json", mf)) {
        Json j;
        std::string e;
        if (parseJson(std::string(mf.begin(), mf.end()), j, e)) {
            if (const auto* v = jfind(j, {"framecount", "frames", "count", "nframes"})) if (static_cast<int>((*v)[0]) < n) n = static_cast<int>((*v)[0]);
            for (const char* key : {"stoppedreason", "stopreason", "stoppedearly", "reason"}) {
                const auto it = j.str.find(key);
                if (it != j.str.end() && !it->second.empty()) note = "manifest: " + it->second;
            }
        }
    } else {
        note = "no manifest.json";
    }
    return n;
}

// ---- the GPU harness --------------------------------------------------------------------------------------------------------------------------------------

constexpr int kEntries = kFrameGenSynthEntries;   // the entries, their names and the dispatch sequence: framegen_synth_pass.h
const char* const* const kEntryNames = kFrameGenSynthEntryNames;
constexpr int kSteps = kFrameGenSynthSteps;      // the entries and the clear step, for timing
const char* const* const kPassLabels = kFrameGenSynthStepLabels;

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    uint64_t bytes = 0;
};

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs[kEntries];
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11Buffer> cb;
    std::string adapterName;
    uint64_t dedicatedVram = 0;
    bool warp = false;
    DXGI_FORMAT lumaFormat = DXGI_FORMAT_R16_FLOAT;
    std::vector<std::pair<std::string, std::string>> defines;   // --define

    // adapterIndex < 0: the default hardware adapter. warp: the software one.
    bool init(int adapterIndex, bool useWarp, std::string& err) {
        warp = useWarp;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr;
        if (useWarp) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        } else if (adapterIndex >= 0) {
            ComPtr<IDXGIFactory1> fac;
            ComPtr<IDXGIAdapter1> ad;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || FAILED(fac->EnumAdapters1(static_cast<UINT>(adapterIndex), &ad))) { err = "no adapter " + std::to_string(adapterIndex); return false; }
            hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        } else {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
        }
        if (FAILED(hr)) { err = fmtStr("D3D11CreateDevice failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            adapterName = name;
            dedicatedVram = desc.DedicatedVideoMemory;
        }
        // The production shader text, compiled the way tools\temporal_shader_build compiles it.
        HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
        if (!lib) { err = "d3dcompiler_47.dll did not load"; return false; }
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        const CompileFn compile = reinterpret_cast<CompileFn>(GetProcAddress(lib, "D3DCompile"));
        if (!compile) { err = "D3DCompile missing"; return false; }
        std::string text = frameGenSynthShaderSource();
        for (const auto& d : defines) {   // --define NAME=V: one of the shader's switches set to V, for a comparison (production compiles them all 1)
            const std::string from = "#define " + d.first + " 1";
            const size_t at = text.find(from);
            if (at == std::string::npos) { err = "the shader has no `" + from + "`"; return false; }
            text.replace(at, from.size(), "#define " + d.first + " " + d.second);
        }
        for (int i = 0; i < kEntries; ++i) {
            ComPtr<ID3DBlob> code, e;
            if (FAILED(compile(text.data(), text.size(), "framegen_synth", nullptr, nullptr, kEntryNames[i], "cs_5_0", 0, 0, &code, &e))) {
                err = std::string(kEntryNames[i]) + ": " + (e ? static_cast<const char*>(e->GetBufferPointer()) : "compile failed");
                return false;
            }
            if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs[i]))) { err = std::string("CreateComputeShader ") + kEntryNames[i]; return false; }
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FrameGenSynthCb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) { err = "constant buffer"; return false; }
        if (!frameGenSynthCreateSampler(dev.Get(), &linear)) { err = "sampler"; return false; }
        // framegen_synth.cpp createCommon: the formats the synth stores in, and the luma format.
        const auto uavStoreOk = [&](DXGI_FORMAT fmt) {
            UINT support = 0;
            const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
            return SUCCEEDED(dev->CheckFormatSupport(fmt, &support)) && (support & want) == want;
        };
        if (!uavStoreOk(DXGI_FORMAT_R32G32_FLOAT) || !uavStoreOk(DXGI_FORMAT_R32_UINT) || !uavStoreOk(DXGI_FORMAT_R32_FLOAT)) {
            err = "the adapter does not store the synth's texture formats";
            return false;
        }
        lumaFormat = uavStoreOk(DXGI_FORMAT_R16_FLOAT) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        return true;
    }

    bool uavOk(DXGI_FORMAT fmt) {
        UINT support = 0;
        const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
        return SUCCEEDED(dev->CheckFormatSupport(fmt, &support)) && (support & want) == want;
    }

    // data: tightly packed rows (or null). wantUav: the synth's own writable textures.
    bool makeTex(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const void* data, bool wantUav, Tex& out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (wantUav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        const uint32_t bpp = bytesPerTexel(fmt);
        D3D11_SUBRESOURCE_DATA sd{data, w * bpp, 0};
        if (FAILED(dev->CreateTexture2D(&td, data ? &sd : nullptr, &out.t))) return false;
        if (FAILED(dev->CreateShaderResourceView(out.t.Get(), nullptr, &out.srv))) return false;
        if (wantUav && FAILED(dev->CreateUnorderedAccessView(out.t.Get(), nullptr, &out.uav))) return false;
        out.bytes = static_cast<uint64_t>(w) * h * bpp;
        return true;
    }

    std::vector<uint8_t> readTex(ID3D11Texture2D* src) {
        D3D11_TEXTURE2D_DESC td{};
        src->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        const uint32_t row = td.Width * bytesPerTexel(td.Format);
        std::vector<uint8_t> v(static_cast<size_t>(row) * td.Height);
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return {};
        ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
        for (uint32_t y = 0; y < td.Height; ++y) std::memcpy(&v[static_cast<size_t>(y) * row], static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, row);
        ctx->Unmap(st.Get(), 0);
        return v;
    }

    void setCb(const FrameGenSynthCb& c) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(cb.Get(), 0);
        }
        ID3D11Buffer* b = cb.Get();
        ctx->CSSetConstantBuffers(0, 1, &b);
    }
};

// The synth's resources for one eye size (framegen_synth.cpp createEye + ensureScratch).
struct Synth {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, qw = 0, qh = 0;
    DXGI_FORMAT sFormat = DXGI_FORMAT_UNKNOWN;
    Tex s, sLuma, bInfo, key, read;
    ComPtr<ID3D11Buffer> stats, staging[4], holes;
    ComPtr<ID3D11UnorderedAccessView> statsUav, holesUav;
    FrameGenSynthVram vram;   // measured from what was made, as the module's VRAM line is

    bool create(Gpu& g, uint32_t rw_, uint32_t rh_, uint32_t ow_, uint32_t oh_, DXGI_FORMAT sFmt) {
        rw = rw_; rh = rh_; ow = ow_; oh = oh_; qw = (rw + 3) / 4; qh = (rh + 3) / 4; sFormat = sFmt;
        if (!g.uavOk(sFmt)) return false;
        if (!g.makeTex(ow, oh, sFmt, nullptr, true, s) || !g.makeTex(qw, qh, g.lumaFormat, nullptr, true, sLuma) || !g.makeTex(qw, qh, DXGI_FORMAT_R32G32_FLOAT, nullptr, true, bInfo)) return false;
        if (!g.makeTex(rw, rh, DXGI_FORMAT_R32_UINT, nullptr, true, key) || !g.makeTex(rw, rh, DXGI_FORMAT_R32_UINT, nullptr, true, read)) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kFrameGenSynthStatsUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &stats))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kFrameGenSynthStatsUints;
        if (FAILED(g.dev->CreateUnorderedAccessView(stats.Get(), &ud, &statsUav))) return false;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& st : staging) if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &st))) return false;
        vram.s = s.bytes; vram.luma = sLuma.bytes; vram.cls = bInfo.bytes;
        vram.stats = bd.ByteWidth * 5ull;   // the stats buffer and its four staging slots
        const uint32_t cap = frameGenSynthHoleCapacity(rw, rh);   // the hole list (framegen_synth.cpp ensureScratch)
        bd = D3D11_BUFFER_DESC{};
        bd.ByteWidth = cap * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ud.Buffer.NumElements = cap;
        if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &holes)) || FAILED(g.dev->CreateUnorderedAccessView(holes.Get(), &ud, &holesUav))) return false;
        vram.key = key.bytes; vram.read = read.bytes; vram.holes = bd.ByteWidth;
        return true;
    }

    bool readStats(Gpu& g, uint32_t out[kFrameGenSynthStatsUints]) {
        g.ctx->CopyResource(staging[0].Get(), stats.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g.ctx->Map(staging[0].Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        std::memcpy(out, m.pData, kFrameGenSynthStatsUints * 4u);
        g.ctx->Unmap(staging[0].Get(), 0);
        return true;
    }
};

struct Inputs {
    ID3D11ShaderResourceView* out = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* mv = nullptr;
};

// The synth's per-frame sequence (framegen_synth_pass.h, the module's own table): clear, [score], splat, src, resolve, resample, [luma].
// scoreOnly: clear and score alone (a scored frame's own build is not needed for its score). `after(entry)` is called after each dispatch (the bench's timestamps).
void runCall(Gpu& g, Synth& sy, const Inputs& in, bool score, bool keep, bool scoreOnly, const std::function<void(int)>& after) {
    FrameGenSynthPassViews v;
    v.out = in.out; v.depth = in.depth; v.mv = in.mv;
    v.keySrv = sy.key.srv.Get(); v.keyUav = sy.key.uav.Get();
    v.readSrv = sy.read.srv.Get(); v.readUav = sy.read.uav.Get();
    v.holesUav = sy.holesUav.Get();
    v.sSrv = sy.s.srv.Get(); v.sUav = sy.s.uav.Get();
    v.sLumaSrv = sy.sLuma.srv.Get(); v.sLumaUav = sy.sLuma.uav.Get();
    v.bInfoSrv = sy.bInfo.srv.Get(); v.bInfoUav = sy.bInfo.uav.Get();
    v.statsUav = sy.statsUav.Get();
    v.linear = g.linear.Get();
    v.rw = sy.rw; v.rh = sy.rh; v.ow = sy.ow; v.oh = sy.oh; v.qw = sy.qw; v.qh = sy.qh;
    ID3D11ComputeShader* cs[kEntries];
    for (int i = 0; i < kEntries; ++i) cs[i] = g.cs[i].Get();
    frameGenSynthRunPasses(g.ctx.Get(), cs, v, score, !scoreOnly, keep, [&](int e) { if (after) after(e); });
}

// ---- pictures -----------------------------------------------------------------------------------------------------------------------------------------------

// One texel of a supported output format as linear RGB.
bool texelRgb(DXGI_FORMAT f, const uint8_t* t, float rgb[3]) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: for (int i = 0; i < 3; ++i) rgb[i] = halfToFloat(reinterpret_cast<const uint16_t*>(t)[i]); return true;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: for (int i = 0; i < 3; ++i) rgb[i] = reinterpret_cast<const float*>(t)[i]; return true;
        case DXGI_FORMAT_R11G11B10_FLOAT: {
            uint32_t u;
            std::memcpy(&u, t, 4);
            rgb[0] = smallFloat(u & 0x7FFu, 6); rgb[1] = smallFloat((u >> 11) & 0x7FFu, 6); rgb[2] = smallFloat((u >> 22) & 0x3FFu, 5);
            return true;
        }
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            uint32_t u;
            std::memcpy(&u, t, 4);
            rgb[0] = static_cast<float>(u & 1023u) / 1023.0f; rgb[1] = static_cast<float>((u >> 10) & 1023u) / 1023.0f; rgb[2] = static_cast<float>((u >> 20) & 1023u) / 1023.0f;
            return true;
        }
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: for (int i = 0; i < 3; ++i) rgb[i] = t[i] / 255.0f; return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM: rgb[0] = t[2] / 255.0f; rgb[1] = t[1] / 255.0f; rgb[2] = t[0] / 255.0f; return true;
        default: return false;
    }
}

bool writePng(const std::string& path, uint32_t w, uint32_t h, DXGI_FORMAT f, const std::vector<uint8_t>& raw, std::string& err) {
    const uint32_t bpp = bytesPerTexel(f);
    float probe[3];
    if (!bpp || !texelRgb(f, raw.data(), probe) || raw.size() < static_cast<size_t>(w) * h * bpp) { err = "format " + std::to_string(static_cast<int>(f)) + " is not one the PNG writer decodes"; return false; }
    std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
    const bool srgbIn = f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        float c[3];
        texelRgb(f, &raw[i * bpp], c);
        for (int k = 0; k < 3; ++k) {
            float v = c[k];
            if (!(v > 0.0f)) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            if (!srgbIn) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            bgr[i * 3 + (2 - k)] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> fac;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        std::wstring wpath(path.begin(), path.end());
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) && SUCCEEDED(fac->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) && SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
             SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props.Get())) &&
             SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&pf)) && SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        if (!ok) err = "WIC could not write " + path;
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// ---- the replay ---------------------------------------------------------------------------------------------------------------------------------------------

struct GpuFrame {
    Tex out, depth, mv;
    Inputs in() const { return Inputs{out.srv.Get(), depth.srv.Get(), mv.srv.Get()}; }
};

bool uploadFrame(Gpu& g, const FrameData& fd, GpuFrame& gf) {
    return g.makeTex(fd.meta.ow, fd.meta.oh, fd.meta.outFmt, fd.out.data(), false, gf.out) && g.makeTex(fd.meta.rw, fd.meta.rh, fd.meta.depthFmt, fd.depth.data(), false, gf.depth) &&
           g.makeTex(fd.meta.rw, fd.meta.rh, fd.meta.mvFmt, fd.mv.data(), false, gf.mv);
}

// S's sharpness against B's, the eye dumps' measure: the mean |Laplacian| of luma (4-neighbour, output pixels, the interior), summed per class of the B pixel each S pixel reads
// (the f = 1 call's B info: 0 far, 1 cockpit, 2 world) and over all; the ratio S/B is the share of B's high-frequency energy S kept (1.0: as sharp; a bilinear resample of an 8-bit
// source reads about 0.8). The far class is the stars'. `holes` (2026-11-07): the pixels whose render pixel was a hole of that S (the fill's), also counted in their class and
// in all: a torn fill's false edges read as sharpness there, so the fill is judged by the hole region's error and the resample by the rest.
struct Sharpness {
    double s[5] = {}, b[5] = {};   // far, cockpit, world, all, holes
    uint64_t orphan = 0;           // render pixels with a key but no read offer, made holes (the stats word kFrameGenSynthStatsOrphan): fgSynSrc's recompute of the splat's landing disagreeing with it; 0 on every replay of 2026-11-08
    void add(const Sharpness& o) { for (int i = 0; i < 5; ++i) { s[i] += o.s[i]; b[i] += o.b[i]; } orphan += o.orphan; }
    double ratio(int i) const { return b[i] > 0.0 ? s[i] / b[i] : -1.0; }
    double restRatio() const { return b[3] - b[4] > 0.0 ? (s[3] - s[4]) / (b[3] - b[4]) : -1.0; }   // all but the holes
    std::string text() const {
        static const char* const kNames[5] = {"far", "cockpit", "world", "all", "holes"};
        std::string t;
        for (int i = 0; i < 5; ++i) t += fmtStr("%s%s %s", i ? " " : "", kNames[i], ratio(i) < 0.0 ? "-" : fmtStr("%.3f", ratio(i)).c_str());
        t += fmtStr(" orphan %llu", static_cast<unsigned long long>(orphan));
        return t;
    }
};

bool lumaPlane(const std::vector<uint8_t>& raw, uint32_t w, uint32_t h, DXGI_FORMAT f, std::vector<float>& out) {
    const uint32_t bpp = bytesPerTexel(f);
    if (!bpp || raw.size() < static_cast<size_t>(w) * h * bpp) return false;
    out.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.size(); ++i) {
        float c[3];
        if (!texelRgb(f, &raw[i * bpp], c)) return false;
        float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        out[i] = l < 0.0f ? 0.0f : (l > 1.0f ? 1.0f : l);
    }
    return true;
}

// A packed read offset (the shader's synUnpackOffset), in render pixels; false: kNoRead (the pixel reads itself).
bool unpackReadOffset(uint32_t p, float& ox, float& oy) {
    if ((p & 0x7C000000u) == 0x7C000000u) {   // a guarded read: an integer pair
        ox = static_cast<float>(static_cast<int>(p & 0xFFFu) - 2048);
        oy = static_cast<float>(static_cast<int>((p >> 12) & 0xFFFu) - 2048);
        return true;
    }
    ox = halfToFloat(static_cast<uint16_t>(p & 0xFFFFu));
    oy = halfToFloat(static_cast<uint16_t>(p >> 16));
    return !(ox < kFrameGenSynthNoRead * 0.5f);
}

// info: the synth's B info texture read back (R32G32_FLOAT, quarter grid; y the class); reads: that S's read offsets (render size, R32_UINT). Each S pixel is classed by the
// B pixel it reads (so content a motion carried over the sky is not counted as the sky's).
bool measureSharpness(const std::vector<uint8_t>& sRaw, const std::vector<uint8_t>& bRaw, const std::vector<uint8_t>& info, const std::vector<uint8_t>& reads, const Synth& sy,
                      Sharpness& out, const std::vector<uint8_t>* keys = nullptr) {
    std::vector<float> ls, lb;
    if (!lumaPlane(sRaw, sy.ow, sy.oh, sy.sFormat, ls) || !lumaPlane(bRaw, sy.ow, sy.oh, sy.sFormat, lb) || info.size() < static_cast<size_t>(sy.qw) * sy.qh * 8 ||
        reads.size() < static_cast<size_t>(sy.rw) * sy.rh * 4) return false;
    const float* cls = reinterpret_cast<const float*>(info.data());
    const uint32_t* rd = reinterpret_cast<const uint32_t*>(reads.data());
    const uint32_t* key = keys && keys->size() >= static_cast<size_t>(sy.rw) * sy.rh * 4 ? reinterpret_cast<const uint32_t*>(keys->data()) : nullptr;
    const auto lap = [&](const std::vector<float>& l, size_t i) { return std::fabs(4.0f * l[i] - l[i - 1] - l[i + 1] - l[i - sy.ow] - l[i + sy.ow]); };
    for (uint32_t y = 1; y + 1 < sy.oh; ++y) {
        const double ry = (y + 0.5) * sy.rh / sy.oh - 0.5;
        const uint32_t py = std::min(static_cast<uint32_t>(ry + 0.5), sy.rh - 1);
        for (uint32_t x = 1; x + 1 < sy.ow; ++x) {
            const double rx = (x + 0.5) * sy.rw / sy.ow - 0.5;
            const uint32_t px = std::min(static_cast<uint32_t>(rx + 0.5), sy.rw - 1);
            float ox = 0.0f, oy = 0.0f;
            if (!unpackReadOffset(rd[static_cast<size_t>(py) * sy.rw + px], ox, oy)) ox = oy = 0.0f;
            const double sx = std::min(std::max(rx + ox + 0.5, 0.0), sy.rw - 1.0), sy2 = std::min(std::max(ry + oy + 0.5, 0.0), sy.rh - 1.0);
            const uint32_t cx = std::min(static_cast<uint32_t>(sx) / 4u, sy.qw - 1), cy = std::min(static_cast<uint32_t>(sy2) / 4u, sy.qh - 1);
            const int c = static_cast<int>(cls[(static_cast<size_t>(cy) * sy.qw + cx) * 2 + 1] + 0.5f);
            const size_t i = static_cast<size_t>(y) * sy.ow + x;
            const double a = lap(ls, i), b = lap(lb, i);
            if (c >= 0 && c < 3) { out.s[c] += a; out.b[c] += b; }
            out.s[3] += a; out.b[3] += b;
            if (key && key[static_cast<size_t>(py) * sy.rw + px] == 0u) { out.s[4] += a; out.b[4] += b; }
        }
    }
    return true;
}

// The hole region's error (2026-11-07, the disocclusion fill): over the output pixels whose render pixel (the resample's nearest) was a hole of the f = 1 S (its key 0: no B pixel
// landed there, so its read is the fill's), the mean |luma| of S against the next real frame C, and of B against C (no warp) beside it.
struct HoleError {
    double sumS = 0.0, sumB = 0.0;
    uint64_t pixels = 0;
    void add(const HoleError& o) { sumS += o.sumS; sumB += o.sumB; pixels += o.pixels; }
    double meanS() const { return pixels ? sumS / static_cast<double>(pixels) : 0.0; }
    double meanB() const { return pixels ? sumB / static_cast<double>(pixels) : 0.0; }
    std::string text() const { return fmtStr("%llu px, |S-C| %.2f |B-C| %.2f milli-luma", static_cast<unsigned long long>(pixels), 1000.0 * meanS(), 1000.0 * meanB()); }
};

bool measureHoles(const std::vector<uint8_t>& sRaw, const std::vector<uint8_t>& bRaw, const std::vector<uint8_t>& cRaw, const std::vector<uint8_t>& keys, const Synth& sy, HoleError& out) {
    std::vector<float> ls, lb, lc;
    if (!lumaPlane(sRaw, sy.ow, sy.oh, sy.sFormat, ls) || !lumaPlane(bRaw, sy.ow, sy.oh, sy.sFormat, lb) || !lumaPlane(cRaw, sy.ow, sy.oh, sy.sFormat, lc) ||
        keys.size() < static_cast<size_t>(sy.rw) * sy.rh * 4) return false;
    const uint32_t* key = reinterpret_cast<const uint32_t*>(keys.data());
    for (uint32_t y = 0; y < sy.oh; ++y) {
        const uint32_t py = std::min(static_cast<uint32_t>(std::max((y + 0.5) * sy.rh / sy.oh - 0.5, 0.0) + 0.5), sy.rh - 1);
        for (uint32_t x = 0; x < sy.ow; ++x) {
            const uint32_t px = std::min(static_cast<uint32_t>(std::max((x + 0.5) * sy.rw / sy.ow - 0.5, 0.0) + 0.5), sy.rw - 1);
            if (key[static_cast<size_t>(py) * sy.rw + px] != 0u) continue;
            const size_t i = static_cast<size_t>(y) * sy.ow + x;
            out.sumS += std::fabs(ls[i] - lc[i]);
            out.sumB += std::fabs(lb[i] - lc[i]);
            ++out.pixels;
        }
    }
    return true;
}

struct PairResult {
    int b = 0, c = 0;
    Sharpness sharp05, sharp1;   // the f = 0.5 S (the one shown) and the f = 1 S against B
    HoleError holes;             // the f = 1 S's hole region against C
    bool scored = false;
    std::string note;
    float rotDeg = 0.0f;
    FrameGenSynthWindow w;
    std::string line;
};

// A directory and every missing parent (CreateDirectoryA makes only the last). True when it exists afterwards.
bool makeDirs(const std::string& dir) {
    for (size_t i = 1; i < dir.size(); ++i) {
        if ((dir[i] != '\\' && dir[i] != '/') || dir[i - 1] == ':' || dir[i - 1] == '\\' || dir[i - 1] == '/') continue;   // a drive's root, or a UNC prefix's slashes
        CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
    }
    CreateDirectoryA(dir.c_str(), nullptr);
    const DWORD a = GetFileAttributesA(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// A path made absolute (against the current directory), its slashes backslashes, no trailing one, lower case: for comparing two paths.
std::string pathKey(const std::string& p) {
    char full[MAX_PATH * 4] = {};
    const DWORD n = GetFullPathNameA(p.c_str(), sizeof(full), full, nullptr);
    std::string s = n && n < sizeof(full) ? std::string(full, n) : p;
    for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    return s;
}

bool insideDir(const std::string& path, const std::string& dir) {
    const std::string p = pathKey(path), d = pathKey(dir);
    return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && (d.back() == '\\' || p[d.size()] == '\\'));
}

// The default PNG folder of a replay: %TEMP%\edvr_bench_out\<the capture's folder name>, else .\bench_out\<that name>; never inside the capture (it sits in the game's folder),
// so "" when both would be (the caller then asks for --png-dir).
std::string defaultPngDir(const std::string& capture) {
    std::string leaf = pathKey(capture);
    leaf = leaf.substr(leaf.find_last_of('\\') + 1);
    if (leaf.empty() || leaf.back() == ':') leaf = "capture";
    char tmp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathA(sizeof(tmp), tmp);
    std::vector<std::string> bases;
    if (n && n < sizeof(tmp)) bases.push_back(std::string(tmp, n) + "edvr_bench_out");
    bases.push_back("bench_out");
    for (const std::string& b : bases) {
        const std::string d = pathKey(b + "\\" + leaf);
        if (!insideDir(d, capture)) return d;
    }
    return "";
}

struct ReplayOptions {
    std::string pngDir;
    bool png = true;
    bool dry = false;
};

struct ReplaySummary {
    std::vector<PairResult> pairs;
    FrameGenSynthWindow agg;
    Sharpness sharp05, sharp1;   // summed over the pairs
    HoleError holes;             // summed over the scored pairs
    std::string aggLine;
    int frames = 0;
};

bool replayFolder(Gpu& g, const std::string& dir, const ReplayOptions& opt, ReplaySummary& sum, std::string& err) {
    std::string note;
    const int n = captureFrameCount(dir, note);
    sum.frames = n;
    if (n < 2) { err = fmtStr("%s holds %d frame(s); a replay needs at least 2 consecutive ones", dir.c_str(), n); return false; }
    std::printf("replay %s: %d frames%s%s\n", dir.c_str(), n, note.empty() ? "" : "  (", note.empty() ? "" : (note + ")").c_str());
    if (opt.png && !opt.dry && !makeDirs(opt.pngDir)) { err = fmtStr("the PNG folder %s could not be created", opt.pngDir.c_str()); return false; }

    FrameData cur;
    if (!loadFrame(dir, 0, cur, err)) return false;
    std::vector<float> rotRes;
    FrameGenSynthWindow& agg = sum.agg;
    uint64_t pixels = 0;
    Synth sy;
    for (int k = 0; k + 1 < n; ++k) {
        FrameData next;
        if (!loadFrame(dir, k + 1, next, err)) return false;
        const FrameMeta& bm = cur.meta;
        const FrameMeta& cm = next.meta;
        PairResult pr;
        pr.b = k; pr.c = k + 1;
        if (bm.rw != cm.rw || bm.rh != cm.rh || bm.ow != cm.ow || bm.oh != cm.oh || bm.outFmt != cm.outFmt) { err = fmtStr("frames %d and %d differ in size or format", k, k + 1); return false; }
        if (sy.rw != bm.rw || sy.rh != bm.rh || sy.ow != bm.ow || sy.oh != bm.oh || sy.sFormat != bm.outFmt) {
            sy = Synth{};
            if (!sy.create(g, bm.rw, bm.rh, bm.ow, bm.oh, bm.outFmt)) { err = "the synth's textures could not be created for this size/format"; return false; }
        }
        GpuFrame gb, gc;
        if (!uploadFrame(g, cur, gb) || !uploadFrame(g, next, gc)) { err = "an input texture could not be created"; return false; }

        const std::string stem = opt.pngDir + "\\f" + std::to_string(k);
        const auto png = [&](const std::string& name, ID3D11Texture2D* t, DXGI_FORMAT fmt) {
            if (!opt.png || opt.dry) return;
            std::string e;
            if (!writePng(name, bm.ow, bm.oh, fmt, g.readTex(t), e)) std::printf("  png: %s\n", e.c_str());
        };
        if (k == 0) png(stem + "_B.png", gb.out.t.Get(), bm.outFmt);
        png(opt.pngDir + "\\f" + std::to_string(k + 1) + "_B.png", gc.out.t.Get(), cm.outFmt);

        // f = 0.5 from B: the picture the later phases would show.
        g.setCb(fillCb(bm, 0.5f, nullptr, nullptr));
        runCall(g, sy, gb.in(), false, false, false, nullptr);
        uint32_t st05[kFrameGenSynthStatsUints] = {};
        sy.readStats(g, st05);
        png(stem + "_S05.png", sy.s.t.Get(), bm.outFmt);
        const std::vector<uint8_t> s05 = g.readTex(sy.s.t.Get()), r05 = g.readTex(sy.read.t.Get()), key05 = g.readTex(sy.key.t.Get());

        // f = 1 from B, kept for C.
        const FrameGenSynthCb cbB = fillCb(bm, 1.0f, nullptr, nullptr);
        g.setCb(cbB);
        runCall(g, sy, gb.in(), false, true, false, nullptr);
        uint32_t stB[kFrameGenSynthStatsUints] = {};
        sy.readStats(g, stB);
        png(stem + "_S1.png", sy.s.t.Get(), bm.outFmt);
        std::vector<uint8_t> s1Kept, key1;
        {
            const std::vector<uint8_t> s1 = g.readTex(sy.s.t.Get()), r1 = g.readTex(sy.read.t.Get()), info = g.readTex(sy.bInfo.t.Get());
            s1Kept = s1;
            key1 = g.readTex(sy.key.t.Get());
            pr.sharp05.orphan = st05[kFrameGenSynthStatsOrphan];
            pr.sharp1.orphan = stB[kFrameGenSynthStatsOrphan];
            if (measureSharpness(s05, cur.out, info, r05, sy, pr.sharp05, &key05) && measureSharpness(s1, cur.out, info, r1, sy, pr.sharp1, &key1)) {
                sum.sharp05.add(pr.sharp05);
                sum.sharp1.add(pr.sharp1);
            }
        }

        const bool jump = bm.cameraJump || cm.cameraJump;
        const bool consecutive = bm.frame < 0 || cm.frame < 0 || bm.frame + 1 == cm.frame;
        if (jump || !consecutive) {
            pr.note = jump ? "not scored: a camera jump" : "not scored: the frames are not consecutive";
            pr.line = pr.note;
        } else {
            float keptRot[9], residual[9];
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) keptRot[r * 3 + c] = cbB.rs[r][c];
            frameGenSynthResidual(keptRot, cm.d.Hd, residual);
            pr.rotDeg = temporalRotationAngleDeg(residual);
            g.setCb(fillCb(cm, 1.0f, residual, bm.tanNow));
            runCall(g, sy, gc.in(), true, false, true, nullptr);
            uint32_t stC[kFrameGenSynthStatsUints] = {};
            sy.readStats(g, stC);
            FrameGenSynthEyeResult rb, rc;
            frameGenSynthDecodeStats(stB, rb);
            frameGenSynthDecodeStats(stC, rc);
            const uint64_t px = static_cast<uint64_t>(bm.rw) * bm.rh;
            frameGenSynthMergeFrame(pr.w, 1, px, rb);
            frameGenSynthMergeScore(pr.w, rc);
            pr.w.rotSamples = 1; pr.w.rotMedian = pr.w.rotMax = pr.rotDeg;
            pr.scored = true;
            pr.line = formatFrameGenSynthReport(pr.w);
            if (measureHoles(s1Kept, cur.out, next.out, key1, sy, pr.holes)) sum.holes.add(pr.holes);
            // the aggregate
            frameGenSynthMergeFrame(agg, 1, px, rb);
            frameGenSynthMergeScore(agg, rc);
            rotRes.push_back(pr.rotDeg);
            pixels += px;
        }
        std::printf("pair B=%d C=%d: %s\n", k, k + 1, pr.line.c_str());
        std::printf("  sharpness S/B: f=0.5 %s | f=1 %s\n", pr.sharp05.text().c_str(), pr.sharp1.text().c_str());
        if (pr.scored) std::printf("  hole region (f=1 S's holes): %s\n", pr.holes.text().c_str());
        sum.pairs.push_back(pr);
        cur = std::move(next);
    }
    agg.rotSamples = static_cast<uint32_t>(rotRes.size());
    agg.rotMax = 0.0f;
    for (float v : rotRes) if (v > agg.rotMax) agg.rotMax = v;
    std::vector<float> tmp = rotRes;
    agg.rotMedian = frameGenMedian(tmp.data(), tmp.size());
    sum.aggLine = formatFrameGenSynthReport(agg);
    std::printf("aggregate (%u scored pairs, the |id: fields are the no-warp baseline): %s\n", agg.scored, sum.aggLine.c_str());
    std::printf("aggregate sharpness S/B (mean |Laplacian| of luma, all pairs): f=0.5 %s | f=1 %s\n", sum.sharp05.text().c_str(), sum.sharp1.text().c_str());
    std::printf("aggregate sharpness S/B outside the holes: f=0.5 %.3f | f=1 %.3f\n", sum.sharp05.restRatio(), sum.sharp1.restRatio());
    std::printf("aggregate hole region (the f=1 S's holes against C, scored pairs): %s\n", sum.holes.text().c_str());
    (void)pixels;
    return true;
}

// ---- the bench ----------------------------------------------------------------------------------------------------------------------------------------------

struct Scene {
    std::vector<uint16_t> out;   // RGBA16F, output size
    std::vector<float> depth;    // R32_FLOAT, render size
    std::vector<uint16_t> mv;    // R16G16_FLOAT, render size
    FrameMeta meta;
};

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

// A textured scene: sky (the far plane) at the top, a world plane at 30 m with a near box (8 m) in front of a distant wall (400 m), a cockpit band (1.2 m) at the bottom; the
// vectors carry a ship screen velocity (world and far) plus the screen flow of a 0.4 degree head turn (all of them).
void buildScene(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, Scene& sc) {
    FrameMeta& m = sc.meta;
    m = FrameMeta{};
    m.rw = rw; m.rh = rh; m.ow = ow; m.oh = oh;
    m.outFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const float tan[4] = {-1.20f, 1.05f, -1.10f, 1.15f};
    std::memcpy(m.tanNow, tan, sizeof(tan));
    std::memcpy(m.tanPrev, tan, sizeof(tan));
    m.depthA = 0.0f; m.depthB = 0.025f; m.split = 10.0f;
    rotY(0.4f, m.d.Hd);
    m.d.th[0] = 0.0015f; m.d.th[1] = -0.0004f; m.d.th[2] = 0.0008f;
    m.frame = 1;
    sc.out.resize(static_cast<size_t>(ow) * oh * 4);
    sc.depth.resize(static_cast<size_t>(rw) * rh);
    sc.mv.resize(static_cast<size_t>(rw) * rh * 2);
    const float shipVx = -7.5f, shipVy = 2.5f;
    for (uint32_t y = 0; y < rh; ++y) {
        for (uint32_t x = 0; x < rw; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(rw), fy = static_cast<float>(y) / static_cast<float>(rh);
            float z;   // metres, <= 0: the far plane
            if (fy < 0.12f) z = 0.0f;
            else if (fy > 0.80f) z = 1.2f;
            else if (fx > 0.55f && fx < 0.75f && fy > 0.30f && fy < 0.60f) z = 8.0f;
            else if (fx >= 0.75f) z = 400.0f;
            else z = 30.0f;
            const size_t i = static_cast<size_t>(y) * rw + x;
            sc.depth[i] = z > 0.0f ? m.depthB / z : 0.0f;
            float d[3], dp[3], pp[2] = {static_cast<float>(x), static_cast<float>(y)};
            temporalPixelToDir(static_cast<float>(x), static_cast<float>(y), m.tanNow, rw, rh, d);
            temporalApply3(m.d.Hd, d, dp);
            temporalDirToPixel(dp, m.tanNow, rw, rh, &pp[0], &pp[1]);
            float vx = pp[0] - static_cast<float>(x), vy = pp[1] - static_cast<float>(y);
            if (z <= 0.0f || z > m.split) { vx += shipVx; vy += shipVy; }
            sc.mv[i * 2] = floatToHalf(vx);
            sc.mv[i * 2 + 1] = floatToHalf(vy);
        }
    }
    const float sx = static_cast<float>(rw) / static_cast<float>(ow), sy = static_cast<float>(rh) / static_cast<float>(oh);
    for (uint32_t y = 0; y < oh; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(oh);
        for (uint32_t x = 0; x < ow; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(ow);
            const float base = 0.45f + 0.25f * std::sin(static_cast<float>(x) * 0.021f) * std::cos(static_cast<float>(y) * 0.017f);
            const float noise = (static_cast<float>(hash32(x * 73856093u ^ y * 19349663u) & 255u) / 255.0f - 0.5f) * 0.08f;
            float tint[3] = {1.0f, 1.0f, 1.0f};
            if (fy < 0.12f) { tint[0] = 0.4f; tint[1] = 0.5f; tint[2] = 0.9f; }
            else if (fy > 0.80f) { tint[0] = 0.7f; tint[1] = 0.65f; tint[2] = 0.6f; }
            else if (fx > 0.55f && fx < 0.75f && fy > 0.30f && fy < 0.60f) { tint[0] = 1.1f; tint[1] = 0.8f; tint[2] = 0.7f; }
            uint16_t* o = &sc.out[(static_cast<size_t>(y) * ow + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = floatToHalf((base + noise) * tint[k]);
            o[3] = floatToHalf(1.0f);
        }
    }
    (void)sx; (void)sy;
}

struct PassStats {
    std::vector<double> ms[kSteps];
    std::vector<double> total;
    int passes[kSteps] = {};
};

struct VariantResult {
    std::string name;
    std::vector<int> order;   // the entries run, in order
    PassStats st;
};

struct QuerySlot {
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> ts[kSteps + 1];
    bool used = false;
    int n = 0;
    int entries[kSteps] = {};
    bool measured = false;
};

bool collectSlot(Gpu& g, QuerySlot& q, PassStats& st) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (g.ctx->GetData(q.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) Sleep(0);
    q.used = false;
    if (dj.Disjoint || !q.measured) return !dj.Disjoint;
    UINT64 t[kSteps + 1] = {};
    for (int i = 0; i <= q.n; ++i)
        while (g.ctx->GetData(q.ts[i].Get(), &t[i], sizeof(UINT64), 0) == S_FALSE) Sleep(0);
    const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
    for (int i = 0; i < q.n; ++i) st.ms[q.entries[i]].push_back(static_cast<double>(t[i + 1] - t[i]) * toMs);
    st.total.push_back(static_cast<double>(t[q.n] - t[0]) * toMs);
    return true;
}

bool benchVariant(Gpu& g, Synth& sy, const Inputs& in, const FrameGenSynthCb& cb, bool score, bool keep, int warm, int iters, VariantResult& vr) {
    QuerySlot slots[8];
    D3D11_QUERY_DESC dd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, td{D3D11_QUERY_TIMESTAMP, 0};
    for (auto& s : slots) {
        if (FAILED(g.dev->CreateQuery(&dd, &s.disjoint))) return false;
        for (auto& t : s.ts) if (FAILED(g.dev->CreateQuery(&td, &t))) return false;
    }
    for (int i = 0; i < warm + iters; ++i) {
        QuerySlot& q = slots[i % 8];
        if (q.used) collectSlot(g, q, vr.st);
        q.n = 0;
        q.measured = i >= warm;
        g.setCb(cb);
        g.ctx->Begin(q.disjoint.Get());
        g.ctx->End(q.ts[0].Get());
        runCall(g, sy, in, score, keep, false, [&](int e) {
            q.entries[q.n] = e;
            g.ctx->End(q.ts[++q.n].Get());
        });
        g.ctx->End(q.disjoint.Get());
        q.used = true;
        g.ctx->Flush();
    }
    for (auto& q : slots) if (q.used) collectSlot(g, q, vr.st);
    return !vr.st.total.empty();
}

struct BenchOptions {
    uint32_t rw = 2817, rh = 2448, ow = 3756, oh = 3264;
    int iters = 300, warm = 30;
    std::string json;
    std::string frameDir;   // --frame: a capture folder whose frame `frameIndex` is the bench's input instead of the synthetic scene (its sizes replace --render/--output)
    int frameIndex = 0;
};

struct BenchOutput {
    std::string adapter;
    std::vector<VariantResult> variants;
    FrameGenSynthVram vram;
    std::string sanity;   // the scene's own fill/guard/clamp shares, so a bench scene that exercises nothing shows
    bool ok = false;
};

bool runBench(Gpu& g, BenchOptions& o, BenchOutput& bo, std::string& err) {
    GpuFrame gf;
    FrameMeta meta;
    if (o.frameDir.empty()) {
        Scene sc;
        buildScene(o.rw, o.rh, o.ow, o.oh, sc);
        if (!g.makeTex(o.ow, o.oh, DXGI_FORMAT_R16G16B16A16_FLOAT, sc.out.data(), false, gf.out) || !g.makeTex(o.rw, o.rh, DXGI_FORMAT_R32_FLOAT, sc.depth.data(), false, gf.depth) ||
            !g.makeTex(o.rw, o.rh, DXGI_FORMAT_R16G16_FLOAT, sc.mv.data(), false, gf.mv)) { err = "the input textures could not be created"; return false; }
        meta = sc.meta;
    } else {   // a real captured frame: the workload the game gives the synth (holes, the guard, the vectors), its own sizes and S format
        FrameData fd;
        if (!loadFrame(o.frameDir, o.frameIndex, fd, err)) return false;
        if (!uploadFrame(g, fd, gf)) { err = "the capture's input textures could not be created"; return false; }
        meta = fd.meta;
        o.rw = meta.rw; o.rh = meta.rh; o.ow = meta.ow; o.oh = meta.oh;
    }
    Synth sy;
    if (!sy.create(g, o.rw, o.rh, o.ow, o.oh, meta.outFmt)) { err = "the synth's textures could not be created"; return false; }
    bo.adapter = g.adapterName;
    bo.vram = sy.vram;

    // A kept f = 1 S to score against: one build with the luma pass; the residual is a small head turn (0.05 degrees) and the tangents the kept S was built with are this frame's.
    const FrameGenSynthCb cb1 = fillCb(meta, 1.0f, nullptr, nullptr);
    g.setCb(cb1);
    runCall(g, sy, gf.in(), false, true, false, nullptr);
    float keptRot[9], resid[9], hdC[9];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) keptRot[r * 3 + c] = cb1.rs[r][c];
    rotY(0.05f, hdC);
    frameGenSynthResidual(keptRot, hdC, resid);
    const FrameGenSynthCb cbScore = fillCb(meta, 1.0f, resid, meta.tanNow);
    const FrameGenSynthCb cbHalf = fillCb(meta, 0.5f, nullptr, nullptr);

    {
        uint32_t st[kFrameGenSynthStatsUints] = {};
        g.setCb(cb1);
        runCall(g, sy, gf.in(), false, true, false, nullptr);
        FrameGenSynthEyeResult r;
        if (sy.readStats(g, st)) {
            frameGenSynthDecodeStats(st, r);
            const double px = static_cast<double>(o.rw) * o.rh;
            bo.sanity = fmtStr("%s, f=1: filled %.2f%% left %.2f%% guard %.2f%% clamp %.3f%% of the render pixels", o.frameDir.empty() ? "synthetic scene" : "captured frame",
                               100.0 * r.filled / px, 100.0 * r.left / px, 100.0 * r.guard / px, 100.0 * r.clamped / px);
        }
    }
    VariantResult v0, v1;
    v0.name = "f=0.5 (build only: clear splat src resolve resample)";
    v1.name = "f=1 (score + build + keep: clear score splat src resolve resample luma)";
    if (!benchVariant(g, sy, gf.in(), cbHalf, false, false, o.warm, o.iters, v0) || !benchVariant(g, sy, gf.in(), cbScore, true, true, o.warm, o.iters, v1)) {
        err = "timestamp queries gave no usable data (disjoint)";
        return false;
    }
    bo.variants.push_back(v0);
    bo.variants.push_back(v1);
    bo.ok = true;
    return true;
}

void printBench(const BenchOptions& o, const BenchOutput& bo, const Gpu& g) {
    std::printf("adapter: %s (%.0f MiB dedicated%s)\n", bo.adapter.c_str(), static_cast<double>(g.dedicatedVram) / 1048576.0, g.warp ? ", WARP" : "");
    std::printf("render %ux%u, output %ux%u, %s, %d iterations after %d warm-up; ms per eye\n", o.rw, o.rh, o.ow, o.oh,
                o.frameDir.empty() ? "the synthetic scene (RGBA16F)" : fmtStr("frame %d of %s", o.frameIndex, o.frameDir.c_str()).c_str(), o.iters, o.warm);
    for (const VariantResult& v : bo.variants) {
        std::printf("\n%s\n  %-10s %10s %10s\n", v.name.c_str(), "pass", "median ms", "p95 ms");
        double sumMed = 0.0;
        for (int e = 0; e < kSteps; ++e) {
            if (v.st.ms[e].empty()) continue;
            const double med = percentile(v.st.ms[e], 0.5);
            sumMed += med;
            std::printf("  %-10s %10.3f %10.3f\n", kPassLabels[e], med, percentile(v.st.ms[e], 0.95));
        }
        std::printf("  %-10s %10.3f %10.3f   (sum of pass medians %.3f; p90 %.3f p99 %.3f max %.3f: a total past the sum is a gap between passes)\n", "TOTAL",
                    percentile(v.st.total, 0.5), percentile(v.st.total, 0.95), sumMed, percentile(v.st.total, 0.9), percentile(v.st.total, 0.99), percentile(v.st.total, 1.0));
    }
    std::printf("\n%s\n", bo.sanity.c_str());
    // The VRAM the synth allocates, as the module's own line says it (framegen_synth_report.h formatFrameGenSynthVram), measured from the bench's textures.
    std::printf("%s\n", formatFrameGenSynthVram(0, bo.vram).c_str());
}

std::string benchJson(const BenchOptions& o, const BenchOutput& bo, const Gpu& g) {
    std::string s = "{\n";
    s += fmtStr("  \"adapter\": \"%s\", \"warp\": %s, \"render\": [%u, %u], \"output\": [%u, %u], \"iterations\": %d, \"warmup\": %d, \"source\": \"%s\",\n", bo.adapter.c_str(),
                g.warp ? "true" : "false", o.rw, o.rh, o.ow, o.oh, o.iters, o.warm, o.frameDir.empty() ? "synthetic" : fmtStr("frame %d of a capture", o.frameIndex).c_str());
    const FrameGenSynthVram& v = bo.vram;
    s += fmtStr("  \"vram_bytes\": {\"per_eye\": %llu, \"scratch\": %llu, \"two_eyes\": %llu, \"eye\": {\"s\": %llu, \"luma\": %llu, \"class\": %llu, \"stats\": %llu}, "
                "\"shared\": {\"key\": %llu, \"read\": %llu, \"holes\": %llu}},\n",
                static_cast<unsigned long long>(v.eye()), static_cast<unsigned long long>(v.scratch()), static_cast<unsigned long long>(v.total()),
                static_cast<unsigned long long>(v.s), static_cast<unsigned long long>(v.luma), static_cast<unsigned long long>(v.cls), static_cast<unsigned long long>(v.stats),
                static_cast<unsigned long long>(v.key), static_cast<unsigned long long>(v.read), static_cast<unsigned long long>(v.holes));
    s += "  \"variants\": [\n";
    for (size_t i = 0; i < bo.variants.size(); ++i) {
        const VariantResult& v = bo.variants[i];
        s += fmtStr("    {\"name\": \"%s\", \"total_ms\": {\"median\": %.4f, \"p90\": %.4f, \"p95\": %.4f, \"p99\": %.4f, \"max\": %.4f}, \"passes\": {", v.name.substr(0, v.name.find(' ')).c_str(),
                    percentile(v.st.total, 0.5), percentile(v.st.total, 0.9), percentile(v.st.total, 0.95), percentile(v.st.total, 0.99), percentile(v.st.total, 1.0));
        bool first = true;
        for (int e = 0; e < kSteps; ++e) {
            if (v.st.ms[e].empty()) continue;
            s += fmtStr("%s\"%s\": {\"median\": %.4f, \"p95\": %.4f}", first ? "" : ", ", kPassLabels[e], percentile(v.st.ms[e], 0.5), percentile(v.st.ms[e], 0.95));
            first = false;
        }
        s += fmtStr("}}%s\n", i + 1 < bo.variants.size() ? "," : "");
    }
    s += "  ]\n}\n";
    return s;
}

// ---- the self-test ------------------------------------------------------------------------------------------------------------------------------------------

int g_fail = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}

// A world plane at 30 m scrolling 4 render pixels (8 output pixels) a frame to the right under an identity head: the f = 1 S built from frame k is frame k + 1.
void syntheticFrame(int k, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, FrameData& fd) {
    FrameMeta& m = fd.meta;
    m = FrameMeta{};
    m.rw = rw; m.rh = rh; m.ow = ow; m.oh = oh;
    m.outFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const float tan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
    std::memcpy(m.tanNow, tan, sizeof(tan));
    std::memcpy(m.tanPrev, tan, sizeof(tan));
    m.depthA = 0.0f; m.depthB = 0.025f; m.split = 10.0f;
    m.frame = 100 + k;
    m.qpcFreq = 10000000.0; m.qpc = 1000000.0 * k;
    m.variant = 1; m.f = 1.0f;
    fd.out.resize(static_cast<size_t>(ow) * oh * 8);
    fd.depth.resize(static_cast<size_t>(rw) * rh * 4);
    fd.mv.resize(static_cast<size_t>(rw) * rh * 4);
    for (uint32_t y = 0; y < oh; ++y) {
        for (uint32_t x = 0; x < ow; ++x) {
            const float xs = static_cast<float>(x) - 8.0f * static_cast<float>(k);
            const float v = 0.5f + 0.3f * std::sin(xs * 0.35f) + 0.15f * std::sin(static_cast<float>(y) * 0.3f + xs * 0.11f);
            uint16_t* o = reinterpret_cast<uint16_t*>(&fd.out[(static_cast<size_t>(y) * ow + x) * 8]);
            o[0] = floatToHalf(v); o[1] = floatToHalf(v * 0.9f); o[2] = floatToHalf(v * 0.8f); o[3] = floatToHalf(1.0f);
        }
    }
    const float z = 30.0f, depth = m.depthB / z;
    for (size_t i = 0; i < static_cast<size_t>(rw) * rh; ++i) {
        std::memcpy(&fd.depth[i * 4], &depth, 4);
        reinterpret_cast<uint16_t*>(&fd.mv[i * 4])[0] = floatToHalf(-4.0f);   // B to A: the content was 4 px to the left
        reinterpret_cast<uint16_t*>(&fd.mv[i * 4])[1] = floatToHalf(0.0f);
    }
}

bool writeCapture(const std::string& dir, const std::vector<FrameData>& frames) {
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string manifest = fmtStr("{\n  \"stamp\": \"selftest\", \"eye\": \"left\", \"frame_count\": %d, \"stopped_reason\": \"\",\n  \"files\": [\n", static_cast<int>(frames.size()));
    for (size_t k = 0; k < frames.size(); ++k) {
        const FrameData& f = frames[k];
        const std::string b = dir + "\\f" + std::to_string(k);
        if (!writeFile(b + "_out.bin", f.out.data(), f.out.size()) || !writeFile(b + "_depth.bin", f.depth.data(), f.depth.size()) || !writeFile(b + "_mv.bin", f.mv.data(), f.mv.size()) ||
            !writeText(b + ".json", metaToJson(f.meta, "DXGI_FORMAT_R16G16B16A16_FLOAT"))) return false;
        manifest += fmtStr("    {\"name\": \"f%zu_out.bin\", \"bytes\": %zu}, {\"name\": \"f%zu_depth.bin\", \"bytes\": %zu}, {\"name\": \"f%zu_mv.bin\", \"bytes\": %zu}%s\n", k, f.out.size(), k,
                           f.depth.size(), k, f.mv.size(), k + 1 < frames.size() ? "," : "");
    }
    manifest += "  ]\n}\n";
    return writeText(dir + "\\manifest.json", manifest);
}

void removeDir(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    const HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!std::strcmp(fd.cFileName, ".") || !std::strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeDir(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

int selfTest() {
    // 1. the JSON reader and the key tolerance.
    {
        Json j;
        std::string e;
        const bool ok = parseJson("{\"Render\": {\"W\": 8, \"H\": 6}, \"tan-now\": [-1, 1, -0.5, 0.5], \"hd\": [[1,0,0],[0,1,0],[0,0,1]], \"flag\": true, \"s\": \"x\", \"n\": null}", j, e);
        check(ok && jfind(j, {"renderw"}) && (*jfind(j, {"renderw"}))[0] == 8 && jfind(j, {"tannow"}) && jfind(j, {"tannow"})->size() == 4 && jfind(j, {"hd"}) && jfind(j, {"hd"})->size() == 9 &&
                  (*jfind(j, {"flag"}))[0] == 1.0 && j.str["s"] == "x",
              "JSON: nested objects flatten, keys normalise, nested arrays flatten row-major, booleans read as 0/1");
        check(!parseJson("{\"a\": [1, 2", j, e) && !e.empty(), "JSON: a truncated document is refused with a reason");
    }
    // 2. the metadata round trip and the constant block against the production fill.
    {
        FrameMeta m;
        m.rw = 2817; m.rh = 2448; m.ow = 3756; m.oh = 3264;
        m.outFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const float tn[4] = {-1.17f, 1.04f, -1.08f, 1.11f}, tp[4] = {-1.171f, 1.041f, -1.081f, 1.111f};
        std::memcpy(m.tanNow, tn, sizeof(tn));
        std::memcpy(m.tanPrev, tp, sizeof(tp));
        m.depthA = 0.0f; m.depthB = 0.025f; m.split = 10.0f;
        float R[9];
        rotY(1.3f, R);
        std::memcpy(m.d.Hd, R, sizeof(R));
        m.d.th[0] = 0.0123f; m.d.th[1] = -0.004f; m.d.th[2] = 0.0071f;
        m.d.tv[0] = 0.5f; m.worldValid = true; m.cameraJump = false; m.frame = 77; m.qpc = 123456789.0; m.qpcFreq = 10000000.0; m.variant = 0; m.f = 0.5f;
        FrameMeta back;
        Json j;
        std::string e;
        const bool parsed = parseJson(metaToJson(m, "DXGI_FORMAT_R16G16B16A16_FLOAT"), j, e) && metaFromJson(j, back, e);
        check(parsed, "frame JSON: the writer's output parses back (" + e + ")");
        float resid[9], rot2[9];
        rotY(0.2f, rot2);
        float keptRot[9];
        const FrameGenSynthCb a = fillCb(m, 0.5f, nullptr, nullptr);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) keptRot[r * 3 + c] = a.rs[r][c];
        frameGenSynthResidual(keptRot, rot2, resid);
        FrameGenSynthCb direct, rebuilt;
        frameGenSynthFillCb(direct, 2817, 2448, 3756, 3264, tn, 0.0f, 0.025f, 10.0f, 1.0f, m.d, resid, tp);
        rebuilt = fillCb(back, 1.0f, resid, tp);
        check(parsed && std::memcmp(&direct, &rebuilt, sizeof(direct)) == 0 && back.rw == m.rw && back.oh == m.oh && back.frame == 77 && back.variant == 0 && back.worldValid &&
                  back.outFmt == DXGI_FORMAT_R16G16B16A16_FLOAT,
              "the constant block rebuilt from a frame's JSON equals frameGenSynthFillCb on the original numbers (f = 1, with a residual and the kept tangents)");
        Json j2;
        FrameMeta incomplete;
        std::string e2;
        parseJson("{\"render_w\": 4, \"render_h\": 4}", j2, e2);
        check(!metaFromJson(j2, incomplete, e2) && e2.find("missing key") == 0, "frame JSON: a missing required key is named");
        Json j3;
        FrameMeta alt;
        std::string e3;
        parseJson("{\"render\":[64,48],\"output\":{\"w\":128,\"h\":96},\"outFormat\":10,\"tanNow\":[-1,1,-0.75,0.75],\"depthKnobs\":[0,0.025],\"splitMetres\":10,\"Hd\":[1,0,0,0,1,0,0,0,1],\"th\":[0,0,0]}", j3, e3);
        check(metaFromJson(j3, alt, e3) && alt.rw == 64 && alt.oh == 96 && alt.depthB == 0.025f, "frame JSON: pair, nested and camelCase spellings are read (" + e3 + ")");
    }
    // 3. a synthetic capture: written, read back, replayed on WARP.
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    const std::string root = std::string(tmp) + "framegen_synth_bench_selftest_" + std::to_string(GetCurrentProcessId());
    const uint32_t rw = 128, rh = 96, ow = 256, oh = 192;
    std::vector<FrameData> frames(6);
    for (int k = 0; k < 6; ++k) syntheticFrame(k, rw, rh, ow, oh, frames[k]);
    check(writeCapture(root, frames), "a six-frame capture is written in the replay format");
    {
        std::string note, e;
        check(captureFrameCount(root, note) == 6, "the reader counts the capture's six frames");
        FrameData rd;
        const bool ok = loadFrame(root, 3, rd, e);
        check(ok && rd.out == frames[3].out && rd.depth == frames[3].depth && rd.mv == frames[3].mv && rd.meta.frame == 103 && rd.meta.ow == ow,
              "frame 3 reads back byte for byte with its metadata (" + e + ")");
        FILE* f = std::fopen((root + "\\f3_mv.bin").c_str(), "ab");
        if (f) { std::fputc(0, f); std::fclose(f); }
        FrameData bad;
        check(!loadFrame(root, 3, bad, e) && e.find("mv.bin") != std::string::npos, "a .bin of the wrong size is refused");
        writeFile(root + "\\f3_mv.bin", frames[3].mv.data(), frames[3].mv.size());
    }
    {   // the default PNG folder is never inside the capture: not for this capture, nor for one that is the temp folder, the default's base or the default itself
        const std::string d = defaultPngDir(root);
        check(!d.empty() && !insideDir(d, root) && insideDir(d, std::string(tmp)), "the default PNG folder is under %TEMP%, not inside the capture (" + d + ")");
        bool never = true;
        const std::string leaf = root.substr(root.find_last_of('\\') + 1);
        for (const std::string& c : {std::string(tmp), std::string(tmp) + "edvr_bench_out", std::string(tmp) + "edvr_bench_out\\" + leaf, std::string("bench_out"), std::string(".")}) {
            const std::string dc = defaultPngDir(c);
            if (!dc.empty() && insideDir(dc, c)) { never = false; std::printf("  the default for %s is %s, inside it\n", c.c_str(), dc.c_str()); }
        }
        check(never, "the default PNG folder never resolves inside the capture (a capture that is the temp folder, the default's base, the default itself, the current directory)");
        check(insideDir(root + "\\a\\b", root) && insideDir(root, root) && !insideDir(root + "x", root) && insideDir(root + "/A/../a", root + "\\"),
              "the inside test: a child, the folder itself, a sibling sharing a prefix, slashes, dot-dot and case");
    }
    Gpu g;
    std::string err;
    if (!g.init(-1, true, err)) {
        std::printf("SKIP: WARP is unavailable (%s); the GPU parts of the self-test did not run\n", err.c_str());
    } else {
        ReplayOptions ro;
        ro.pngDir = root + "\\png\\nested\\deeper";   // --png-dir makes its missing parents
        ReplaySummary sum;
        const bool ok = replayFolder(g, root, ro, sum, err);
        check(ok, "the synthetic capture replays on WARP (" + err + ")");
        if (ok) {
            check(sum.pairs.size() == 5 && sum.agg.scored == 5, "five consecutive pairs, all scored");
            const FrameGenShadowClassStats& w = sum.agg.cls[2];
            const double mean = w.pixels ? static_cast<double>(w.sumErr1024) / 1024.0 / w.pixels : 1e9;
            const double meanId = w.pixels ? static_cast<double>(w.sumErrId1024) / 1024.0 / w.pixels : 0.0;
            std::printf("  world class: warped mean %.4f, no-warp mean %.4f, %u cells\n", mean, meanId, w.pixels);
            check(w.pixels > 0 && meanId > 0.01 && mean < 0.5 * meanId, "the f = 1 S of a scene scrolling at the vectors' rate predicts the next frame better than the frame before it");
            const FrameGenShadowClassStats& wt = sum.agg.tcls[2];
            check(wt.pixels > 0 && wt.sumErr1024 == w.sumErr1024, "an identity residual head rotation scores the turned S the same as the raw one");
            check(GetFileAttributesA((ro.pngDir + "\\f0_S05.png").c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA((ro.pngDir + "\\f1_B.png").c_str()) != INVALID_FILE_ATTRIBUTES,
                  "the f = 0.5 S and the B pictures are written as PNGs, into a --png-dir whose parents did not exist");
        }
        // the bench machinery, tiny
        BenchOptions bo;
        bo.rw = 128; bo.rh = 96; bo.ow = 256; bo.oh = 192; bo.iters = 4; bo.warm = 2;
        BenchOutput out;
        if (runBench(g, bo, out, err)) {
            check(out.variants.size() == 2 && out.variants[0].st.total.size() == 4 && out.variants[1].st.ms[kFgSynScore].size() == 4 && out.variants[0].st.ms[kFgSynScore].empty() &&
                      out.vram.eye() > 0 && out.vram.scratch() > 0 && !benchJson(bo, out, g).empty(),
                  "the bench times both variants pass by pass (f = 0.5 has no score pass, f = 1 does) and reports the VRAM");
        } else {
            std::printf("SKIP: the bench on WARP gave nothing (%s); WARP may not support timestamp queries\n", err.c_str());
        }
    }
    removeDir(root);
    std::printf(g_fail ? "framegen_synth_bench self-test: %d FAILED\n" : "framegen_synth_bench self-test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

bool parseWxH(const char* s, uint32_t& w, uint32_t& h) {
    unsigned a = 0, b = 0;
    if (std::sscanf(s, "%ux%u", &a, &b) != 2 || !a || !b) return false;
    w = a; h = b;
    return true;
}

int usage() {
    std::printf("usage: framegen_synth_bench --bench [--render WxH] [--output WxH] [--frame FOLDER[:k]] [--iters N] [--warmup N] [--adapter N] [--json FILE]\n"
                "       framegen_synth_bench --replay FOLDER [--png-dir DIR] [--no-png] [--adapter N] [--json FILE] [--dry-run]\n"
                "       framegen_synth_bench --self-test | --list-adapters\n"
                "       --bench and --replay take [--define NAME=V] (a shader switch compiled as V)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    bool bench = false, test = false, list = false;
    std::string replay;
    int adapter = -1;
    BenchOptions bopt;
    ReplayOptions ropt;
    std::string json;
    std::vector<std::pair<std::string, std::string>> defines;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--bench") bench = true;
        else if (a == "--self-test") test = true;
        else if (a == "--list-adapters") list = true;
        else if (a == "--replay") replay = next();
        else if (a == "--render") { if (!parseWxH(next(), bopt.rw, bopt.rh)) return usage(); }
        else if (a == "--output") { if (!parseWxH(next(), bopt.ow, bopt.oh)) return usage(); }
        else if (a == "--frame") {   // FOLDER or FOLDER:k (a drive letter's colon is at index 1, a frame index's past it)
            bopt.frameDir = next();
            const size_t colon = bopt.frameDir.rfind(':');
            if (colon != std::string::npos && colon > 1) { bopt.frameIndex = std::atoi(bopt.frameDir.c_str() + colon + 1); bopt.frameDir.resize(colon); }
            if (bopt.frameDir.empty() || bopt.frameIndex < 0) return usage();
        }
        else if (a == "--iters") bopt.iters = std::atoi(next());
        else if (a == "--warmup") bopt.warm = std::atoi(next());
        else if (a == "--adapter") adapter = std::atoi(next());
        else if (a == "--json") json = next();
        else if (a == "--png-dir") ropt.pngDir = next();
        else if (a == "--no-png") ropt.png = false;
        else if (a == "--dry-run") ropt.dry = true;
        else if (a == "--define") {   // NAME=V
            const std::string d = next();
            const size_t eq = d.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == d.size()) return usage();
            defines.emplace_back(d.substr(0, eq), d.substr(eq + 1));
        }
        else return usage();
    }
    if (test) return selfTest();
    if (list) {
        ComPtr<IDXGIFactory1> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return 1;
        ComPtr<IDXGIAdapter1> ad;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; ++i, ad.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            std::printf("adapter %u: %s (%.0f MiB dedicated%s)\n", i, name, static_cast<double>(d.DedicatedVideoMemory) / 1048576.0, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "");
        }
        return 0;
    }
    if (!bench && replay.empty()) return usage();
    if (bopt.iters < 1 || bopt.warm < 0) return usage();
    Gpu g;
    g.defines = defines;
    std::string err;
    if (!g.init(adapter, false, err)) { std::printf("FAIL: %s\n", err.c_str()); return 1; }
    for (const auto& d : defines) std::printf("shader switch: %s %s\n", d.first.c_str(), d.second.c_str());
    if (bench) {
        BenchOutput out;
        if (!runBench(g, bopt, out, err)) { std::printf("FAIL: %s\n", err.c_str()); return 1; }
        printBench(bopt, out, g);
        if (!json.empty() && !writeText(json, benchJson(bopt, out, g))) { std::printf("FAIL: cannot write %s\n", json.c_str()); return 1; }
        return 0;
    }
    if (ropt.pngDir.empty() && ropt.png) {
        ropt.pngDir = defaultPngDir(replay);
        if (ropt.pngDir.empty()) { std::printf("FAIL: every default PNG folder is inside the capture %s; give --png-dir or --no-png\n", replay.c_str()); return 1; }
        if (!ropt.dry) std::printf("png: %s\n", ropt.pngDir.c_str());
    }
    ReplaySummary sum;
    std::printf("adapter: %s\n", g.adapterName.c_str());
    if (!replayFolder(g, replay, ropt, sum, err)) { std::printf("FAIL: %s\n", err.c_str()); return 1; }
    if (!json.empty()) {
        std::string s = fmtStr("{\n  \"adapter\": \"%s\", \"frames\": %d, \"scored\": %u,\n  \"pairs\": [\n", g.adapterName.c_str(), sum.frames, sum.agg.scored);
        for (size_t i = 0; i < sum.pairs.size(); ++i) {
            const PairResult& p = sum.pairs[i];
            s += fmtStr("    {\"b\": %d, \"c\": %d, \"scored\": %s, \"rot_res_deg\": %.4f, \"line\": \"%s\", \"sharpness_f05\": \"%s\", \"sharpness_f1\": \"%s\", \"hole_px\": %llu, \"hole_err_s\": %.6f, \"hole_err_b\": %.6f}%s\n", p.b, p.c,
                        p.scored ? "true" : "false", static_cast<double>(p.rotDeg), p.line.c_str(), p.sharp05.text().c_str(), p.sharp1.text().c_str(),
                        static_cast<unsigned long long>(p.holes.pixels), p.holes.meanS(), p.holes.meanB(), i + 1 < sum.pairs.size() ? "," : "");
        }
        s += fmtStr("  ],\n  \"aggregate\": \"%s\",\n  \"sharpness_f05\": \"%s\", \"sharpness_f1\": \"%s\",\n  \"sharpness_rest_f05\": %.4f, \"sharpness_rest_f1\": %.4f,\n  \"hole_px\": %llu, \"hole_err_s\": %.6f, \"hole_err_b\": %.6f\n}\n", sum.aggLine.c_str(),
                    sum.sharp05.text().c_str(), sum.sharp1.text().c_str(), sum.sharp05.restRatio(), sum.sharp1.restRatio(), static_cast<unsigned long long>(sum.holes.pixels), sum.holes.meanS(),
                    sum.holes.meanB());
        writeText(json, s);
    }
    return 0;
}
