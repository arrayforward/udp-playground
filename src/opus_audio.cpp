#include "opus_audio.hpp"

#include <opus.h>

#include <cstdio>

namespace audio {

namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameSamples = 960;  // 20ms
}  // namespace

OpusEncoder::OpusEncoder() = default;
OpusEncoder::~OpusEncoder() {
    if (m_enc) opus_encoder_destroy(static_cast<::OpusEncoder*>(m_enc));
    m_enc = nullptr;
}

bool OpusEncoder::init(std::uint32_t bitrate) {
    if (m_enc) return true;
    int err = 0;
    ::OpusEncoder* e = opus_encoder_create(kSampleRate, kChannels,
                                           OPUS_APPLICATION_AUDIO, &err);
    if (!e || err != OPUS_OK) {
        if (e) opus_encoder_destroy(e);
        fprintf(stderr, "[audio] opus_encoder_create failed err=%d\n", err);
        return false;
    }
    if (opus_encoder_ctl(e, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate))) != OPUS_OK ||
        opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(6)) != OPUS_OK) {
        opus_encoder_destroy(e);
        fprintf(stderr, "[audio] opus encoder ctl failed\n");
        return false;
    }
    m_enc = e;
    return true;
}

int OpusEncoder::encode(const std::int16_t* pcm, std::uint8_t* out, std::size_t out_cap) {
    if (!m_enc || !pcm || !out || out_cap == 0) return 0;
    int n = opus_encode(static_cast<::OpusEncoder*>(m_enc), pcm, kFrameSamples,
                        out, static_cast<int>(out_cap));
    return n > 0 ? n : 0;
}

OpusDecoder::OpusDecoder() = default;
OpusDecoder::~OpusDecoder() {
    if (m_dec) opus_decoder_destroy(static_cast<::OpusDecoder*>(m_dec));
    m_dec = nullptr;
}

bool OpusDecoder::init() {
    if (m_dec) return true;
    int err = 0;
    ::OpusDecoder* d = opus_decoder_create(kSampleRate, kChannels, &err);
    if (!d || err != OPUS_OK) {
        if (d) opus_decoder_destroy(d);
        fprintf(stderr, "[audio] opus_decoder_create failed err=%d\n", err);
        return false;
    }
    m_dec = d;
    return true;
}

int OpusDecoder::decode(const std::uint8_t* opus, int len, std::int16_t* out) {
    if (!m_dec || !out) return -1;
    // opus 为空 / len==0 �?PLC 隐藏帧（丢包隐藏，播放端欠载顶替用）
    return opus_decode(static_cast<::OpusDecoder*>(m_dec), opus, opus ? len : 0,
                       out, kFrameSamples, 0);
}

}  // namespace audio
