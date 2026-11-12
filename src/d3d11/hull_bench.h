// The hull variant bench's files (hotkey.hull_bench, TEMPORARY; docs\research-own-hull-layer-2026-11-08.md "Variant bench"):
// the manifest eye_<stamp>_Bench.json, the capture file names, and the writer thread that puts the EDVRTEX1 captures on disk.
// No D3D here: the state machine that runs the variants lives in hull_layer.cpp; tools\hull_bench_test builds this file alone
// and checks its output with tools\hull_bench_report.py, the reader.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edvr {
namespace hull_bench {

// The variants, in the bench's order (hull_bench_report.py ORDER).
// sep_norefl, sep_nopre, sep_norect: sep_taa with one feature off (the reflection re-issue, the depth prefill, the Build A'
// rectangle), to find which moves dluma (docs "Bench A/B variants").
enum Variant : int { kOrigA = 0, kSepTaa, kSepNorefl, kSepNopre, kSepNorect, kSepNoaa, kDlssFull, kDlssSub, kNative, kNativeFxaa, kOrigB, kVariants };
const char* variantId(int v);

constexpr int kCaptureFrames = 8;   // eye 0 frames a variant; eye 1 and the headset frame on its frame 0
constexpr int kLayerStages = 6;     // hull_layer.cpp TStage: gbuffer, mask_filter, screen, light, tone, taa

// One eye-0 capture frame of a variant.
struct Frame {
    int index = 0;
    uint32_t layerFrame = 0;
    std::string file;   // the file's name (no directory)
    float jitter[2] = {};
    float headRotDeg = -1.0f;   // < 0: not known (no head delta that frame)
    float tanNow[4] = {};
};

// A median, or "not measured" (null in the JSON).
struct Num {
    bool have = false;
    double v = 0.0;
};

struct VariantRec {
    std::string status = "refused";   // ok | refused | aborted
    std::string reason;
    uint32_t warmFrames = 0, fallbackFrames = 0;
    std::vector<Frame> frames;
    std::string finalFiles[2];
    bool finalFlip[2][2] = {};          // the headset frame's flipU, flipV per eye
    Num layerStage[kLayerStages], hullDlss, mainDlss, frameSpan, cpuMs;
    Num mbOwn, mbNgx, mbDxgi;
};

struct Manifest {
    std::string stamp, build;
    uint32_t render[2] = {}, output[2] = {};
    uint32_t rectRender[4] = {}, rectOutput[4] = {};
    std::string dlssPreset, dlssQuality, dlssVersion;
    std::string aborted;   // empty: not aborted (null)
    bool evicted = false, overlayInFinal = false;
    std::string overlayNote;
    VariantRec variants[kVariants];
};

// eye_<stamp>_Bench.json's text (schema 1, as hull_bench_report.py reads it).
std::string manifestJson(const Manifest& m);
// eye_<stamp>_Bench_<id>_f<NN>.bin (eye 0) and _f00_e1.bin (eye 1).
std::string frameName(const std::string& stamp, int variant, int frame, int eye);
// eye_<stamp>_Bench_<id>_final.bin / _final_e1.bin: the headset frame.
std::string finalName(const std::string& stamp, int variant, int eye);
// eye_<stamp>_Bench_Mask.bin / _Mask_e1.bin: the hull coverage (alpha), the report's region source.
std::string maskName(const std::string& stamp, int eye);
std::string manifestName(const std::string& stamp);

// The median of v (empty: not measured).
Num median(std::vector<double> v);

// One EDVRTEX1 file: magic, version 1, width, height, DXGI format, row bytes, frame, eye, 0, 0, then the rows.
bool writeTex(const std::wstring& path, uint32_t w, uint32_t h, uint32_t format, uint32_t rowBytes, uint32_t frame, uint32_t eye,
              const uint8_t* rows);

// The writer thread: captures copied off the GPU (tightly packed rows) go to disk in order. The queue's bytes are the budget the
// bench pauses on (kQueueBudget); the thread starts with the first job and ends at stop().
class Writer {
public:
    static constexpr uint64_t kQueueBudget = 768ull << 20;
    ~Writer();
    void push(std::wstring path, uint32_t w, uint32_t h, uint32_t format, uint32_t rowBytes, uint32_t frame, uint32_t eye,
              std::vector<uint8_t> data);
    uint64_t queuedBytes();
    bool idle();   // nothing queued and nothing being written
    // Waits for the queue to drain and joins the thread.
    void stop();
    uint32_t written() const { return written_; }
    uint32_t failed() const { return failed_; }
    uint64_t bytesWritten() const { return bytes_; }
    void resetCounts() {
        written_ = 0;
        failed_ = 0;
        bytes_ = 0;
    }

private:
    struct Job {
        std::wstring path;
        uint32_t w = 0, h = 0, format = 0, rowBytes = 0, frame = 0, eye = 0;
        std::vector<uint8_t> data;
    };
    void run();
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::thread thread_;
    uint64_t queued_ = 0;
    bool busy_ = false, quit_ = false;
    std::atomic<uint32_t> written_{0}, failed_{0};   // the writer thread's counts
    std::atomic<uint64_t> bytes_{0};
};

}  // namespace hull_bench
}  // namespace edvr
