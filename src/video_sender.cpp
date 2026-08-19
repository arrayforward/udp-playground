// video_sender：Media Foundation 解码 cold.mp4（循环）�?�?tight 带宽估计
// 动态码�?H.264 硬编（Intel QSV，ffmpeg.exe h264_qsv 子进程，通道 0）；
// 音频：ffmpeg.exe 子进程解码音�?�?PCM �?Opus 编码（通道 1）�?//
// 角色：node（服务端），绑定端口，向发现�?leaf 推送媒体分片�?// 视频编码器：QSV 硬编（硬�?RC 码率收敛 ~1 帧，对比 MF 软编 set_bitrate
// 需 ~1-2s 收敛——弱网排空恢复期超发循环的根源）。码率变�?强制关键�?// = 重启子进程（新进程首帧即 IDR，天然获得关键帧）�?
#include "tight/tight.hpp"
#include "media_chunk.hpp"
#include "opus_audio.hpp"
#include "qsv_video.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

using Microsoft::WRL::ComPtr;
using namespace std::chrono;

namespace {

constexpr const wchar_t* kMediaPath = L"D:\\tools\\udp-sim\\media\\cold.mp4";
constexpr const char* kMediaPathUtf8 = "D:\\tools\\udp-sim\\media\\cold.mp4";
// 编码分辨�?720p（QSV 硬编，CPU 余量保音频实时）
constexpr UINT32 kVideoW = 1280;
constexpr UINT32 kVideoH = 720;
UINT32 kSrcW = 1920;   // 源视频分辨率（运行时从原生类型读取）
UINT32 kSrcH = 1080;

// 读取 sample �?payload �?buffer
std::vector<std::uint8_t> sample_to_bytes(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    std::vector<std::uint8_t> out;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
        BYTE* data = nullptr;
        DWORD max = 0, cur = 0;
        if (SUCCEEDED(buffer->Lock(&data, &max, &cur))) {
            out.assign(data, data + cur);
            buffer->Unlock();
        }
    }
    return out;
}

bool sample_is_keyframe(IMFSample* sample) {
    UINT32 cp = 0;
    return SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cp)) && cp != 0;
}

// NV12 双线性缩放：Y 平面 + 交错 UV 平面�?U/V 分别缩放，再交错写回
void scale_nv12(const std::uint8_t* src, int sw, int sh,
                std::uint8_t* dst, int dw, int dh) {
    auto bilinear_plane = [](const std::uint8_t* s, int sw, int sh,
                             std::uint8_t* d, int dw, int dh) {
        for (int y = 0; y < dh; ++y) {
            double sy = (y + 0.5) * sh / dh - 0.5;
            int y0 = sy < 0 ? 0 : (int)sy;
            int y1 = y0 + 1; if (y1 >= sh) y1 = sh - 1;
            double fy = sy - y0;
            for (int x = 0; x < dw; ++x) {
                double sx = (x + 0.5) * sw / dw - 0.5;
                int x0 = sx < 0 ? 0 : (int)sx;
                int x1 = x0 + 1; if (x1 >= sw) x1 = sw - 1;
                double fx = sx - x0;
                double v = s[y0 * sw + x0] * (1 - fx) * (1 - fy)
                         + s[y0 * sw + x1] * fx * (1 - fy)
                         + s[y1 * sw + x0] * (1 - fx) * fy
                         + s[y1 * sw + x1] * fx * fy;
                d[y * dw + x] = (std::uint8_t)(v + 0.5);
            }
        }
    };
    bilinear_plane(src, sw, sh, dst, dw, dh);
    int usw = sw / 2, ush = sh / 2;
    int udw = dw / 2, udh = dh / 2;
    const std::uint8_t* suv = src + (size_t)sw * sh;
    std::vector<std::uint8_t> u_src((size_t)usw * ush), v_src((size_t)usw * ush);
    for (int i = 0; i < usw * ush; ++i) { u_src[i] = suv[i * 2]; v_src[i] = suv[i * 2 + 1]; }
    std::vector<std::uint8_t> u_dst((size_t)udw * udh), v_dst((size_t)udw * udh);
    bilinear_plane(u_src.data(), usw, ush, u_dst.data(), udw, udh);
    bilinear_plane(v_src.data(), usw, ush, v_dst.data(), udw, udh);
    std::uint8_t* duv = dst + (size_t)dw * dh;
    for (int i = 0; i < udw * udh; ++i) { duv[i * 2] = u_dst[i]; duv[i * 2 + 1] = v_dst[i]; }
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::uint16_t port = 9999;
    int run_seconds = 0;
    bool video_off = false;  // --video-off：只发音频（验证音频通道 50fps 端到端）
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) run_seconds = atoi(argv[++i]);
        else if (a == "--video-off") video_off = true;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    tight::TightConfig cfg;
    cfg.bind = tight::NetAddress("0.0.0.0", port);
    cfg.id = "video-src";
    cfg.token = "tok";
    cfg.role = tight::LinkRole::Node;
    cfg.retransmit_enabled = false;  // 实时视频：纯 FEC 兜底，不重传
    cfg.lite_mode = true;   // lite 视频模式：标准 lite 队列 + FEC 关（丢帧由播放端 req-keyframe 兜底）
    cfg.lite_profile = tight::LiteProfile::Video;
    cfg.fec_enabled = false;  // 视频模式关闭 FEC：接收不再被 RS 解码拖慢（lite 视频可行的前提），省校验片内存/CPU
    cfg.report_interval = std::chrono::milliseconds(333);  // 1s 3 次，带宽估计收敛更快
    cfg.late_buffer_ms = 16;  // 迟到 buffer：视频 33ms 帧周期的一半
    cfg.channel_reliable[2] = true;  // file 通道：可靠 ARQ
    cfg.channel_reliable[3] = true;  // data 通道：可靠 ARQ + 去重
    // 音频编码码率固定预留（tight 计算视频可用码率时扣除；校验片开销按
    // channel_fec_extra[1] 设置自动叠加，未设置即不预留）
    cfg.audio_reserved_bps = media::kOpusBitrate;
    // 不设 channel_fec_extra 固定冗余：FEC 完全由 tight 协议自适应决策
    // （迟到率驱动分段冗余；RTT>200ms 自动关闭让出带宽）
    tight::TightTransport tx(cfg);
    std::mutex pid_mutex;
    std::string peer_id;
    std::atomic<int> force_idr_count{0};  // >0 时下一视频帧重启编码器（新 IDR）
    tx.set_peer_callback([&](const tight::PeerEvent& e) {
        if (e.state == tight::LinkState::Established || e.state == tight::LinkState::Online) {
            { std::lock_guard<std::mutex> lk(pid_mutex); peer_id = e.id; }
            force_idr_count = 1;  // 起步强制 IDR，确保播放端至少收到一个
            printf("[src] peer online: %s\n", e.id.c_str());
            fflush(stdout);
        }
    });
    // 接收�?FEC 重组失败时申请关键帧：置位让 worker 下一帧重启编码器
    tx.set_command_callback([&](const std::string&, tight::Bytes payload) {
        static const char kReq[] = "req-keyframe";
        if (payload.size() == sizeof(kReq) - 1 &&
            std::memcmp(payload.data(), kReq, sizeof(kReq) - 1) == 0) {
            force_idr_count.store(1);
            printf("[src] peer requested keyframe\n");
            fflush(stdout);
        }
    });
    if (!tx.start()) { fprintf(stderr, "[src] start failed\n"); return 1; }
    printf("[src] video source on %u\n", port);
    fflush(stdout);

    // H.264 硬编（Intel QSV）：ffmpeg.exe h264_qsv 子进程，码率变化即重启
    video::QsvVideoEncoder venc;
    if (!venc.start(kVideoW, kVideoH, 30, 3000 * 1000)) {
        fprintf(stderr, "[src] qsv encoder start failed\n");
        return 1;
    }

    // 音频编码：mp4 音轨 → ffmpeg 子进程解码 PCM → Opus 编码（通道 1）
    audio::OpusEncoder aenc;
    bool audio_ok = aenc.init(media::kOpusBitrate);
    if (!audio_ok) fprintf(stderr, "[src] opus encoder unavailable -> video only\n");

    // SourceReader 解码 mp4（视频流输出 NV12，音频流输出 PCM）
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(kMediaPath, nullptr, &reader);
    if (FAILED(hr)) { fprintf(stderr, "[src] open media failed 0x%08x\n", (unsigned)hr); return 1; }

    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &kSrcW, &kSrcH);
        UINT32 fn = 0, fd = 0;
        MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fn, &fd);
        printf("[src] native video %ux%u fps=%u/%u\n", kSrcW, kSrcH, fn, fd);
        fflush(stdout);
    }
    ComPtr<IMFMediaType> vtype;
    MFCreateMediaType(&vtype);
    vtype->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vtype->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(vtype.Get(), MF_MT_FRAME_SIZE, kSrcW, kSrcH);
    vtype->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(vtype.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    MFSetAttributeRatio(vtype.Get(), MF_MT_FRAME_RATE, 30, 1);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, vtype.Get());
    if (FAILED(hr)) { fprintf(stderr, "[src] set video type failed 0x%08x\n", (unsigned)hr); return 1; }

    ComPtr<IMFMediaType> atype;
    MFCreateMediaType(&atype);
    atype->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    atype->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    // 请求固定 PCM 格式，SourceReader 自动�?AAC 解码并重采样到该格式
    atype->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, media::kAudioSampleRate);
    atype->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, media::kAudioChannels);
    atype->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, media::kAudioBits);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, atype.Get());
    if (FAILED(hr)) { fprintf(stderr, "[src] set audio type failed 0x%08x\n", (unsigned)hr); return 1; }

    if (video_off) {
        printf("[src] audio-only mode\n");
        fflush(stdout);
    }

    {
        ComPtr<IMFMediaType> vt;
        if (SUCCEEDED(reader->GetCurrentMediaType(1, &vt))) {
            UINT32 sw = 0, sh = 0, stride = 0;
            MFGetAttributeSize(vt.Get(), MF_MT_FRAME_SIZE, &sw, &sh);
            vt->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
            printf("[src] video actual %ux%u stride=%u\n", sw, sh, stride);
        }
        ComPtr<IMFMediaType> at;
        if (SUCCEEDED(reader->GetCurrentMediaType(0, &at))) {
            UINT32 sr = 0, ch = 0, bits = 0;
            GUID sub = GUID_NULL;
            at->GetGUID(MF_MT_SUBTYPE, &sub);
            at->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
            at->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            at->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            printf("[src] audio actual sub=%08x rate=%u ch=%u bits=%u\n", sub.Data1, sr, ch, bits);
        }
        fflush(stdout);
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> sent_bytes{0}, sent_vframes{0};
    std::atomic<std::uint64_t> sent_vbytes_big{0}, sent_vframes_big{0};  // ≥512B 帧（avg 统计用，排除 SPS/PPS 碎片）
    std::atomic<std::uint64_t> sent_aframes{0}, sent_abytes{0};
    std::atomic<std::uint64_t> send_fail{0};        // 视频 send 失败（队列背压）
    std::atomic<std::uint64_t> audio_send_fail{0};  // 音频 send 失败（触发视频让路）
    std::atomic<std::uint64_t> video_in{0};
    std::atomic<std::uint64_t> target_bitrate{3000 * 1000};

    auto t0 = steady_clock::now();

    // 编码器重启冷却（编码线程 force_idr/respawn 与主循环码率重启共享）：
    // 高频重启（播放端 req-keyframe 每 500ms 一次 + 码率阶梯变化）会让 QSV
    // 设备崩溃（device failed -17，之后所有实例都失败 → 10-40s 断流实测）。
    // 冷却 ≥4s：码率重启与 IDR 请求共享，冷却期内 IDR 请求合并忽略。
    std::atomic<steady_clock::time_point> last_reset{
        steady_clock::now() - std::chrono::seconds(1)};  // 允许立即重置

    // 发送线程：读样本 → 视频缩放 → QSV 硬编 → 发送；音频样本忽略
    // （音频由独立 ffmpeg 子进程线程供给，避免 MF ANY_STREAM 排挤）。
    std::thread worker([&] {
        std::uint32_t vseq = 0;  // 视频帧序号（递增，接收端据此丢弃乱序帧）
        std::vector<std::uint8_t> scaled((size_t)kVideoW * kVideoH * 3 / 2);
        std::vector<std::uint8_t> au;   // QSV 输出 AU
        // 内容时钟节流：QSV 硬编远快于源帧率，若无节流 SourceReader 会
        // 快进（实测 46fps 读 > 30fps 内容 → 超发 → 积压排空 → 欠载）。
        // MF 软编时代编码慢（~40ms/帧）天然节流，换 QSV 后必须显式限速。
        auto next_video_tick = steady_clock::now();
        auto last_iter = steady_clock::now();
        while (!stop.load()) {
            auto iter_now = steady_clock::now();
            auto iter_gap = duration_cast<milliseconds>(iter_now - last_iter).count();
            if (iter_gap > 100) printf("[src] worker stall %lld ms\n", (long long)iter_gap);
            last_iter = iter_now;
            DWORD si = 0, flags = 0;
            LONGLONG pts = 0;
            ComPtr<IMFSample> sample;
            DWORD stream_idx = video_off ? MF_SOURCE_READER_FIRST_AUDIO_STREAM
                                         : MF_SOURCE_READER_ANY_STREAM;
            HRESULT r = reader->ReadSample(stream_idx, 0, &si, &flags, &pts, &sample);
            if (r == MF_E_INVALIDREQUEST) continue;
            if (r == MF_SOURCE_READERF_ENDOFSTREAM) {
                // 循环播放：seek �?0
                PROPVARIANT pos{}; pos.vt = VT_I8; pos.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pos);
                continue;
            }
            if (FAILED(r) || !sample) continue;

            std::string pid;
            { std::lock_guard<std::mutex> lk(pid_mutex); pid = peer_id; }
            if (pid.empty()) continue;

            // ReadSample 返回实际流序号。本 mp4：流 0=音频(AAC/PCM)、流 1=视频(NV12)
            bool is_video = (si == 1);

            if (is_video) {
                if (video_off) continue;  // 只发音频：跳过视频编码
                video_in.fetch_add(1);
                // SourceReader �?NV12 样本�?16 行填充（1920x1088），UV 平面
                // �?src + 1920*1088；先提取有效 1920x1080 区域（否则色度错位）
                auto src = sample_to_bytes(sample.Get());
                if (src.empty()) continue;
                const UINT32 stride = kSrcW;
                const UINT32 full_h = (UINT32)(src.size() * 2 / 3 / stride);  // 1088
                std::vector<std::uint8_t> nv12((size_t)kSrcW * kSrcH * 3 / 2);
                for (UINT32 y = 0; y < kSrcH; ++y)
                    std::memcpy(nv12.data() + (size_t)y * kSrcW,
                                src.data() + (size_t)y * stride, kSrcW);
                const std::uint8_t* suv = src.data() + (size_t)stride * full_h;
                for (UINT32 y = 0; y < kSrcH / 2; ++y)
                    std::memcpy(nv12.data() + (size_t)kSrcW * kSrcH + (size_t)y * kSrcW,
                                suv + (size_t)y * stride, kSrcW);
                scale_nv12(nv12.data(), (int)kSrcW, (int)kSrcH,
                           scaled.data(), (int)kVideoW, (int)kVideoH);

                // 强制关键帧（peer 上线起步 / req-keyframe / 排空恢复）：重启
                // 编码器 = 新 IDR（QSV 首帧即 IDR）。**冷却 ≥4s**（与码率重启
                // 共享 last_reset）：播放端 req-keyframe 每 500ms 一次，若每
                // 次都重启 → QSV 设备崩溃（device failed -17，之后所有实例
                // 失败 → 10-20s 断流实测）。冷却期内忽略（合并到下一次——
                // 播放端最多等一个冷却周期拿到新 IDR）。
                int c = force_idr_count.load();
                if (c > 0) {
                    force_idr_count.store(c - 1);
                    auto now2 = steady_clock::now();
                    if (now2 - last_reset.load() >= std::chrono::seconds(4)) {
                        last_reset.store(now2);
                        if (!venc.force_keyframe()) {
                            fprintf(stderr, "[src] qsv restart failed (keyframe)\n");
                        }
                    }
                }

                // 写 NV12 帧 → ffmpeg（阻塞至消费，按编码速率节流）
                if (!venc.alive()) {
                    // QSV 设备失败（device failed -17）后新实例也会立即失败，
                    // 100ms 空转重试无效且刷屏；冷却退避（2s，3 连败后 30s——
                    // 设备故障期停止徒劳重启，避免重启风暴进一步损伤驱动）。
                    // respawn 同样遵守 last_reset 4s 冷却（与码率/IDR 重启
                    // 共享）：频繁重启本身是设备 -17 的诱因（实测 L4S 场景
                    // 27-36 次重启 → 10-40s 断流）。
                    static int qsv_fail_count = 0;
                    static auto qsv_retry_until = steady_clock::now();
                    auto now2 = steady_clock::now();
                    if (now2 < qsv_retry_until) {
                        std::this_thread::sleep_for(milliseconds(100));
                        continue;
                    }
                    if (now2 - last_reset.load() < std::chrono::seconds(4)) {
                        std::this_thread::sleep_for(milliseconds(100));
                        continue;
                    }
                    last_reset.store(now2);
                    if (!venc.force_keyframe()) {
                        ++qsv_fail_count;
                        auto backoff = qsv_fail_count >= 3 ? seconds(30) : seconds(2);
                        qsv_retry_until = now2 + backoff;
                        fprintf(stderr, "[src] qsv respawn failed (x%d), retry in %llds\n",
                               qsv_fail_count, (long long)backoff.count());
                        fflush(stderr);
                        continue;
                    }
                    qsv_fail_count = 0;
                }
                if (!venc.write_frame(scaled.data(), scaled.size())) continue;

                // 读 QSV 输出帧（H.264 AU）并发送
                bool kf = false;
                while (venc.read_frame(au, kf)) {
                    auto chunk = media::pack_chunk(media::kTypeVideo, kf ? media::kFlagKeyframe : 0,
                                                   vseq, (uint64_t)pts / 10000,
                                                   au.data(), (std::uint32_t)au.size());
                    if (tx.send_video(pid, std::move(chunk), kf)) {
                        sent_bytes.fetch_add(au.size());
                        sent_vframes.fetch_add(1);
                        // avg 帧大小统计排除 SPS/PPS 等小碎片（QSV 输出
                        // 把参数集切成独立 AU，~23B；计入会污染 avg →
                        // fec_ratio 虚高（600%）→ 预算钳到下限 → 线上
                        // 超发丢包，实测播放端断流）
                        if (au.size() >= 512) {
                            sent_vbytes_big.fetch_add(au.size());
                            sent_vframes_big.fetch_add(1);
                        }
                        vseq++;
                    } else {
                        send_fail.fetch_add(1);  // 队列背压丢帧
                    }
                }
                // 30fps 内容时钟节流（33.3ms/帧，绝对节拍不漂移）
                next_video_tick += std::chrono::milliseconds(33);
                auto vnow = steady_clock::now();
                if (vnow < next_video_tick) std::this_thread::sleep_until(next_video_tick);
                else next_video_tick = vnow;
            } else {
                // 音频由独立线程（ffmpeg 子进程）供给，MF 样本不处理
                continue;
            }
        }
    });

    // 音频发送线程：ffmpeg.exe 子进程解码音轨 → PCM 管道（进程隔离，规避
    // ffmpeg 库与 MF 同进程的崩溃冲突；MF ANY_STREAM 会被视频样本排挤）。
    // 内容时钟 20ms/帧精确 50fps 发送；EOF 自动重启。
    std::thread asender([&] {
        if (!audio_ok) return;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        constexpr std::size_t kFrameBytes = media::kAudioFrameBytes;  // 3840B
        std::vector<std::uint8_t> pcm_buf(65536);
        std::vector<std::int16_t> frame(media::kAudioFrameSamples * media::kAudioChannels);
        std::vector<std::uint8_t> opkt(2048);
        std::uint32_t aseq = 0;
        auto next_tick = steady_clock::now();
        while (!stop.load()) {
            HANDLE rpipe = INVALID_HANDLE_VALUE, wpipe = INVALID_HANDLE_VALUE;
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) break;
            SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = wpipe;
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION pi{};
            std::string cmd = "\"D:\\tools\\udp-sim\\ffmpeg\\bin\\ffmpeg.exe\" -hide_banner -loglevel error "
                              "-i \"" + std::string(kMediaPathUtf8) +
                              "\" -f s16le -ac 2 -ar 48000 -acodec pcm_s16le pipe:1";
            BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(wpipe);
            if (!ok) {
                CloseHandle(rpipe);
                fprintf(stderr, "[src] ffmpeg spawn failed\n");
                break;
            }
            CloseHandle(pi.hThread);
            std::size_t have = 0;
            bool eof = false;
            while (!stop.load() && !eof) {
                DWORD avail = 0;
                if (PeekNamedPipe(rpipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    DWORD got = 0;
                    if (!ReadFile(rpipe, pcm_buf.data() + have, (DWORD)(pcm_buf.size() - have),
                                  &got, nullptr) || got == 0) {
                        eof = true;
                        break;
                    }
                    have += got;
                } else {
                    DWORD ec = 0;
                    if (!GetExitCodeProcess(pi.hProcess, &ec) || ec != STILL_ACTIVE) {
                        eof = true;   // ffmpeg 已退出（输出结束）
                        break;
                    }
                    std::this_thread::sleep_for(milliseconds(5));
                    continue;
                }
                // 提取完整 20ms 帧
                std::size_t consumed = 0;
                while (have - consumed >= kFrameBytes) {
                    std::memcpy(frame.data(), pcm_buf.data() + consumed, kFrameBytes);
                    consumed += kFrameBytes;
                    std::string pid;
                    { std::lock_guard<std::mutex> lk(pid_mutex); pid = peer_id; }
                    if (!pid.empty()) {
                        int n = aenc.encode(frame.data(), opkt.data(), opkt.size());
                        if (n > 0) {
                            auto chunk = media::pack_chunk(media::kTypeAudio, 0, aseq++, 0,
                                                           opkt.data(), (std::uint32_t)n);
                            // 音频走最高优先级队列（priority 1 > 视频 0）
                            if (tx.send_priority(pid, std::move(chunk), 1)) {
                                sent_aframes.fetch_add(1);
                                sent_abytes.fetch_add((std::uint64_t)n);
                            } else {
                                audio_send_fail.fetch_add(1);   // 音频背压：触发视频降码率
                            }
                        }
                    }
                    // 内容时钟节流：每 20ms 音频内容发一帧（实时速率 50fps）
                    next_tick += std::chrono::milliseconds(media::kAudioFrameMs);
                    auto now = steady_clock::now();
                    if (now < next_tick) std::this_thread::sleep_until(next_tick);
                    else next_tick = now;
                }
                if (consumed > 0) {
                    std::memmove(pcm_buf.data(), pcm_buf.data() + consumed, have - consumed);
                    have -= consumed;
                }
            }
            CloseHandle(rpipe);
            if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); }
        }
    });

    // 码率自适应：视频可用码率由 tight 统一计算（扣音频/FEC/file-data），
    // 应用只做上下限钳制与编码器重启。
    constexpr std::uint64_t kBitrateHysteresis = 100000;  // btl 抖动死区 100KB/s
    auto last_bitrate = steady_clock::now();
    auto last_report = steady_clock::now();
    uint64_t prev_bytes = 0;
    uint64_t prev_vframes = 0;
    uint64_t prev_vbytes = 0;
    uint64_t last_btl = 0;            // 上次采纳的 btl（死区基准）
    uint64_t last_br = 0;             // 当前编码器码率
    auto last_audio_guard = steady_clock::now() - std::chrono::seconds(1);
    constexpr std::uint32_t kRecoveryBitrate = 1500000;   // 排空期码率（QSV 720p 最低可行，实测 <1.5M 编码器拒绝）
    constexpr std::uint32_t kMinVideoBps = 1500000;       // 视频码率下限（同上，QSV 硬性约束）
    bool in_recovery = false;        // 排空恢复期（低码率发关键帧，禁正常重置）
    bool recovery_resume = false;    // 恢复期结束后的首次评估保守（封顶 1600k）
    auto recovery_until = steady_clock::now();
    // 日志时间戳 helper：相对启动秒（看 btl 与编码码率的跟随性）
    auto ts_sec = [&]() -> double {
        return duration_cast<milliseconds>(steady_clock::now() - t0).count() / 1000.0;
    };
    // 令牌贷款耗尽/恢复通知（tight sender 线程调用，只置标志，快速返回）：
    //   耗尽 → tight 已暂停视频（持续排空至债务清零），应用静默等待
    //   恢复 → 债务清零、发送恢复，重启编码器（新 IDR + 低码率）快速恢复画面
    std::atomic<bool> loan_penalty{false};
    std::atomic<bool> loan_resume_req{false};
    tx.set_loan_exhausted_callback([&](bool exhausted) {
        if (exhausted) {
            loan_penalty.store(true);
            printf("[src +%.1fs] loan exhausted -> video paused by tight\n", ts_sec());
            fflush(stdout);
        } else {
            loan_penalty.store(false);
            loan_resume_req.store(true);
            printf("[src +%.1fs] loan cleared -> resume encoder\n", ts_sec());
            fflush(stdout);
        }
    });
    // 拥塞排空窗口（fast：大幅降速清队列）：tight 已丢全部待发视频帧并
    // 排空通道，应用重启编码器（新 IDR + 低码率）——播放端跳到新 IDR
    // 时间线，积压瞬间归零。回调只置标志（tight 接收/轮询线程调用）。
    std::atomic<bool> evac_req{false};
    tx.set_evac_keyframe_callback([&] {
        evac_req.store(true);
    });
    while (run_seconds == 0 || duration_cast<seconds>(steady_clock::now() - t0).count() < run_seconds) {
        std::this_thread::sleep_for(milliseconds(250));
        auto now = steady_clock::now();
        // 拥塞排空窗口（fast：降幅 >50% 的剧烈降速）：tight 已清空视频
        // 积压并排空通道，应用重启编码器（低码率 + 新 IDR）——播放端跳到
        // 新 IDR 时间线。新 IDR 提交后 tight 结束窗口（下一报告确认）。
        if (evac_req.exchange(false)) {
            loan_penalty.store(false);   // 快排接管止损（破产清算）：恢复推送——贷款耗尽置的暂停标志由 fast 排空解除
            venc.set_bitrate(kRecoveryBitrate);
            target_bitrate.store(kRecoveryBitrate);
            last_br = kRecoveryBitrate;
            force_idr_count.store(1);
            last_reset.store(now);                     // 恢复期结束前禁止正常重置
            in_recovery = true;
            recovery_until = now + seconds(4);    // 低码率窗口：4s
            recovery_resume = true;
            printf("[src +%.1fs] evac -> QSV restart @ %u bps (keyframe)\n",
                   ts_sec(), (unsigned)kRecoveryBitrate);
            fflush(stdout);
        }
        // 贷款恢复（tight 债务清零）：重启编码器（低码率 + 新 IDR）快速
        // 恢复画面。set_bitrate 同码率时 no-op，force_idr_count 保证编码
        // 线程强制重启出关键帧；复用 in_recovery 低码率窗口 + 保守恢复。
        if (loan_resume_req.exchange(false)) {
            venc.set_bitrate(kRecoveryBitrate);
            target_bitrate.store(kRecoveryBitrate);
            last_br = kRecoveryBitrate;
            force_idr_count.store(1);
            last_reset.store(now);
            in_recovery = true;
            recovery_until = now + seconds(4);
            recovery_resume = true;
            printf("[src +%.1fs] loan resume -> QSV restart @ %u bps (keyframe)\n",
                   ts_sec(), (unsigned)kRecoveryBitrate);
            fflush(stdout);
        }
        // 恢复期结束：低码率窗口已过，恢复正常估算码率。p50 仍高（链路
        // 队列未排空）时延长——否则恢复即超发、再次排空。
        if (in_recovery && now >= recovery_until) {
            std::string pid_chk;
            { std::lock_guard<std::mutex> lk(pid_mutex); pid_chk = peer_id; }
            uint32_t p50_chk = pid_chk.empty() ? 0 : tx.peer_p50_ms(pid_chk);
            if (p50_chk > 200) {
                recovery_until = now + seconds(1);
                printf("[src +%.1fs] recovery extended (p50=%ums)\n", ts_sec(), p50_chk);
                fflush(stdout);
            } else {
                in_recovery = false;
                recovery_resume = true;               // 本次恢复保守（封顶 1600k）
                last_btl = 0;                         // 强制 btl_moved → 重置
                last_reset.store(now - std::chrono::seconds(1));  // 允许立即重置
                printf("[src +%.1fs] recovery window done -> resume normal bitrate\n", ts_sec());
                fflush(stdout);
            }
        }
        // 每 1s 评估一次目标码率
        if (now - last_bitrate >= seconds(1)) {
            last_bitrate = now;
            uint64_t btl = tx.btl_bw_bps();  // bytes/s，总容量
            // 最近 1s 平均帧大小（编码帧字节，含 media 头；排除 SPS/PPS 小碎片）
            uint64_t sv = sent_vframes_big.load(), sb = sent_vbytes_big.load();
            uint64_t avg_frame = 12000;  // 默认 12KB（首秒无统计）
            uint64_t dv = sv - prev_vframes;
            if (dv > 0) avg_frame = (sb - prev_vbytes) / dv;
            prev_vframes = sv;
            prev_vbytes = sb;
            // 视频可用码率由 tight 统一计算：有效带宽 − 音频固定预留
            // （cfg.audio_reserved_bps）− file/data 实时速率，再按实际
            // FEC 冗余折算（fragmenter 滑动窗口统计）。应用不再自行折让
            // （kFecParityMax 假设删除）。avg_frame 仅用于诊断。
            uint64_t btl_bytes = btl;
            uint64_t br = tx.video_capacity_bps();
            if (br < kMinVideoBps) br = kMinVideoBps;  // 最低码率兜底（QSV 下限）
            if (br > 30000000) br = 30000000;  // 上限 30Mbps
            // 启动期（首 3s）码率上限 3M：首报告前 btl 种子（30M）会让
            // 视频以 30M 全速超发弱网链路，积压数 MB → CE/迟到持续（排空
            // 期）→ 连降崩底死锁（L4S 实测）。3s 后解除（btl 已收敛）。
            double elapsed_sec = duration_cast<milliseconds>(steady_clock::now() - t0).count() / 1000.0;
            if (elapsed_sec < 3.0) {
                br = std::min<uint64_t>(br, 3000000);
            }
            // 注：p50 延迟信号已下沉到 tight（BandwidthEstimator::on_peer_p50_ms
            // 强制跟跌 btl），预算公式用 btl 自然响应，sender 层不再重复调整。
            // p50 仅用于诊断打印
            std::string pid_dbg;
            { std::lock_guard<std::mutex> lk(pid_mutex); pid_dbg = peer_id; }
            uint32_t p50_ms = pid_dbg.empty() ? 0 : tx.peer_p50_ms(pid_dbg);
            // 音频背压保障：音频发送失败（带宽确实不足）→ 视频再砍半让路
            static std::uint64_t prev_audio_fail = 0;
            std::uint64_t af_now = audio_send_fail.load();
            if (af_now > prev_audio_fail && now - last_audio_guard >= seconds(1)) {
                prev_audio_fail = af_now;
                last_audio_guard = now;
                br = br / 2;
                printf("[src] audio backpressure(x%llu) -> video rate halved\n",
                       (unsigned long long)(af_now));
            }
            if (br < kMinVideoBps) br = kMinVideoBps;
            // 恢复期后的第一次评估：保守恢复（封顶 1600k，btl 收敛前防超发）
            if (recovery_resume) {
                recovery_resume = false;
                br = std::min<uint64_t>(br, 1600000);
                printf("[src] conservative resume: video bitrate=%llu bps\n",
                       (unsigned long long)br);
                fflush(stdout);
            }
            // 重置编码器（QSV 重启，~300ms 中断视频）：btl 变化（① 绝对 ≥100KB/s
            // ② 相对 >10%），且距上次重启 ≥4s 才重启——弱网段 btl 每报告
            // 台阶变化（恢复 1.5×/拥塞 0.5×），2s 冷却导致 25s 内重启 ~10 次，
            // iGPU 驱动崩溃（device failed -17，之后所有 QSV 实例都失败）
            uint64_t delta = btl > last_btl ? btl - last_btl : last_btl - btl;
            bool btl_moved = delta >= kBitrateHysteresis && delta * 10 > last_btl;
            if (btl_moved && now - last_reset.load() >= seconds(4)) {
                last_btl = btl;
                last_reset.store(now);
                last_br = br;
                target_bitrate.store((uint32_t)br);
                auto t0_enc = steady_clock::now();
                venc.set_bitrate((UINT32)br);
                printf("[src +%.1fs] set_bitrate %llu bps took %lld ms\n",
                       ts_sec(),
                       (unsigned long long)br,
                       (long long)duration_cast<milliseconds>(steady_clock::now() - t0_enc).count());
            }
            printf("[src +%.1fs] btl=%llu B/s p50=%ums fec=%.0f%% avg=%lluB -> video bitrate=%llu bps\n",
                   ts_sec(),
                   (unsigned long long)btl, p50_ms,
                   tx.fec_redundancy_ratio() * 100.0,
                   (unsigned long long)avg_frame, (unsigned long long)br);
            fflush(stdout);
        }
        if (now - last_report >= seconds(5)) {
            last_report = now;
            uint64_t b = sent_bytes.load(), vf = sent_vframes.load();
            printf("[src +%.1fs 5s] vIn=%llu vFrames=%llu bytes=%llu | aFrames=%llu aBytes=%llu | sendFail=%llu\n",
                   ts_sec(),
                   (unsigned long long)video_in.load(), (unsigned long long)vf,
                   (unsigned long long)b,
                   (unsigned long long)sent_aframes.load(),
                   (unsigned long long)sent_abytes.load(),
                   (unsigned long long)send_fail.load());
            fflush(stdout);
            (void)prev_bytes;
        }
    }

    stop = true;
    if (worker.joinable()) worker.join();
    if (asender.joinable()) asender.join();
    venc.shutdown();
    tx.stop();
    MFShutdown();
    CoUninitialize();
    return 0;
}
