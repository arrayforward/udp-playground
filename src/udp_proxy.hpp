#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace udpsim {

#ifdef _WIN32
using SockT = SOCKET;
inline constexpr SockT kInvalidSock = INVALID_SOCKET;
#else
using SockT = int;
inline constexpr SockT kInvalidSock = -1;
#endif

struct DisturbConfig {
    double loss_rate = 0.0;
    double block_rate = 0.0;
    double dup_rate = 0.0;
    double corrupt_rate = 0.0;

    bool delay_enabled = false;
    double delay_prob = 0.0;
    uint32_t delay_min_ms = 0;
    uint32_t delay_max_ms = 0;
    // 正态分布延迟（模拟真实网络陡峭延迟曲线）：主体 N(mean, sigma)
    // 裁剪到 [mean-2σ, mean+2σ]，另按 tail_prob 概率叠加长尾
    // uniform[tail_min_ms, tail_max_ms]（偶发长尾报文，FEC 针对目标）。
    bool delay_normal = false;
    double delay_mean_ms = 10.0;
    double delay_sigma_ms = 1.0;
    double delay_tail_prob = 0.01;
    uint32_t delay_tail_min_ms = 50;
    uint32_t delay_tail_max_ms = 200;

    bool bw_enabled = false;
    enum class Wave { Rect, Sine, Sawtooth, Random } wave = Wave::Rect;
    double bw_min_bps = 1e6;
    double bw_max_bps = 10e6;
    uint32_t bw_period_ms = 1000;
    double bw_duty = 0.5;

    bool reorder_enabled = false;
    double reorder_prob = 0.0;
    uint32_t reorder_max_ms = 0;

    // L4S (RFC 9331) 仿真：入包 ECT(1) 且排队时延超过阈值时标记 CE（标记
    // 替代丢包，让传输协议提前降速）。带宽下降→队列起→标记；传输降速→
    // 队列清→自动停止标记。
    bool l4s_enabled = false;
    double l4s_threshold_ms = 1.0;

    // 报告丢弃概率（链路严重卡顿仿真）：tight Report 报文（magic 'THGT' +
    // type=8）按概率丢弃——发送端报告超时检测（3×报告周期收不到）→ btl
    // 乘性下降收敛。0 = 不丢。
    double report_drop_rate = 0.0;

    uint64_t seed = 0xC0FFEE1234567ULL;
    size_t queue_cap = 4096;
    bool clean_reverse = false;
};

class UdpProxy {
public:
    UdpProxy(std::string listen, std::string target, DisturbConfig cfg, double stats_interval,
             uint16_t control_port = 0);
    ~UdpProxy();

    bool start(std::string& err);
    void stop();
    void wait();

    // 运行中重配置：解析 "bw 400000" / "loss 0.1" / "wave sine" 等命令
    void apply_command(const std::string& cmd);

    // 加载网络损伤场景脚本：按时间轴逐段应用扰动参数，执行完毕从头循环。
    // 脚本格式（每行）：
    //   <duration_seconds> : cmd1 ; cmd2 ; ...
    // 其中 cmd 使用与 apply_command 相同的语法（bw/loss/delay-*/reorder-*/wave 等）。
    // # 开头为注释；未指定的参数继承上一段（循环回到第一段时恢复启动参数）。
    bool set_scenario(const std::string& path, std::string& err);

private:
    using Clock = std::chrono::steady_clock;

    struct Packet {
        std::vector<uint8_t> data;
        sockaddr_in dst;
        Clock::time_point due;
    };
    struct PktCmp {
        bool operator()(const Packet& a, const Packet& b) const { return a.due > b.due; }
    };

    class PktQueue {
    public:
        explicit PktQueue(size_t cap) : cap_(cap) {}
        bool push(Packet p) {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.size() >= cap_) return false;
            q_.push(std::move(p));
            cv_.notify_all();
            return true;
        }
        bool pop_or_wait(Packet& out, const std::atomic<bool>& stop) {
            std::unique_lock<std::mutex> lk(m_);
            while (!stop.load(std::memory_order_relaxed)) {
                if (!q_.empty()) {
                    if (Clock::now() >= q_.top().due) {
                        out = std::move(const_cast<Packet&>(q_.top()));
                        q_.pop();
                        return true;
                    }
                    cv_.wait_until(lk, q_.top().due);
                } else {
                    cv_.wait(lk);
                }
            }
            return false;
        }
        void wake() { cv_.notify_all(); }

        // 带宽变化后按新旧速率比压缩所有待发报文的 due 时间，
        // 等效于把现有积压按新速率重新排程，避免旧积压继续
        // 以旧速率阻塞链路（带宽恢复后 btl 长时间不回升的根因）。
        void rescale(Clock::time_point now, double ratio) {
            std::lock_guard<std::mutex> lk(m_);
            std::vector<Packet> tmp;
            tmp.reserve(q_.size());
            while (!q_.empty()) {
                Packet p = std::move(const_cast<Packet&>(q_.top()));
                q_.pop();
                if (p.due > now) {
                    p.due = now + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double, Clock::period>(
                                          (p.due - now).count() * ratio));
                }
                tmp.push_back(std::move(p));
            }
            for (auto& p : tmp) q_.push(std::move(p));
            cv_.notify_all();
        }
        // 限速关闭（bw 0 / wave off / reset）：清空排程，全部立即发送
        void rebase(Clock::time_point now) {
            std::lock_guard<std::mutex> lk(m_);
            std::vector<Packet> tmp;
            tmp.reserve(q_.size());
            while (!q_.empty()) {
                Packet p = std::move(const_cast<Packet&>(q_.top()));
                q_.pop();
                if (p.due > now) p.due = now;
                tmp.push_back(std::move(p));
            }
            for (auto& p : tmp) q_.push(std::move(p));
            cv_.notify_all();
        }
        size_t size() {
            std::lock_guard<std::mutex> lk(m_);
            return q_.size();
        }

    private:
        size_t cap_;
        std::priority_queue<Packet, std::vector<Packet>, PktCmp> q_;
        std::mutex m_;
        std::condition_variable cv_;
    };

    struct DirStats {
        std::atomic<uint64_t> recv{0}, sent{0}, lost{0}, blocked{0}, delayed{0},
            dup{0}, corrupt{0}, reordered{0}, qfull{0};
        std::atomic<uint64_t> bytes{0};
        std::atomic<uint64_t> ce_marked{0};
        std::atomic<uint64_t> l4s_seen{0};
        std::atomic<uint64_t> report_dropped{0};
    };

    static uint64_t splitmix64(uint64_t x);

    class Engine {
    public:
        Engine(uint64_t seed) : rng_(seed), start_(Clock::now()) {}
        std::mt19937_64& rng() { return rng_; }

        double bandwidth(const DisturbConfig& cfg) {
            double min = cfg.bw_min_bps, max = cfg.bw_max_bps;
            double el = elapsed_ms();
            if (cfg.wave == DisturbConfig::Wave::Random)
                return std::uniform_real_distribution<double>(min, max)(rng_);
            double ph = std::fmod(el, (double)cfg.bw_period_ms) / (double)cfg.bw_period_ms;
            switch (cfg.wave) {
            case DisturbConfig::Wave::Rect:
                return ph < cfg.bw_duty ? max : min;
            case DisturbConfig::Wave::Sine: {
                double x = ph * 2.0 * 3.14159265358979323846;
                return min + (max - min) * 0.5 * (1.0 + std::sin(x));
            }
            case DisturbConfig::Wave::Sawtooth: {
                uint64_t cyc = (uint64_t)(el / (double)cfg.bw_period_ms);
                double s = (double)(splitmix64(cyc + cfg.seed) & 0x7FFFFFFFFFFFFFFFULL) / 9.2233720368547758e18;
                double p = std::fmod(ph + s, 1.0);
                return min + (max - min) * p;
            }
            default:
                return max;
            }
        }

    private:
        double elapsed_ms() {
            return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        }
        std::mt19937_64 rng_;
        Clock::time_point start_;
    };

    void recvLoop();
    void senderLoop(DirStats& st, PktQueue& q);
    void statsLoop();
    void controlLoop();
    void process(DirStats& st, PktQueue& q, std::atomic<bool>& rescale,
                 Clock::time_point& drain, Clock::time_point& bw_drain,
                 const uint8_t* data, size_t n,
                 const sockaddr_in& dst, bool disturb);
    // 带宽相关配置变化时调用（须持有 cfgMutex_）：记录变化前后
    // 有效速率，置位待重排标志，由各方向下次收包时执行重排。
    void markBwChanged(double before_bw);
    void rescaleDirection(PktQueue& q, Clock::time_point& drain,
                          Clock::time_point& bw_drain, std::atomic<bool>& flag);
    sockaddr_in lastClient();
    static bool sameAddr(const sockaddr_in& a, const sockaddr_in& b);
    static int lastErr();

    // 周期性卡顿调度（由 controlLoop 检查）：每 period_ms 期间有 dur_ms
    // 的带宽降到 low_bps，其余时间恢复 normal_bps。
    struct StallSched {
        bool active = false;
        double low_bps = 0;
        double normal_bps = 0;
        uint64_t dur_ms = 0;
        uint64_t period_ms = 0;
        Clock::time_point next_start{};
        Clock::time_point restore_at{};
        bool stalled_now = false;
    };
    StallSched stall_{};
    void applyStallSchedule();

    // 网络损伤场景脚本：时间轴上的若干时间段，每段携带一组扰动命令。
    struct ScenarioStep {
        double duration_s = 0;
        std::vector<std::string> cmds;
    };
    std::vector<ScenarioStep> scenario_;
    DisturbConfig baseline_;   // 启动参数快照，脚本循环回第一段时恢复
    std::thread scenTh_;
    void scenarioLoop();
    void resetToBaseline();

    std::string listenStr_;
    std::string targetStr_;
    std::mutex cfgMutex_;
    DisturbConfig cfg_;
    double statsInterval_;
    uint16_t controlPort_;

    sockaddr_in target_{};
    SockT sock_ = kInvalidSock;
    SockT ctrlSock_ = kInvalidSock;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    Clock::time_point drainFwd_{};
    Clock::time_point drainRev_{};
    // 纯带宽积压指针（每方向，CE 判定专用）：只被带宽串行时间（tms）推进
    // ——delay-normal/reorder 的 due 推后不参与（否则正常延迟仿真被当成
    // 带宽队列积压 → backlog > l4s 阈值 → CE 误触发 → 发送端过度降速，
    // 实测 40-52s 渐变段反复 evac）。due/drain 的带宽队列语义不变。
    Clock::time_point bwDrainFwd_{};
    Clock::time_point bwDrainRev_{};

    Engine engine_;
    PktQueue fwdQ_;
    PktQueue revQ_;
    DirStats fwdStats_;
    DirStats revStats_;

    // 带宽重排挂起标志（controlLoop 置位，recvLoop 消费）
    std::atomic<bool> rescaleFwd_{false};
    std::atomic<bool> rescaleRev_{false};
    std::atomic<bool> bwWasEnabled_{false};
    std::atomic<bool> bwNowEnabled_{false};
    std::atomic<double> bwBefore_{1.0};
    std::atomic<double> bwAfter_{1.0};

    std::mutex clientM_;
    sockaddr_in lastClient_{};

    std::thread recvTh_;
    std::thread fwdTh_;
    std::thread revTh_;
    std::thread statsTh_;
    std::thread ctrlTh_;
};

}  // namespace udpsim
