#pragma once

// tight 媒体通道：媒体分片封装格式。
//
// 发送端把编码后的 H.264（视频）与 PCM（音频，免编码直传）切成一帧一帧
// 的媒体分片，经 tight 消息发送；接收端按此格式还原并送解码/播放。
// 视频走 tight 通道 0，音频走 tight 通道 1（可独立设置 FEC 冗余），
// 分片内用 type 字段区分。

#include <cstdint>
#include <cstring>
#include <vector>

namespace media {

enum : std::uint8_t {
    kTypeVideo = 0,
    kTypeAudio = 1,
};

// 音频通道：Opus 编码（48kHz 双声道，20ms 帧 = 960 samples）。
// 发送端 SourceReader 解码 AAC → PCM → Opus 编码后发送；接收端 Opus
// 解码 → PCM（16-bit 有符号交错）→ waveOut 播放。128kbps → 平均 320B/
// 帧（VBR 瞬时可达 496B，首帧启动开销最大），加 22B 媒体头仍为单分片。
inline constexpr std::uint32_t kAudioSampleRate = 48000;
inline constexpr std::uint16_t kAudioChannels = 2;
inline constexpr std::uint16_t kAudioBits = 16;         // 解码后 PCM 位深
inline constexpr std::uint32_t kAudioFrameMs = 20;      // 帧时长
inline constexpr std::uint32_t kOpusBitrate = 128000;   // 128kbps（20ms≈320B）
inline constexpr std::uint32_t kAudioFrameSamples = kAudioSampleRate * kAudioFrameMs / 1000;  // 960
inline constexpr std::uint32_t kAudioFrameBytes = kAudioFrameSamples * kAudioChannels * (kAudioBits / 8);  // 3840B

// 分片头：magic(4) + type(1) + flags(1) + seq(4) + pts_ms(8) + size(4)
inline constexpr std::uint32_t kChunkMagic = 0x4149444D;  // 'MDIA'
inline constexpr std::uint8_t kFlagKeyframe = 0x01;

struct ChunkHeader {
    std::uint32_t magic;
    std::uint8_t type;
    std::uint8_t flags;
    std::uint32_t seq;      // 帧序号（视频/音频各自独立递增），接收端据此丢弃乱序/重复帧
    std::uint64_t pts_ms;
    std::uint32_t size;
};
inline constexpr std::size_t kHeaderSize = 4 + 1 + 1 + 4 + 8 + 4;

inline bool is_media_chunk(const std::uint8_t* p, std::size_t n) {
    if (n < kHeaderSize) return false;
    std::uint32_t m;
    std::memcpy(&m, p, 4);
    return m == kChunkMagic;
}

inline ChunkHeader parse_header(const std::uint8_t* p) {
    ChunkHeader h{};
    std::memcpy(&h.magic, p, 4);
    h.type = p[4];
    h.flags = p[5];
    std::memcpy(&h.seq, p + 6, 4);
    std::memcpy(&h.pts_ms, p + 10, 8);
    std::memcpy(&h.size, p + 18, 4);
    return h;
}

inline std::vector<std::uint8_t> pack_chunk(std::uint8_t type, std::uint8_t flags,
                                            std::uint32_t seq, std::uint64_t pts_ms,
                                            const std::uint8_t* data, std::uint32_t size) {
    std::vector<std::uint8_t> out(kHeaderSize + size);
    std::uint32_t magic = kChunkMagic;
    std::memcpy(out.data(), &magic, 4);
    out[4] = type;
    out[5] = flags;
    std::memcpy(out.data() + 6, &seq, 4);
    std::memcpy(out.data() + 10, &pts_ms, 8);
    std::memcpy(out.data() + 18, &size, 4);
    if (size) std::memcpy(out.data() + kHeaderSize, data, size);
    return out;
}

}  // namespace media
