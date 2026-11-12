// The hull variant bench's files, end to end without the game (TEMPORARY, with the bench; src\d3d11\hull_bench.h).
//
//   hull_bench_test.exe <out dir>
//
// Writes one synthetic bench press through the DLL's own code -- hull_bench.cpp's file names, its writer thread (EDVRTEX1)
// and its manifest builder -- the way hull_layer.cpp drives them: a disk "hull" with panel stripes over a still textured
// terrain, 8 eye-0 frames a variant, eye 1 and the headset frame on frame 0, the mask in RGBA16F, the variants this build
// does not run refused. Prints the prefix; build.bat then runs tools\hull_bench_report.py on it, which must read it (exit 0).
// Self-checks here: the names, the JSON's shape, the writer's counts.
#include "../../src/d3d11/hull_bench.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace edvr::hull_bench;

namespace {

int g_fail = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("hull_bench_test: FAIL %s\n", what);
        ++g_fail;
    }
}

constexpr uint32_t W = 96, H = 72;

uint16_t toHalf(float f) {   // [0, 1] values only: no denormals, infinities or NaNs here
    if (f <= 0.0f) return 0;
    uint32_t x = 0;
    std::memcpy(&x, &f, 4);
    const int e = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    if (e <= 0) return 0;
    return static_cast<uint16_t>((e << 10) | ((x >> 13) & 0x3FF));
}

struct Scene {
    std::vector<float> terrain;   // W*H*3
    Scene() : terrain(W * H * 3) {
        uint32_t s = 12345;
        for (auto& v : terrain) {
            s = s * 1664525u + 1013904223u;
            v = 0.2f + 0.4f * static_cast<float>(s >> 8) / 16777216.0f;
        }
    }
    // Coverage and hull colour at pixel (x, y), supersampled ss x ss with an offset (jx, jy).
    void hull(uint32_t x, uint32_t y, float jx, float jy, int ss, float* a, float rgb[3]) const {
        *a = 0.0f;
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        for (int sy = 0; sy < ss; ++sy)
            for (int sx = 0; sx < ss; ++sx) {
                const float px = x + (sx + 0.5f) / ss + jx, py = y + (sy + 0.5f) / ss + jy;
                const float dx = px - 48.0f, dy = py - 36.0f;
                if (dx * dx + dy * dy >= 26.0f * 26.0f) continue;
                const float t = 0.55f + 0.2f * std::sin(0.3f * px + 0.2f * py);
                *a += 1.0f;
                rgb[0] += t;
                rgb[1] += 0.9f * t;
                rgb[2] += 0.8f * t;
            }
        const float n = static_cast<float>(ss * ss);
        *a /= n;
        for (int k = 0; k < 3; ++k) rgb[k] /= n;
    }
    // One frame, RGBA8 (bgra: B8G8R8A8 order).
    std::vector<uint8_t> frame(float jx, float jy, int ss, bool bgra) const {
        std::vector<uint8_t> out(W * H * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                float a = 0.0f, c[3];
                hull(x, y, jx, jy, ss, &a, c);
                uint8_t px[4];
                for (int k = 0; k < 3; ++k) {
                    const float v = c[k] + (1.0f - a) * terrain[(y * W + x) * 3 + k];
                    px[k] = static_cast<uint8_t>(std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f));
                }
                px[3] = 255;
                uint8_t* o = &out[(y * W + x) * 4];
                o[0] = bgra ? px[2] : px[0];
                o[1] = px[1];
                o[2] = bgra ? px[0] : px[2];
                o[3] = 255;
            }
        return out;
    }
    // The mask: premultiplied hull colour and its coverage, RGBA16F.
    std::vector<uint8_t> mask() const {
        std::vector<uint8_t> out(W * H * 8);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                float a = 0.0f, c[3];
                hull(x, y, 0.0f, 0.0f, 4, &a, c);
                const uint16_t h[4] = {toHalf(c[0]), toHalf(c[1]), toHalf(c[2]), toHalf(a)};
                std::memcpy(&out[(y * W + x) * 8], h, 8);
            }
        return out;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: hull_bench_test.exe <out dir>\n");
        return 2;
    }
    const std::string dirA = argv[1];
    const std::wstring dir(dirA.begin(), dirA.end());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string stamp = "010203004001";

    // The names hull_bench_report.py globs and joins.
    expect(frameName(stamp, kSepTaa, 3, 0) == "eye_010203004001_Bench_sep_taa_f03.bin", "frame name, eye 0");
    expect(frameName(stamp, kOrigA, 0, 1) == "eye_010203004001_Bench_orig_a_f00_e1.bin", "frame name, eye 1");
    expect(finalName(stamp, kSepNoaa, 1) == "eye_010203004001_Bench_sep_noaa_final_e1.bin", "final name");
    expect(maskName(stamp, 0) == "eye_010203004001_Bench_Mask.bin" && maskName(stamp, 1) == "eye_010203004001_Bench_Mask_e1.bin", "mask names");
    expect(manifestName(stamp) == "eye_010203004001_Bench.json", "manifest name");
    expect(median({3.0, 1.0, 2.0}).v == 2.0 && median({1.0, 2.0, 3.0, 4.0}).v == 2.5 && !median({}).have, "median");

    const Scene scene;
    Writer writer;
    auto path = [&](const std::string& name) { return dir + L"\\" + std::wstring(name.begin(), name.end()); };
    Manifest m;
    m.stamp = stamp;
    m.build = "v0.0.0-test";
    m.render[0] = 72;
    m.render[1] = 54;
    m.output[0] = W;
    m.output[1] = H;
    const uint32_t rr[4] = {0, 0, 72, 54}, ro[4] = {0, 0, W, H};
    std::memcpy(m.rectRender, rr, sizeof rr);
    std::memcpy(m.rectOutput, ro, sizeof ro);
    m.dlssPreset = "K";
    m.dlssQuality = "quality";
    m.dlssVersion = "test";
    m.overlayNote = "synthetic";
    writer.push(path(maskName(stamp, 0)), W, H, 10, W * 8, 0, 0, scene.mask());
    writer.push(path(maskName(stamp, 1)), W, H, 10, W * 8, 0, 1, scene.mask());
    uint32_t files = 2;
    for (int v = 0; v < kVariants; ++v) {
        VariantRec& r = m.variants[v];
        if (v == kNative || v == kNativeFxaa) {
            r.status = "refused";
            r.reason = "needs shader dump (feasibility builds A and C)";
            continue;
        }
        r.status = "ok";
        r.warmFrames = v == kSepNoaa ? 8 : 60;
        const bool bgra = v == kOrigA || v == kOrigB;   // the submitted eye may be B8G8R8A8 (87): the report reads both
        for (int k = 0; k < kCaptureFrames; ++k) {
            // sep_noaa: one jittered sample a pixel (aliased); the rest supersampled (anti-aliased, still).
            const float jx = v == kSepNoaa ? 0.37f * ((k * 5) % 8) / 8.0f - 0.18f : 0.0f;
            const float jy = v == kSepNoaa ? 0.41f * ((k * 3) % 8) / 8.0f - 0.2f : 0.0f;
            std::vector<uint8_t> img = scene.frame(jx, jy, v == kSepNoaa ? 1 : 4, bgra);
            Frame f;
            f.index = k;
            f.layerFrame = 1000u + 100u * static_cast<uint32_t>(v) + static_cast<uint32_t>(k);
            f.file = frameName(stamp, v, k, 0);
            f.jitter[0] = jx;
            f.jitter[1] = jy;
            f.headRotDeg = 0.01f;
            f.tanNow[0] = -1.0f;
            f.tanNow[1] = 1.0f;
            f.tanNow[2] = 1.0f;
            f.tanNow[3] = -1.0f;
            r.frames.push_back(f);
            if (k == 0) {
                writer.push(path(frameName(stamp, v, 0, 1)), W, H, bgra ? 87 : 28, W * 4, f.layerFrame, 1, img);
                writer.push(path(finalName(stamp, v, 0)), W, H, bgra ? 87 : 28, W * 4, f.layerFrame, 0, img);
                r.finalFiles[0] = finalName(stamp, v, 0);
                files += 2;
            }
            writer.push(path(f.file), W, H, bgra ? 87 : 28, W * 4, f.layerFrame, 0, std::move(img));
            ++files;
        }
        const bool sep = !(v == kOrigA || v == kOrigB);
        const bool dlss = v == kDlssFull || v == kDlssSub;
        for (auto& s : r.layerStage) s = Num{true, sep ? 0.1 : 0.0};
        r.hullDlss = Num{true, dlss ? 0.9 : 0.0};
        r.mainDlss = Num{true, 1.5};
        r.frameSpan = Num{true, 9.0 + v};
        r.cpuMs = Num{true, 0.2};
        r.mbOwn = Num{true, sep ? 40.0 : 0.0};
        r.mbNgx = Num{true, dlss ? 120.0 : 0.0};
    }
    writer.stop();
    expect(writer.written() == files && writer.failed() == 0, "the writer wrote every capture");
    expect(writer.idle() && writer.queuedBytes() == 0, "the writer drained");

    const std::string json = manifestJson(m);
    expect(json.find("\"schema\": 1") != std::string::npos && json.find("\"aborted\": null") != std::string::npos, "manifest header");
    expect(json.find("\"order\": [\"orig_a\", \"sep_taa\", \"sep_norefl\", \"sep_nopre\", \"sep_norect\", \"sep_noaa\", \"dlss_full\", \"dlss_sub\", \"native\", \"native_fxaa\", \"orig_b\"]") !=
               std::string::npos,
           "manifest order");
    expect(json.find("{\"id\": \"native\", \"status\": \"refused\", \"reason\": \"needs shader dump") != std::string::npos, "a refused variant");
    expect(json.find("\"hull_dlss\": 0.9000") != std::string::npos && json.find("\"ngx\": 120.0") != std::string::npos, "the second DLSS's ms and MB");
    expect(json.find("\"layer_stage\": [0.1000, 0.1000") != std::string::npos, "the stage times");
    Manifest q;
    q.aborted = "a \"quoted\" reason\n";
    const std::string qj = manifestJson(q);
    expect(qj.find("\"aborted\": \"a \\\"quoted\\\" reason\\u000a\"") != std::string::npos && qj.find("\"main_dlss\": null") != std::string::npos,
           "escapes and nulls");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path(manifestName(stamp)).c_str(), L"wb") == 0 && f) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    } else {
        expect(false, "the manifest was written");
    }
    if (g_fail) return 1;
    std::printf("hull_bench_test: passed (%u captures written); prefix %s\\eye_%s\n", files, dirA.c_str(), stamp.c_str());
    return 0;
}
