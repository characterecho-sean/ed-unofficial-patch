#pragma once
// Frame generation replay capture, the pure half (fix.frame_generation_shadow, TEMPORARY; .agents\framegen-replay-format.md): SHA-256, the DXGI formats the capture
// knows (name and bytes per texel), and the two JSON documents (one per captured frame, and the manifest). The writer is src\d3d11\framegen_synth.cpp; the reader is
// tools\framegen_synth_bench. Nothing here touches D3D or the disk.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace edvr {

constexpr uint32_t kFrameGenReplayFrames = 6;   // consecutive real frames of the left eye

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h_, init, sizeof(h_));
        len_ = 0;
        fill_ = 0;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len_ += n;
        while (n) {
            const size_t take = (64 - fill_) < n ? (64 - fill_) : n;
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = static_cast<uint64_t>(len_) * 8u;
        const uint8_t one = 0x80, zero = 0;
        Sha256 keep = *this;   // padding must not disturb a copy the caller keeps feeding; this object is finished here
        keep.update(&one, 1);
        while (keep.fill_ != 56) keep.update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        keep.update(lenBytes, 8);
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i)
            for (int b = 3; b >= 0; --b) { const uint8_t v = static_cast<uint8_t>(keep.h_[i] >> (8 * b)); out += digits[v >> 4]; out += digits[v & 15]; }
        return out;
    }
private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* b) {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u,
            0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du,
            0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu,
            0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = static_cast<uint32_t>(b[4 * i]) << 24 | static_cast<uint32_t>(b[4 * i + 1]) << 16 | static_cast<uint32_t>(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t fill_;
    uint64_t len_;
};

// The DXGI formats a capture can hold (the upscaler's output, the depth copy and the vectors): name and bytes per texel. Bytes 0 = not supported (the capture stops, and says so).
struct FrameGenReplayFormat { uint32_t id; const char* name; uint32_t bytes; };
inline const FrameGenReplayFormat* frameGenReplayFormat(uint32_t f) {
    static const FrameGenReplayFormat table[] = {
        {1, "R32G32B32A32_TYPELESS", 16}, {2, "R32G32B32A32_FLOAT", 16}, {5, "R32G32B32_TYPELESS", 12}, {6, "R32G32B32_FLOAT", 12},
        {9, "R16G16B16A16_TYPELESS", 8}, {10, "R16G16B16A16_FLOAT", 8}, {11, "R16G16B16A16_UNORM", 8},
        {15, "R32G32_TYPELESS", 8}, {16, "R32G32_FLOAT", 8},
        {23, "R10G10B10A2_TYPELESS", 4}, {24, "R10G10B10A2_UNORM", 4}, {26, "R11G11B10_FLOAT", 4},
        {27, "R8G8B8A8_TYPELESS", 4}, {28, "R8G8B8A8_UNORM", 4}, {29, "R8G8B8A8_UNORM_SRGB", 4},
        {33, "R16G16_TYPELESS", 4}, {34, "R16G16_FLOAT", 4}, {35, "R16G16_UNORM", 4}, {37, "R16G16_SNORM", 4},
        {39, "R32_TYPELESS", 4}, {40, "D32_FLOAT", 4}, {41, "R32_FLOAT", 4}, {42, "R32_UINT", 4},
        {53, "R16_TYPELESS", 2}, {54, "R16_FLOAT", 2}, {56, "R16_UNORM", 2}, {60, "R8_TYPELESS", 1}, {61, "R8_UNORM", 1},
        {87, "B8G8R8A8_UNORM", 4}, {91, "B8G8R8A8_UNORM_SRGB", 4}};
    for (const FrameGenReplayFormat& e : table) if (e.id == f) return &e;
    return nullptr;
}
inline uint32_t frameGenReplayFormatBytes(uint32_t f) { const FrameGenReplayFormat* e = frameGenReplayFormat(f); return e ? e->bytes : 0; }
inline const char* frameGenReplayFormatName(uint32_t f) { const FrameGenReplayFormat* e = frameGenReplayFormat(f); return e ? e->name : "UNKNOWN"; }

struct FrameGenReplayFrame {
    uint32_t index = 0;
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    uint32_t outFormat = 0, depthFormat = 0, mvFormat = 0;   // DXGI_FORMAT values
    float tanNow[4] = {};
    bool haveTanPrev = false;
    float tanPrev[4] = {};
    float depthA = 0.0f, depthB = 0.0f, splitMetres = 0.0f, f = 0.0f;
    float Hd[9] = {}, th[3] = {}, W[9] = {}, tvCam[3] = {};
    bool worldValid = false, cameraJump = false;
    uint32_t passFrame = 0;
    int64_t qpc = 0, qpcFrequency = 0;
};

namespace replay_detail {
inline void num(std::string& s, float v) { char b[40]; std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v)); s += b; }
inline void arr(std::string& s, const float* v, int n) { s += '['; for (int i = 0; i < n; ++i) { if (i) s += ", "; num(s, v[i]); } s += ']'; }
inline void u(std::string& s, uint64_t v) { char b[32]; std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(v)); s += b; }
inline void fmt(std::string& s, const char* key, uint32_t f) { s += "\"" ; s += key; s += "\": "; u(s, f); s += ", \""; s += key; s += "_name\": \""; s += frameGenReplayFormatName(f); s += "\", "; }
inline std::string esc(const std::string& in) {
    std::string o;
    for (char c : in) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (static_cast<unsigned char>(c) >= 0x20) o += c; }
    return o;
}
}  // namespace replay_detail

// f<k>.json: everything the synthesizer's constant block is built from for the frame (frameGenSynthFillCb's inputs), as numbers. Key names are the bench's (tools\framegen_synth_bench header); variant is 0 (f = 0.5) or 1 (f = 1).
inline std::string frameGenReplayFrameJson(const FrameGenReplayFrame& r) {
    using namespace replay_detail;
    std::string s = "{\n  \"schema\": 1, \"index\": ";
    u(s, r.index);
    s += ",\n  \"render_w\": "; u(s, r.renderW); s += ", \"render_h\": "; u(s, r.renderH);
    s += ", \"out_w\": "; u(s, r.outW); s += ", \"out_h\": "; u(s, r.outH);
    s += ",\n  ";
    fmt(s, "out_format", r.outFormat); fmt(s, "depth_format", r.depthFormat); fmt(s, "mv_format", r.mvFormat);
    s += "\n  \"tan_now\": "; arr(s, r.tanNow, 4);
    s += ",\n  \"tan_prev\": "; if (r.haveTanPrev) arr(s, r.tanPrev, 4); else s += "null";
    s += ",\n  \"depth_a\": "; num(s, r.depthA); s += ", \"depth_b\": "; num(s, r.depthB);
    s += ", \"split_metres\": "; num(s, r.splitMetres);
    s += ",\n  \"f\": "; num(s, r.f); s += r.f < 0.75f ? ", \"variant\": 0, \"variant_name\": \"half\"" : ", \"variant\": 1, \"variant_name\": \"full\"";   // the bench reads variant as a number: 0 the f = 0.5 S, 1 the f = 1 S
    s += ",\n  \"hd\": "; arr(s, r.Hd, 9);
    s += ",\n  \"th\": "; arr(s, r.th, 3);
    s += ",\n  \"world_w\": "; arr(s, r.W, 9);
    s += ",\n  \"tv_cam\": "; arr(s, r.tvCam, 3);
    s += ",\n  \"world_valid\": "; s += r.worldValid ? "true" : "false";
    s += ", \"camera_jump\": "; s += r.cameraJump ? "true" : "false";
    s += ",\n  \"frame\": "; u(s, r.passFrame);
    s += ", \"qpc\": "; { char b[32]; std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(r.qpc)); s += b; }
    s += ", \"qpc_freq\": "; { char b[32]; std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(r.qpcFrequency)); s += b; }
    s += "\n}\n";
    return s;
}

struct FrameGenReplayFile {
    std::string name;
    uint64_t bytes = 0;
    std::string sha256;
};

// manifest.json. stopReason empty = the run reached kFrameGenReplayFrames frames.
inline std::string frameGenReplayManifestJson(const std::string& stamp, const std::string& build, uint32_t frames, const std::string& stopReason, const std::vector<FrameGenReplayFile>& files) {
    using namespace replay_detail;
    std::string s = "{\n  \"schema\": 1,\n  \"stamp\": \"" + esc(stamp) + "\",\n  \"build\": \"" + esc(build) + "\",\n  \"eye\": \"left\",\n  \"frames\": ";
    u(s, frames);
    s += ",\n  \"frames_requested\": "; u(s, kFrameGenReplayFrames);
    s += ",\n  \"stopped_reason\": "; if (stopReason.empty()) s += "null"; else s += "\"" + esc(stopReason) + "\"";
    s += ",\n  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        s += "    {\"name\": \"" + esc(files[i].name) + "\", \"bytes\": "; u(s, files[i].bytes);
        s += ", \"sha256\": \"" + files[i].sha256 + "\"}"; s += i + 1 < files.size() ? ",\n" : "\n";
    }
    s += "  ]\n}\n";
    return s;
}

}  // namespace edvr
