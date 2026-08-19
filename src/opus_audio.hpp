#pragma once

// Opus 编解码封装（音频通道 1）：PCM(48kHz/2ch/16bit 交错) ↔ Opus 码流。
// 帧长 20ms（960 samples，3840B PCM）。解码端支持丢帧隐藏（PLC）：
// decode(nullptr, 0, samples) 生成一帧隐藏帧，供播放端欠载/丢包时顶替。
// 封装为无锁单线程使用（发送端 1 线程 / 接收端 1 线程），非线程安全。

#include <cstdint>
#include <vector>

namespace audio {

class OpusEncoder {
public:
    OpusEncoder();
    ~OpusEncoder();
    OpusEncoder(const OpusEncoder&) = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;

    // 48kHz / 2ch / 20ms / bitrate bps。失败返回 false（编码器不可用时
    // 应用层降级为静音/不发送，不应崩溃）。
    bool init(std::uint32_t bitrate = 128000);
    // 输入 3840B（960 samples × 2ch × 2B）交错 PCM → Opus 帧。
    // 返回编码字节数，失败返回 0。输出缓冲足够容纳最大 Opus 帧。
    int encode(const std::int16_t* pcm, std::uint8_t* out, std::size_t out_cap);

private:
    void* m_enc = nullptr;  // OpusEncoder*
};

class OpusDecoder {
public:
    OpusDecoder();
    ~OpusDecoder();
    OpusDecoder(const OpusDecoder&) = delete;
    OpusDecoder& operator=(const OpusDecoder&) = delete;

    bool init();
    // 解码一帧（960 samples）。opus 为 nullptr 或 len==0 时生成 PLC
    // 隐藏帧。out 容量 ≥ 3840B。返回解码样本数（960 或负错误码）。
    int decode(const std::uint8_t* opus, int len, std::int16_t* out);

private:
    void* m_dec = nullptr;  // OpusDecoder*
};

}  // namespace audio
