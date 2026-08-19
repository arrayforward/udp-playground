#pragma once

// Opus Custom Mode（纯 CELT）音频压缩编解码。标准 Opus（SILK+CELT 混合）在
// MinGW 下编解码失真，Custom Mode 只用 CELT，参考 opus_custom_demo.c。
// 发送端 MF 解码 AAC → PCM → Opus Custom 编码发送；接收端 Custom 解码 → PCM。

#include "media_chunk.hpp"

#include <opus_custom.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace media {

class AudioEncoder {
public:
    ~AudioEncoder() { destroy(); }

    bool init(int sample_rate, int channels, int bitrate) {
        int err = 0;
        frame_samples_ = sample_rate / 1000 * (int)kAudioFrameMs;  // 20ms
        frame_bytes_ = (size_t)frame_samples_ * channels * 2;
        bytes_per_packet_ = bitrate / 8 * (int)kAudioFrameMs / 1000;  // 128kbps*20ms=320B
        if (bytes_per_packet_ < 32) bytes_per_packet_ = 32;
        mode_ = ::opus_custom_mode_create(sample_rate, frame_samples_, &err);
        if (err != OPUS_OK || !mode_) return false;
        enc_ = ::opus_custom_encoder_create(mode_, channels, &err);
        if (err != OPUS_OK || !enc_) return false;
        ::opus_custom_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
        return true;
    }

    // 输入 PCM 字节流（16-bit 有符号交错），内部按 20ms 帧缓冲，凑满编码一帧
    void feed(const std::uint8_t* pcm, std::size_t len,
              std::vector<std::vector<std::uint8_t>>& out_frames) {
        buf_.insert(buf_.end(), pcm, pcm + len);
        while (buf_.size() >= frame_bytes_) {
            std::vector<std::uint8_t> frame(1275);
            int n = ::opus_custom_encode(enc_,
                                         reinterpret_cast<const opus_int16*>(buf_.data()),
                                         frame_samples_, frame.data(), bytes_per_packet_);
            if (n > 0) {
                frame.resize((size_t)n);
                out_frames.push_back(std::move(frame));
            }
            buf_.erase(buf_.begin(), buf_.begin() + frame_bytes_);
        }
    }

private:
    void destroy() {
        if (enc_) ::opus_custom_encoder_destroy(enc_);
        if (mode_) ::opus_custom_mode_destroy(mode_);
        enc_ = nullptr;
        mode_ = nullptr;
    }
    ::OpusCustomMode* mode_ = nullptr;
    ::OpusCustomEncoder* enc_ = nullptr;
    int frame_samples_ = 0;
    int bytes_per_packet_ = 0;
    std::size_t frame_bytes_ = 0;
    std::vector<std::uint8_t> buf_;
};

class AudioDecoder {
public:
    ~AudioDecoder() {
        if (dec_) ::opus_custom_decoder_destroy(dec_);
        if (mode_) ::opus_custom_mode_destroy(mode_);
        dec_ = nullptr;
        mode_ = nullptr;
    }

    bool init(int sample_rate, int channels) {
        int err = 0;
        frame_samples_ = sample_rate / 1000 * (int)kAudioFrameMs;
        frame_bytes_ = (size_t)frame_samples_ * channels * 2;
        mode_ = ::opus_custom_mode_create(sample_rate, frame_samples_, &err);
        if (err != OPUS_OK || !mode_) return false;
        dec_ = ::opus_custom_decoder_create(mode_, channels, &err);
        if (err != OPUS_OK || !dec_) return false;
        return true;
    }

    // 解码一帧 Opus Custom → PCM（16-bit 有符号交错）
    bool decode(const std::uint8_t* data, std::size_t len, std::vector<std::uint8_t>& pcm) {
        pcm.resize(frame_bytes_);
        int n = ::opus_custom_decode(dec_, data, (opus_int32)len,
                                     reinterpret_cast<opus_int16*>(pcm.data()),
                                     frame_samples_);
        if (n < 0) { pcm.clear(); return false; }
        pcm.resize((size_t)n * 2 * kAudioChannels);
        return true;
    }

private:
    ::OpusCustomMode* mode_ = nullptr;
    ::OpusCustomDecoder* dec_ = nullptr;
    int frame_samples_ = 0;
    std::size_t frame_bytes_ = 0;
};

}  // namespace media
