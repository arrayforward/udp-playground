#pragma once

// AAC 音频压缩编解码封装（Media Foundation，Windows 内置）。
// 发送端 MF 解码 mp4 的 AAC → PCM，再用 MF AAC 编码器编码为 ADTS 后发送；
// 接收端 MF AAC 解码器解码 ADTS → PCM，waveOut 播放。音频固定码率。

#include "media_chunk.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace media {

// MinGW 头文件缺 AAC 编码器 MFT 的 CLSID，手动定义
static const GUID kClsidAacEncMFT = {0x93af0c51, 0x2275, 0x45d2,
                                     {0xa3, 0x5b, 0xf2, 0xba, 0x21, 0xca, 0xed, 0x00}};

class AacEncoder {
public:
    ~AacEncoder() { mft_.Reset(); }

    bool init(int sample_rate, int channels, int bitrate_bps) {
        HRESULT hr = CoCreateInstance(kClsidAacEncMFT, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&mft_));
        if (FAILED(hr)) { printf("[aac] enc create failed 0x%08x\n", (unsigned)hr); return false; }

        // 先设输出类型（AAC），再设输入（PCM）——顺序与 H.264 编码器一致
        ComPtr<IMFMediaType> out_type;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
            GUID sub = GUID_NULL;
            if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFAudioFormat_AAC) {
                out_type = t;
                break;
            }
        }
        if (!out_type) { printf("[aac] enc no AAC output type\n"); return false; }
        out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate_bps / 8);
        hr = mft_->SetOutputType(0, out_type.Get(), 0);
        if (FAILED(hr)) { printf("[aac] enc SetOutputType failed 0x%08x\n", (unsigned)hr); return false; }

        ComPtr<IMFMediaType> in_type;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetInputAvailableType(0, i, &t))) break;
            GUID sub = GUID_NULL;
            if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub))) {
                printf("[aac] enc in[%u] sub=%08x-%04x-%04x\n", i, sub.Data1, sub.Data2, sub.Data3);
            }
            if (sub == MFAudioFormat_PCM) {
                in_type = t;
                break;
            }
        }
        if (!in_type) {
            MFCreateMediaType(&in_type);
            in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        }
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * channels * 2);
        hr = mft_->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) { printf("[aac] enc SetInputType failed 0x%08x\n", (unsigned)hr); return false; }

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // 输入 PCM IMFSample，输出 AAC（ADTS）字节帧
    bool encode(IMFSample* in, std::vector<std::vector<std::uint8_t>>& out) {
        HRESULT hr = mft_->ProcessInput(0, in, 0);
        if (hr == MF_E_NOTACCEPTING) { drain(out); hr = mft_->ProcessInput(0, in, 0); }
        if (FAILED(hr)) return false;
        return drain(out);
    }

private:
    bool drain(std::vector<std::vector<std::uint8_t>>& out) {
        for (;;) {
            DWORD status = 0;
            MFT_OUTPUT_DATA_BUFFER ob{};
            ComPtr<IMFSample> s;
            MFCreateSample(&s);
            ComPtr<IMFMediaBuffer> mb;
            MFCreateMemoryBuffer(8192, &mb);
            s->AddBuffer(mb.Get());
            ob.pSample = s.Get();
            HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(hr)) return false;
            ComPtr<IMFMediaBuffer> cmb;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&cmb))) {
                BYTE* p = nullptr; DWORD cur = 0;
                if (SUCCEEDED(cmb->Lock(&p, nullptr, &cur))) {
                    out.emplace_back(p, p + cur);
                    cmb->Unlock();
                }
            }
        }
    }

    ComPtr<IMFTransform> mft_;
};

class AacDecoder {
public:
    ~AacDecoder() { mft_.Reset(); }

    bool init(int sample_rate, int channels) {
        HRESULT hr = CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&mft_));
        if (FAILED(hr)) { printf("[aac] dec create failed 0x%08x\n", (unsigned)hr); return false; }

        // 枚举 AAC 输入类型（解码器需要其自带的完整类型）
        ComPtr<IMFMediaType> in_type;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetInputAvailableType(0, i, &t))) break;
            GUID sub = GUID_NULL;
            if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFAudioFormat_AAC) {
                in_type = t;
                break;
            }
        }
        if (!in_type) {
            MFCreateMediaType(&in_type);
            in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        }
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        in_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);  // ADTS
        hr = mft_->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) { printf("[aac] dec SetInputType failed 0x%08x\n", (unsigned)hr); return false; }

        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
        out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = mft_->SetOutputType(0, out_type.Get(), 0);
        if (FAILED(hr)) { printf("[aac] dec SetOutputType failed 0x%08x\n", (unsigned)hr); return false; }
        return true;
    }

    // 输入 AAC（ADTS）字节帧，输出 PCM 16-bit 交错
    bool decode(const std::uint8_t* aac, std::size_t len, std::vector<std::uint8_t>& pcm) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer((DWORD)len, &buf))) return false;
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
        std::memcpy(dst, aac, len);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)len);

        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());

        HRESULT hr = mft_->ProcessInput(0, s.Get(), 0);
        if (FAILED(hr)) return false;

        pcm.clear();
        for (;;) {
            DWORD status = 0;
            MFT_OUTPUT_DATA_BUFFER ob{};
            ComPtr<IMFSample> os;
            MFCreateSample(&os);
            ComPtr<IMFMediaBuffer> omb;
            MFCreateMemoryBuffer(16384, &omb);
            os->AddBuffer(omb.Get());
            ob.pSample = os.Get();
            hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return !pcm.empty();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
            if (FAILED(hr)) return false;
            ComPtr<IMFMediaBuffer> cmb;
            if (SUCCEEDED(os->ConvertToContiguousBuffer(&cmb))) {
                BYTE* p = nullptr; DWORD cur = 0;
                if (SUCCEEDED(cmb->Lock(&p, nullptr, &cur))) {
                    pcm.insert(pcm.end(), p, p + cur);
                    cmb->Unlock();
                }
            }
        }
    }

private:
    ComPtr<IMFTransform> mft_;
};

}  // namespace media
