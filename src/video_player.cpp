// video_player：tight leaf 接收媒体分片 → MF 解码 H.264 → GDI 渲染；
// Opus 音频走 tight 通道 1，Opus 解码 → PCM → waveOut 播放。
//
// 角色：leaf（客户端），连接 video-sender（node，可经 udp-proxy 中继）。
// 画质/卡顿打点：收包码率、视频帧率、关键帧、解码失败、音频欠载。

#include "tight/tight.hpp"
#include "media_chunk.hpp"
#include "opus_audio.hpp"

#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;
using namespace std::chrono;

namespace {

constexpr UINT32 kVideoW = 1920;
constexpr UINT32 kVideoH = 1080;
constexpr const char* kFfplayPath =
    "D:\\tools\\udp-sim\\ffmpeg\\bin\\ffplay.exe";

// ---------- 线程安全队列 ----------
template <typename T>
struct Queue {
    std::mutex m;
    std::deque<T> q;
    std::condition_variable cv;
    bool closed = false;

    void push(T v) {
        { std::lock_guard<std::mutex> lk(m); q.push_back(std::move(v)); }
        cv.notify_one();
    }
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return closed || !q.empty(); });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
    void close() {
        { std::lock_guard<std::mutex> lk(m); closed = true; }
        cv.notify_all();
    }
};

struct VideoFrame {
    std::vector<std::uint8_t> data;  // Annex-B 一个访问单元
    bool keyframe = false;
    std::uint64_t pts_ms = 0;
    std::uint32_t seq = 0;
};

// ---------- H.264 解码器（MF MFT） ----------
// ---------- ffplay 子进程播放器 ----------
// 把 H.264 访问单元写入 ffplay 的 stdin，ffplay 解码并显示窗口。
class FfplayDecoder {
public:
    bool init() {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE stdin_r = nullptr, stdin_w = nullptr;
        if (!CreatePipe(&stdin_r, &stdin_w, &sa, 0)) return false;
        SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdin_r;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::string cmd = std::string("\"") + kFfplayPath +
            "\" -hide_banner -loglevel error -hwaccel dxva2 -vf \"hwdownload,format=nv12\" "
            "-f h264 -i pipe:0 "
            "-window_title \"tight video\" -autoexit -framedrop -x 640 -y 360";
        BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(stdin_r);
        if (!ok) { CloseHandle(stdin_w); return false; }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        in_ = stdin_w;
        return true;
    }

    bool write(const std::uint8_t* p, std::size_t n) {
        DWORD written = 0;
        return WriteFile(in_, p, (DWORD)n, &written, nullptr) && written == (DWORD)n;
    }

    bool alive() const {
        DWORD code = 0;
        return proc_ && GetExitCodeProcess(proc_, &code) && code == STILL_ACTIVE;
    }

    ~FfplayDecoder() {
        if (in_) { CloseHandle(in_); in_ = nullptr; }
        if (proc_) {
            if (GetExitCodeProcess(proc_, nullptr)) {
                // 尝试正常退出，避免挂起；关闭 stdin 后 ffplay 收到 EOF
                WaitForSingleObject(proc_, 500);
            }
            TerminateProcess(proc_, 0);
            CloseHandle(proc_);
            proc_ = nullptr;
        }
    }

private:
    HANDLE proc_ = nullptr;
    HANDLE in_ = nullptr;
};

// NV12 → 32-bit RGB（GDI DIB 用 BGRX，小端）
void nv12_to_rgb(const std::uint8_t* nv12, std::uint8_t* rgb,
                 int w, int h) {
    const std::uint8_t* y = nv12;
    const std::uint8_t* uv = nv12 + (size_t)w * h;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            int yy = y[j * w + i];
            int u = uv[(j / 2) * w + (i & ~1)];
            int v = uv[(j / 2) * w + (i & ~1) + 1];
            int c = yy - 16;
            int d = u - 128;
            int e = v - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
            auto clip = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };
            std::uint8_t* px = rgb + (size_t)(j * w + i) * 4;
            px[0] = (std::uint8_t)clip(b);
            px[1] = (std::uint8_t)clip(g);
            px[2] = (std::uint8_t)clip(r);
            px[3] = 0;
        }
    }
}

}  // namespace

// ---------- 音频播放：Opus 解码 → waveOut，300ms 抖动缓冲状态机 ----------
// 缓冲策略（防连续卡顿）：
//   Warm-up  队列水位 < kBufWarmFrames(300ms) 时只攒不播
//   Playing  水位 ≥ 300ms 后按 20ms 帧节拍播放；队列耗尽（欠载）时
//            用 Opus PLC 隐藏帧顶替一拍并回到 Warm-up 重新攒满再播
//   入队封顶  水位 > kBufMaxFrames(800ms) 丢队头旧帧（延迟封顶）
struct AudioOut {
    static constexpr int kFrameSamples = 960;                       // 20ms
    static constexpr std::size_t kFrameBytes = kFrameSamples * 2 * 2;
    static constexpr int kBufWarmFrames = 15;                       // 300ms
    static constexpr int kBufMaxFrames = 40;                        // 800ms
    static constexpr int kNBuf = 4;                                 // waveOut 缓冲（覆盖 ~40ms 设备预取延迟）

    audio::OpusDecoder dec;
    std::mutex dec_m;   // 解码器非线程安全：接收线程(解码)与播放线程(PLC)互斥
    std::atomic<bool> ok{false};
    std::atomic<bool> stop_flag{false};
    std::atomic<std::uint64_t> underrun{0}, plc_frames{0}, drop_old{0}, played{0};
    std::atomic<int> level{0};

    std::uint32_t last_aseq = 0;
    bool have_aseq = false;
    // 音频帧序号过滤：重复/乱序丢弃（message_callback 单线程调用，无需锁）。
    // 注意：缺口的帧不等待——Opus 帧自含可独立解码，缺口由 PLC 顶替。
    bool aseq_gap(std::uint32_t seq) {
        if (!have_aseq) { last_aseq = seq; have_aseq = true; return false; }
        if (seq <= last_aseq) return true;
        last_aseq = seq;
        return false;
    }

    std::mutex m;
    std::deque<std::vector<std::int16_t>> q;
    std::condition_variable cv;

    void push(std::vector<std::int16_t>&& pcm) {
        {
            std::lock_guard<std::mutex> lk(m);
            // 入队封顶：超过 800ms 丢队头旧帧（内容继续走，防延迟无限增大）
            while ((int)q.size() >= kBufMaxFrames) {
                q.pop_front();
                drop_old.fetch_add(1);
            }
            q.push_back(std::move(pcm));
            level.store((int)q.size());
        }
        cv.notify_one();
    }

    bool pop(std::vector<std::int16_t>& out) {
        std::unique_lock<std::mutex> lk(m);
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        level.store((int)q.size());
        return true;
    }

    void run() {
        if (!dec.init()) { fprintf(stderr, "[player] opus decoder unavailable\n"); return; }
        // 高精度定时器（1ms 粒度）：播放节拍依赖 sleep 精度，默认 15.6ms
        // 粒度会放大水位反馈增益导致播放速率震荡（顶 800ms 丢旧帧）。
        timeBeginPeriod(1);
        // 播放线程提优先级：ffplay 视频解码（子进程）会抢占 CPU，导致
        // 播放节拍 sleep_until 延迟（实测 50fps 节拍被拖到 ~28fps）。
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        // 48kHz / 2ch / 16bit PCM
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        HWAVEOUT hwo = nullptr;
        if (waveOutOpen(&hwo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            fprintf(stderr, "[player] waveOutOpen failed (no audio device)\n");
            return;
        }
        ok.store(true);
        std::vector<std::vector<std::int16_t>> bufs(kNBuf);
        std::vector<WAVEHDR> hdrs(kNBuf);
        for (int i = 0; i < kNBuf; ++i) {
            bufs[i].resize(kFrameSamples * 2);
            std::memset(&hdrs[i], 0, sizeof(hdrs[i]));
        }
        bool playing = false;          // false = warm-up（只攒不播）
        int cur = 0;                   // 双缓冲轮换
        auto next_tick = std::chrono::steady_clock::now();
        while (!stop_flag.load()) {
            if (!playing) {
                if (level.load() < kBufWarmFrames) {
                    std::this_thread::sleep_for(milliseconds(5));
                    continue;
                }
                playing = true;        // 攒满 300ms，启动播放
                next_tick = std::chrono::steady_clock::now();
            }
            std::vector<std::int16_t> pcm;
            if (!pop(pcm)) {
                // 欠载：PLC 隐藏帧顶替一拍，回到 warm-up 重新攒满再播
                pcm.resize(kFrameSamples * 2);
                int n;
                {
                    std::lock_guard<std::mutex> lk(dec_m);
                    n = dec.decode(nullptr, 0, pcm.data());
                }
                if (n != kFrameSamples) { std::this_thread::sleep_for(milliseconds(5)); continue; }
                underrun.fetch_add(1);
                plc_frames.fetch_add(1);
                playing = false;
            }
            // 写块：找空闲缓冲（waveOut 有 ~20ms 设备预取延迟，DONE 约
            // 40ms/块；双缓冲下 20ms 节拍会写失败）。4 缓冲覆盖 80ms，
            // 正常节拍几乎不丢；全忙（设备过慢）则丢这一块。
            int idx = -1;
            for (int i = 0; i < kNBuf; ++i) {
                if (!(hdrs[i].dwFlags & WHDR_PREPARED) || (hdrs[i].dwFlags & WHDR_DONE)) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0) {
                if (hdrs[idx].dwFlags & WHDR_DONE)
                    waveOutUnprepareHeader(hwo, &hdrs[idx], sizeof(WAVEHDR));
                std::memcpy(bufs[idx].data(), pcm.data(), kFrameBytes);
                hdrs[idx].dwBufferLength = kFrameBytes;
                hdrs[idx].lpData = reinterpret_cast<LPSTR>(bufs[idx].data());
                if (waveOutPrepareHeader(hwo, &hdrs[idx], sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
                    waveOutWrite(hwo, &hdrs[idx], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                    played.fetch_add(1);
                } else {
                    drop_old.fetch_add(1);  // Prepare/Write 失败
                }
            } else {
                drop_old.fetch_add(1);  // 设备过慢丢一块（极端情况）
            }
            // 绝对时钟节拍 20ms（50fps，不漂移）：CPU 抢占导致 sleep_until
            // 延迟时追帧——跳到下一节拍点（丢旧内容、保 50fps 速率），
            // 实时音频标准做法（直播追帧止损）。
            next_tick += std::chrono::milliseconds(20);
            auto now = std::chrono::steady_clock::now();
            while (next_tick < now) next_tick += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next_tick);
        }
        waveOutReset(hwo);
        for (int i = 0; i < kNBuf; ++i) {
            if (hdrs[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));
        }
        waveOutClose(hwo);
        timeEndPeriod(1);
    }
};

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string connect_host = "127.0.0.1";
    std::uint16_t connect_port = 9999;
    int run_seconds = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--connect" && i + 1 < argc) {
            std::string ep = argv[++i];
            auto pos = ep.find(':');
            connect_host = ep.substr(0, pos);
            connect_port = (uint16_t)atoi(ep.substr(pos + 1).c_str());
        } else if (a == "--seconds" && i + 1 < argc) run_seconds = atoi(argv[++i]);
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    FfplayDecoder dec;
    if (!dec.init()) { fprintf(stderr, "[player] ffplay init failed\n"); return 1; }

    Queue<VideoFrame> vq;

    std::atomic<std::uint64_t> rx_bytes{0};
    std::atomic<std::uint64_t> wrote_frames{0}, wrote_bytes{0}, dropped_frames{0};

    tight::TightConfig cfg;
    cfg.bind = tight::NetAddress("0.0.0.0", 0);
    cfg.id = "player";
    cfg.token = "tok";
    cfg.role = tight::LinkRole::Leaf;
    cfg.lite_mode = false;  // 1080p 高码率下 lite 单线程 reactor 会因 RS 编码拖慢接收导致 UDP 丢包
    cfg.report_interval = std::chrono::milliseconds(333);  // 1s 3 次反馈，配合发送端快速收敛
    cfg.late_buffer_ms = 16;  // 迟到 buffer（与发送端一致）：延迟超 P50+16ms 记迟到，超线比例驱动 FEC
    cfg.channel_reliable[2] = true;  // file 通道：可靠 ARQ
    cfg.channel_reliable[3] = true;  // data 通道：可靠 ARQ + 去重

    AudioOut aout;

    tight::TightTransport tx(cfg);
    tx.set_message_callback([&](const std::string&, tight::Bytes payload) {
        if (!media::is_media_chunk(payload.data(), payload.size())) return;
        media::ChunkHeader h = media::parse_header(payload.data());
        const std::uint8_t* body = payload.data() + media::kHeaderSize;
        if (h.type == media::kTypeVideo) {
            VideoFrame vf;
            vf.data.assign(body, body + h.size);
            vf.keyframe = (h.flags & media::kFlagKeyframe) != 0;
            vf.pts_ms = h.pts_ms;
            vf.seq = h.seq;
            rx_bytes.fetch_add(h.size);
            vq.push(std::move(vf));
        } else if (h.type == media::kTypeAudio) {
            // 音频：seq 乱序/重复过滤 → Opus 解码 → 抖动缓冲队列
            if (aout.aseq_gap(h.seq)) return;
            std::vector<std::int16_t> pcm(aout.kFrameSamples * 2);
            int n;
            {
                std::lock_guard<std::mutex> lk(aout.dec_m);
                n = aout.dec.decode(body, (int)h.size, pcm.data());
            }
            if (n != aout.kFrameSamples) return;
            aout.push(std::move(pcm));
        }
    });

    if (!tx.start()) { fprintf(stderr, "[player] start failed\n"); return 1; }
    if (!tx.connect({"video-src", tight::NetAddress(connect_host.c_str(), connect_port)})) {
        fprintf(stderr, "[player] connect failed\n");
        return 1;
    }
    printf("[player] connected to %s:%u\n", connect_host.c_str(), connect_port);
    fflush(stdout);

    // FEC 重组失败（视频帧丢失）→ 向发送端申请关键帧快速恢复画面。
    // 限频 500ms 防弱网风暴；命令通道直发，发送端数据面积压时仍可达。
    // channel 区分流：仅视频通道（0）需要关键帧。
    auto last_keyframe_req = steady_clock::now() - std::chrono::seconds(1);
    tx.set_message_loss_callback([&](const std::string& peer_id, std::uint8_t channel) {
        if (channel != 0) return;  // 非视频通道不请求关键帧
        auto now = steady_clock::now();
        if (now - last_keyframe_req >= milliseconds(500)) {
            last_keyframe_req = now;
            const char req[] = "req-keyframe";
            tight::Bytes cmd(req, req + sizeof(req) - 1);
            tx.send_command(peer_id, std::move(cmd));
        }
    });

    // 视频喂入线程：乱序过滤 → 写 ffplay stdin（ffplay 负责解码显示）
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> drop_ooo{0}, drop_nokey{0};
    std::thread dthread([&] {
        VideoFrame vf;
        std::uint32_t last_seq = 0;
        bool have_seq = false;
        bool need_keyframe = true;  // 初始需 IDR；丢帧缺口后也需 IDR 恢复
        while (vq.pop(vf)) {
            if (have_seq) {
                if (vf.seq <= last_seq) { dropped_frames.fetch_add(1); drop_ooo.fetch_add(1); continue; }  // 乱序/重复
                if (vf.seq > last_seq + 1) need_keyframe = true;  // 有丢帧缺口
            }
            if (!vf.keyframe && need_keyframe) { dropped_frames.fetch_add(1); drop_nokey.fetch_add(1); continue; }
            last_seq = vf.seq;
            have_seq = true;
            if (vf.keyframe) need_keyframe = false;
            if (!dec.write(vf.data.data(), vf.data.size())) break;  // ffplay 退出（管道断开）
            wrote_frames.fetch_add(1);
            wrote_bytes.fetch_add(vf.data.size());
        }
    });

    // 音频播放线程：300ms 抖动缓冲状态机 + waveOut + PLC
    std::thread athread([&] { aout.run(); });

    auto t0 = steady_clock::now();
    auto last_report = steady_clock::now();
    uint64_t prev_bytes = 0;
    auto prev_time = steady_clock::now();
    while (run_seconds == 0 || duration_cast<seconds>(steady_clock::now() - t0).count() < run_seconds) {
        std::this_thread::sleep_for(milliseconds(50));

        auto now = steady_clock::now();
        if (now - last_report >= seconds(5)) {
            uint64_t b = rx_bytes.load();
            double rate = (double)(b - prev_bytes) /
                          duration_cast<milliseconds>(now - prev_time).count() * 1000.0;
            printf("[player 5s] rx=%.1fKB/s wrote=%llu/%lluB drop=%llu(ooo%llu,nokey%llu) play=%d btl=%.1fKB/s | audio played=%llu underrun=%llu plc=%llu dropOld=%llu lvl=%d%s\n",
                   rate / 1024.0,
                   (unsigned long long)wrote_frames.load(),
                   (unsigned long long)wrote_bytes.load(),
                   (unsigned long long)dropped_frames.load(),
                   (unsigned long long)drop_ooo.load(),
                   (unsigned long long)drop_nokey.load(),
                   dec.alive() ? 1 : 0,
                   tx.btl_bw_bps() / 1024.0,
                   (unsigned long long)aout.played.load(),
                   (unsigned long long)aout.underrun.load(),
                   (unsigned long long)aout.plc_frames.load(),
                   (unsigned long long)aout.drop_old.load(),
                   aout.level.load(),
                   aout.ok.load() ? "" : " (no-audio)");
            fflush(stdout);
            prev_bytes = b;
            prev_time = now;
            last_report = now;
        }
    }

    stop = true;
    aout.stop_flag.store(true);
    vq.close();
    if (dthread.joinable()) dthread.join();
    if (athread.joinable()) athread.join();

    tx.stop();
    MFShutdown();
    CoUninitialize();
    return 0;
}
