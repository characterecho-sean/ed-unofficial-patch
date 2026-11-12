// The hull variant bench's files (TEMPORARY; hull_bench.h).
#include "hull_bench.h"

#include <algorithm>
#include <cstdio>

namespace edvr {
namespace hull_bench {

namespace {
const char* const kIds[kVariants] = {"orig_a", "sep_taa", "sep_norefl", "sep_nopre", "sep_norect", "sep_noaa", "dlss_full", "dlss_sub", "native", "native_fxaa", "orig_b"};

// A JSON string literal of s (the reasons are this file's and hull_layer.cpp's own words; quotes and controls escaped anyway).
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string num(const Num& n, int digits) {
    if (!n.have) return "null";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", digits, n.v);
    return buf;
}

std::string fmt(const char* f, double a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}
}  // namespace

const char* variantId(int v) { return v >= 0 && v < kVariants ? kIds[v] : "?"; }

std::string frameName(const std::string& stamp, int variant, int frame, int eye) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "eye_%s_Bench_%s_f%02d%s.bin", stamp.c_str(), variantId(variant), frame, eye ? "_e1" : "");
    return buf;
}

std::string finalName(const std::string& stamp, int variant, int eye) {
    return "eye_" + stamp + "_Bench_" + variantId(variant) + (eye ? "_final_e1.bin" : "_final.bin");
}

std::string maskName(const std::string& stamp, int eye) { return "eye_" + stamp + (eye ? "_Bench_Mask_e1.bin" : "_Bench_Mask.bin"); }

std::string manifestName(const std::string& stamp) { return "eye_" + stamp + "_Bench.json"; }

Num median(std::vector<double> v) {
    Num n;
    if (v.empty()) return n;
    std::sort(v.begin(), v.end());
    const size_t k = v.size() / 2;
    n.v = v.size() % 2 ? v[k] : 0.5 * (v[k - 1] + v[k]);
    n.have = true;
    return n;
}

std::string manifestJson(const Manifest& m) {
    std::string j = "{\"schema\": 1, \"stamp\": " + quote(m.stamp) + ", \"build\": " + quote(m.build);
    j += ", \"render\": [" + std::to_string(m.render[0]) + ", " + std::to_string(m.render[1]) + "]";
    j += ", \"output\": [" + std::to_string(m.output[0]) + ", " + std::to_string(m.output[1]) + "]";
    j += ", \"rect_render\": [";
    for (int k = 0; k < 4; ++k) j += (k ? ", " : "") + std::to_string(m.rectRender[k]);
    j += "], \"rect_output\": [";
    for (int k = 0; k < 4; ++k) j += (k ? ", " : "") + std::to_string(m.rectOutput[k]);
    j += "],\n \"dlss\": {\"preset\": " + quote(m.dlssPreset) + ", \"quality\": " + quote(m.dlssQuality) + ", \"version\": " + quote(m.dlssVersion) + "}";
    j += ", \"fg\": false, \"order\": [";
    for (int v = 0; v < kVariants; ++v) j += (v ? ", " : "") + quote(kIds[v]);
    j += "], \"aborted\": " + (m.aborted.empty() ? std::string("null") : quote(m.aborted));
    j += ", \"evicted\": " + std::string(m.evicted ? "true" : "false");
    j += ", \"overlay_in_final\": " + std::string(m.overlayInFinal ? "true" : "false");
    j += ", \"overlay_note\": " + quote(m.overlayNote);
    j += ",\n \"variants\": [";
    for (int v = 0; v < kVariants; ++v) {
        const VariantRec& r = m.variants[v];
        j += std::string(v ? "," : "") + "\n  {\"id\": " + quote(kIds[v]) + ", \"status\": " + quote(r.status) + ", \"reason\": " + quote(r.reason);
        j += ", \"warm_frames\": " + std::to_string(r.warmFrames) + ", \"fallback_frames\": " + std::to_string(r.fallbackFrames);
        j += ", \"frames\": [";
        for (size_t i = 0; i < r.frames.size(); ++i) {
            const Frame& f = r.frames[i];
            j += std::string(i ? ", " : "") + "{\"index\": " + std::to_string(f.index) + ", \"layer_frame\": " + std::to_string(f.layerFrame) +
                 ", \"file\": " + quote(f.file) + ", \"jitter\": [" + fmt("%.6g", f.jitter[0]) + ", " + fmt("%.6g", f.jitter[1]) + "]" +
                 ", \"head_rot_deg\": " + (f.headRotDeg >= 0.0f ? fmt("%.6g", f.headRotDeg) : std::string("null")) + ", \"tanNow\": [";
            for (int k = 0; k < 4; ++k) j += (k ? ", " : "") + fmt("%.9g", f.tanNow[k]);
            j += "]}";
        }
        j += "], \"final_files\": [" + quote(r.finalFiles[0]) + ", " + quote(r.finalFiles[1]) + "]";
        j += ", \"final_flip\": [[" + std::string(r.finalFlip[0][0] ? "true" : "false") + ", " + (r.finalFlip[0][1] ? "true" : "false") + "], [" +
             (r.finalFlip[1][0] ? "true" : "false") + ", " + (r.finalFlip[1][1] ? "true" : "false") + "]]";
        j += ", \"gpu_ms\": {\"layer_stage\": [";
        for (int k = 0; k < kLayerStages; ++k) j += (k ? ", " : "") + num(r.layerStage[k], 4);
        j += "], \"hull_dlss\": " + num(r.hullDlss, 4) + ", \"main_dlss\": " + num(r.mainDlss, 4) + ", \"frame_span\": " + num(r.frameSpan, 4) +
             ", \"cpu_ms\": " + num(r.cpuMs, 4) + "}";
        j += ", \"mb\": {\"own\": " + num(r.mbOwn, 1) + ", \"ngx\": " + num(r.mbNgx, 1) + ", \"dxgi\": " + num(r.mbDxgi, 1) + "}}";
    }
    j += "\n ]\n}\n";
    return j;
}

bool writeTex(const std::wstring& path, uint32_t w, uint32_t h, uint32_t format, uint32_t rowBytes, uint32_t frame, uint32_t eye,
              const uint8_t* rows) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const uint32_t header[9] = {1u, w, h, format, rowBytes, frame, eye, 0u, 0u};
    bool ok = std::fwrite("EDVRTEX1", 1, 8, f) == 8 && std::fwrite(header, sizeof header, 1, f) == 1;
    const size_t bytes = static_cast<size_t>(rowBytes) * h;
    ok = ok && (bytes == 0 || std::fwrite(rows, 1, bytes, f) == bytes);
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

Writer::~Writer() { stop(); }

void Writer::push(std::wstring path, uint32_t w, uint32_t h, uint32_t format, uint32_t rowBytes, uint32_t frame, uint32_t eye,
                  std::vector<uint8_t> data) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!thread_.joinable()) {
        quit_ = false;
        thread_ = std::thread([this] { run(); });
    }
    queued_ += data.size();
    Job job;
    job.path = std::move(path);
    job.w = w;
    job.h = h;
    job.format = format;
    job.rowBytes = rowBytes;
    job.frame = frame;
    job.eye = eye;
    job.data = std::move(data);
    jobs_.push_back(std::move(job));
    cv_.notify_one();
}

uint64_t Writer::queuedBytes() {
    std::lock_guard<std::mutex> lock(mu_);
    return queued_;
}

bool Writer::idle() {
    std::lock_guard<std::mutex> lock(mu_);
    return jobs_.empty() && !busy_;
}

void Writer::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!thread_.joinable()) return;
        quit_ = true;
        cv_.notify_one();
    }
    thread_.join();
}

void Writer::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (jobs_.empty()) return;   // quit, and the queue drained
            job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }
        const bool ok = writeTex(job.path, job.w, job.h, job.format, job.rowBytes, job.frame, job.eye, job.data.data());
        if (ok) {
            ++written_;
            bytes_ += 44ull + job.data.size();
        } else {
            ++failed_;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            queued_ -= job.data.size();
            busy_ = false;
        }
    }
}

}  // namespace hull_bench
}  // namespace edvr
