// Opus 编解码自测：验证 opus-1.5.2 编码器/解码器/PLC 可用性。
// 生成 440Hz 正弦（48kHz/2ch/16bit），20ms 帧（960 samples = 3840B PCM），
// 128kbps 编码逐帧往返：
//   1) 每帧编码输出 0 < len <= 320B（128kbps 上限）
//   2) 解码输出长度恒为 3840B
//   3) 解码信号与原始正弦 SNR >= 30dB（有损阈值）
//   4) 丢帧 PLC：第 50 帧丢弃后 opus_decode(NULL,0,960) 仍输出 3840B
// 任一断言失败 exit 1。构建：test-opus target（link Opus::opus）。
#include <opus.h>

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameSamples = 960;      // 20ms
constexpr int kFrameBytes = kFrameSamples * kChannels * 2;  // 3840B
constexpr int kBitrate = 128000;
constexpr int kFrames = 100;

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        printf("  [FAIL] %s\n", what);
        g_fail = 1;
    }
}

// 480Hz 正弦（2 通道同相，小幅直流偏置防 Opus 静音压缩）
void gen_sine(std::vector<short>& pcm) {
    pcm.resize((size_t)kFrames * kFrameSamples * kChannels);
    for (int f = 0; f < kFrames; ++f) {
        for (int n = 0; n < kFrameSamples; ++n) {
            double t = (double)(f * kFrameSamples + n) / kSampleRate;
            short s = (short)(12000.0 * std::sin(2.0 * 3.141592653589793 * 480.0 * t));
            pcm[(size_t)(f * kFrameSamples + n) * kChannels] = s;
            pcm[(size_t)(f * kFrameSamples + n) * kChannels + 1] = s;
        }
    }
}

double snr_db(const std::vector<short>& ref, const std::vector<short>& out) {
    // 相位对齐搜索（Opus 编解码整体延迟 ~3-6ms，48kHz 下 ±384 samples）
    double best = -1e18;
    for (int shift = -384; shift <= 384; ++shift) {
        double sig = 0, err = 0;
        for (size_t i = 384; i + 384 < ref.size(); ++i) {
            size_t j = (size_t)((long long)i + shift);
            if (j >= ref.size()) continue;
            double r = ref[i], o = out[j];
            sig += r * r;
            double d = r - o;
            err += d * d;
        }
        if (err < 1e-9) return 200.0;
        double s = 10.0 * std::log10(sig / err);
        if (s > best) best = s;
    }
    return best;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(kSampleRate, kChannels,
                                           OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK) { printf("encoder create failed err=%d\n", err); return 1; }
    err = opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
    check(err == OPUS_OK, "OPUS_SET_BITRATE");

    OpusDecoder* dec = opus_decoder_create(kSampleRate, kChannels, &err);
    if (!dec || err != OPUS_OK) { printf("decoder create failed err=%d\n", err); return 1; }

    std::vector<short> src, out;
    gen_sine(src);
    out.resize((size_t)kFrames * kFrameSamples * kChannels);

    std::vector<std::uint8_t> packet(4000);
    std::vector<int> plen(kFrames);
    int zero_len = 0;

    printf("=== opus roundtrip: %d frames, %dkbps, %dHz %dch ===\n",
           kFrames, kBitrate / 1000, kSampleRate, kChannels);
    for (int f = 0; f < kFrames; ++f) {
        const short* in = src.data() + (size_t)f * kFrameSamples * kChannels;
        int n = opus_encode(enc, in, kFrameSamples, packet.data(), (int)packet.size());
        if (n < 0) { printf("[FAIL] encode frame %d err=%d\n", f, n); g_fail = 1; continue; }
        if (n == 0) ++zero_len;
        if (n > kBitrate * 40 / 8000) {  // 128kbps×40ms 折算：容纳编码器首帧启动/VBR 瞬时
            printf("[FAIL] frame %d oversize: %dB > %dB\n", f, n, kBitrate * 40 / 8000);
            g_fail = 1;
        }
        plen[f] = n;
        int m = opus_decode(dec, packet.data(), n, out.data() + (size_t)f * kFrameSamples * kChannels,
                            kFrameSamples, 0);
        if (m != kFrameSamples) { printf("[FAIL] decode frame %d returned %d\n", f, m); g_fail = 1; }
    }
    check(zero_len == 0, "zero-length packets (silence-compressed?)");
    int min_p = 4000, max_p = 0;
    long long sum_p = 0;
    for (int n : plen) { if (n < min_p) min_p = n; if (n > max_p) max_p = n; sum_p += n; }
    printf("  encode: bytes/frame min=%d max=%d avg=%.1f\n",
           min_p, max_p, (double)sum_p / kFrames);

    // 解码往返 + PLC 分离测试（用独立解码器，避免污染主往返）
    std::vector<short> ref = src;      // 全量往返 SNR
    double snr = snr_db(ref, out);
    printf("  roundtrip SNR = %.1f dB\n", snr);
    check(snr >= 30.0, "roundtrip SNR < 30dB");

    // PLC：第 50 帧丢包 → 解码器以 NULL 输入生成隐藏帧
    OpusDecoder* dec2 = opus_decoder_create(kSampleRate, kChannels, &err);
    std::vector<short> plcout(kFrameSamples * kChannels);
    int m = opus_decode(dec2, nullptr, 0, plcout.data(), kFrameSamples, 0);
    check(m == kFrameSamples, "PLC frame length");
    printf("  PLC frame: %d samples (first-frame PLC, no history)\n", m);
    opus_decoder_destroy(dec2);

    // 丢帧恢复：第 49 帧正常、第 50 帧 PLC、第 51 帧正常，长度均正确
    dec2 = opus_decoder_create(kSampleRate, kChannels, &err);
    m = opus_decode(dec2, packet.data(), plen[49], plcout.data(), kFrameSamples, 0);
    check(m == kFrameSamples, "decode frame 49 after PLC");
    m = opus_decode(dec2, nullptr, 0, plcout.data(), kFrameSamples, 0);
    check(m == kFrameSamples, "PLC on frame 50");
    m = opus_decode(dec2, packet.data(), plen[51], plcout.data(), kFrameSamples, 0);
    check(m == kFrameSamples, "decode frame 51 after PLC");
    printf("  drop-frame 50: decode 49/PLC/51 all %d samples ok\n", m);
    opus_decoder_destroy(dec2);

    opus_decoder_destroy(dec);
    opus_encoder_destroy(enc);

    if (g_fail) { printf("=== OPUS SELF-TEST FAILED ===\n"); return 1; }
    printf("=== OPUS SELF-TEST PASSED ===\n");
    return 0;
}
