#include "udp_proxy.hpp"

#include "ecn_platform.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace udpsim {

namespace {

bool resolveAddr(const std::string& hostport, sockaddr_in& out) {
    size_t pos = hostport.rfind(':');
    if (pos == std::string::npos) return false;
    std::string host = hostport.substr(0, pos);
    std::string port = hostport.substr(pos + 1);
    if (host.empty() || port.empty()) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    sockaddr_in r = *reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    out = r;
    return true;
}

}  // namespace

UdpProxy::UdpProxy(std::string listen, std::string target, DisturbConfig cfg, double stats_interval,
                   uint16_t control_port)
    : listenStr_(std::move(listen)),
      targetStr_(std::move(target)),
      cfg_(cfg),
      baseline_(cfg),
      statsInterval_(stats_interval),
      controlPort_(control_port),
      engine_(cfg.seed),
      fwdQ_(cfg.queue_cap),
      revQ_(cfg.queue_cap) {}

UdpProxy::~UdpProxy() { stop(); }

uint64_t UdpProxy::splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

int UdpProxy::lastErr() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool UdpProxy::sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

sockaddr_in UdpProxy::lastClient() {
    std::lock_guard<std::mutex> lk(clientM_);
    return lastClient_;
}

bool UdpProxy::start(std::string& err) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup failed";
        return false;
    }
#endif

    if (!resolveAddr(targetStr_, target_)) {
        err = "cannot resolve target address: " + targetStr_;
        return false;
    }

    sockaddr_in listenAddr{};
    if (!resolveAddr(listenStr_, listenAddr)) {
        err = "cannot resolve listen address: " + listenStr_;
        return false;
    }

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSock) {
        err = "socket() failed, errno=" + std::to_string(lastErr());
        return false;
    }

    int on = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

#ifdef _WIN32
    DWORD rcvTimeout = 200;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (bind(sock_, reinterpret_cast<const sockaddr*>(&listenAddr), sizeof(listenAddr)) != 0) {
        err = "bind(" + listenStr_ + ") failed, errno=" + std::to_string(lastErr());
        return false;
    }

    recvTh_ = std::thread(&UdpProxy::recvLoop, this);
    fwdTh_ = std::thread(&UdpProxy::senderLoop, this, std::ref(fwdStats_), std::ref(fwdQ_));
    revTh_ = std::thread(&UdpProxy::senderLoop, this, std::ref(revStats_), std::ref(revQ_));
    if (statsInterval_ > 0)
        statsTh_ = std::thread(&UdpProxy::statsLoop, this);
    if (controlPort_ > 0) {
        ctrlSock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ctrlSock_ != kInvalidSock) {
            sockaddr_in ctrlAddr{};
            ctrlAddr.sin_family = AF_INET;
            ctrlAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ctrlAddr.sin_port = htons(controlPort_);
            if (bind(ctrlSock_, reinterpret_cast<const sockaddr*>(&ctrlAddr), sizeof(ctrlAddr)) == 0) {
                ctrlTh_ = std::thread(&UdpProxy::controlLoop, this);
            } else {
                closesocket(ctrlSock_);
                ctrlSock_ = kInvalidSock;
            }
        }
    }
    if (!scenario_.empty()) {
        scenTh_ = std::thread(&UdpProxy::scenarioLoop, this);
    }

    started_.store(true);
    return true;
}

void UdpProxy::stop() {
    if (!started_.load() && !stop_.load()) return;
    stop_.store(true);
    fwdQ_.wake();
    revQ_.wake();
    if (recvTh_.joinable()) recvTh_.join();
    if (fwdTh_.joinable()) fwdTh_.join();
    if (revTh_.joinable()) revTh_.join();
    if (statsTh_.joinable()) statsTh_.join();
    if (ctrlTh_.joinable()) ctrlTh_.join();
    if (scenTh_.joinable()) scenTh_.join();
    if (ctrlSock_ != kInvalidSock) {
        closesocket(ctrlSock_);
        ctrlSock_ = kInvalidSock;
    }
    if (sock_ != kInvalidSock) {
#ifdef _WIN32
        closesocket(sock_);
        WSACleanup();
#else
        close(sock_);
#endif
        sock_ = kInvalidSock;
    }
}

void UdpProxy::wait() {
    if (recvTh_.joinable()) recvTh_.join();
}

void UdpProxy::apply_command(const std::string& cmd) {
    std::istringstream iss(cmd);
    std::string key;
    iss >> key;
    if (key.empty()) return;

    std::lock_guard<std::mutex> lk(cfgMutex_);
    double before_bw = cfg_.bw_enabled ? engine_.bandwidth(cfg_) : 0.0;
    bool bw_touched = false;
    auto next = [&]() -> double {
        double v = 0;
        iss >> v;
        return v;
    };
    auto nextU = [&]() -> uint64_t {
        uint64_t v = 0;
        iss >> v;
        return v;
    };

    if (key == "bw") {
        double v = next();
        cfg_.bw_enabled = v > 0;
        if (v > 0) cfg_.bw_min_bps = cfg_.bw_max_bps = v;
        bw_touched = true;
    } else if (key == "bw-min") {
        cfg_.bw_min_bps = next();
        cfg_.bw_enabled = true;
        bw_touched = true;
    } else if (key == "bw-max") {
        cfg_.bw_max_bps = next();
        cfg_.bw_enabled = true;
        bw_touched = true;
    } else if (key == "bw-period") {
        cfg_.bw_period_ms = (uint32_t)nextU();
        bw_touched = true;
    } else if (key == "bw-duty") {
        cfg_.bw_duty = next();
        bw_touched = true;
    } else if (key == "wave") {
        std::string w;
        iss >> w;
        if (w == "rect") cfg_.wave = DisturbConfig::Wave::Rect;
        else if (w == "sine") cfg_.wave = DisturbConfig::Wave::Sine;
        else if (w == "sawtooth") cfg_.wave = DisturbConfig::Wave::Sawtooth;
        else if (w == "random") cfg_.wave = DisturbConfig::Wave::Random;
        else if (w == "off") cfg_.bw_enabled = false;
        bw_touched = true;
    } else if (key == "loss") {
        cfg_.loss_rate = next();
    } else if (key == "block") {
        cfg_.block_rate = next();
    } else if (key == "dup") {
        cfg_.dup_rate = next();
    } else if (key == "corrupt") {
        cfg_.corrupt_rate = next();
    } else if (key == "report-drop") {
        double v = next();
        cfg_.report_drop_rate = (v < 0.0) ? 0.0 : (v > 1.0 ? 1.0 : v);
    } else if (key == "delay-prob") {
        double p = next();
        cfg_.delay_prob = p;
        cfg_.delay_enabled = p > 0;
        if (cfg_.delay_enabled) {
            // 兼容旧语法：delay-prob <prob> <min_ms> <max_ms>
            std::string rest;
            std::getline(iss, rest);
            std::istringstream rs(rest);
            rs >> cfg_.delay_min_ms >> cfg_.delay_max_ms;
            if (cfg_.delay_max_ms == 0) cfg_.delay_max_ms = cfg_.delay_min_ms;
        }
    } else if (key == "delay-min") {
        cfg_.delay_enabled = true;
        cfg_.delay_min_ms = (uint32_t)nextU();
    } else if (key == "delay-max") {
        cfg_.delay_enabled = true;
        cfg_.delay_max_ms = (uint32_t)nextU();
    } else if (key == "delay-normal") {
        // 正态分布延迟：delay-normal <mean_ms> <sigma_ms> [tail_prob tail_min_ms tail_max_ms]
        // 主体 N(mean,sigma) 裁剪 [mean-2σ, mean+2σ] + tail_prob 概率长尾 uniform[tail_min,tail_max]
        cfg_.delay_normal = true;
        cfg_.delay_enabled = true;
        cfg_.delay_mean_ms = next();
        cfg_.delay_sigma_ms = next();
        std::string rest;
        std::getline(iss, rest);
        std::istringstream rs(rest);
        double tp = 0.0;
        uint32_t tmin = 0, tmax = 0;
        rs >> tp >> tmin >> tmax;
        if (tp > 0) cfg_.delay_tail_prob = tp;
        if (tmin > 0) cfg_.delay_tail_min_ms = tmin;
        if (tmax > 0) cfg_.delay_tail_max_ms = tmax;
    } else if (key == "delay-off") {
        cfg_.delay_enabled = false;
        cfg_.delay_normal = false;
    } else if (key == "reorder-prob") {
        cfg_.reorder_enabled = next() > 0;
        cfg_.reorder_prob = cfg_.reorder_enabled ? cfg_.reorder_prob : 0;
    } else if (key == "reorder-max") {
        cfg_.reorder_enabled = true;
        cfg_.reorder_max_ms = (uint32_t)nextU();
    } else if (key == "stats") {
        double v = next();
        if (v >= 0) statsInterval_ = v;
    } else if (key == "l4s") {
        std::string sub;
        iss >> sub;
        if (sub == "on") cfg_.l4s_enabled = true;
        else if (sub == "off") cfg_.l4s_enabled = false;
    } else if (key == "l4s-threshold") {
        double v = next();
        if (v >= 0) cfg_.l4s_threshold_ms = v;
    } else if (key == "stall") {
        std::string sub;
        iss >> sub;
        if (sub == "off") {
            if (stall_.stalled_now) {
                cfg_.bw_enabled = true;
                cfg_.bw_min_bps = cfg_.bw_max_bps = stall_.normal_bps;
                bw_touched = true;
            }
            stall_ = StallSched{};
        } else {
            std::istringstream s(sub + " ");
            std::string rest;
            std::getline(iss, rest);
            s.str(sub + " " + rest);
            double low_bps = 0, dur = 0, period = 0;
            s >> low_bps >> dur >> period;
            if (low_bps > 0 && dur > 0 && period > 0) {
                stall_.active = true;
                stall_.low_bps = low_bps;
                stall_.normal_bps = cfg_.bw_max_bps > 0 ? cfg_.bw_max_bps : 4000000.0;
                stall_.dur_ms = (uint64_t)dur;
                stall_.period_ms = (uint64_t)period;
                stall_.next_start = Clock::now() + std::chrono::milliseconds((uint64_t)period);
                stall_.restore_at = Clock::time_point{};
                stall_.stalled_now = false;
                cfg_.bw_enabled = true;
                bw_touched = true;
            }
        }
    } else if (key == "reset") {
        cfg_.loss_rate = cfg_.block_rate = cfg_.dup_rate = cfg_.corrupt_rate = 0;
        cfg_.report_drop_rate = 0;
        cfg_.delay_enabled = false;
        cfg_.bw_enabled = false;
        bw_touched = true;
    } else {
        printf("unknown control command: %s\n", cmd.c_str());
    }
    if (bw_touched) markBwChanged(before_bw);
}

bool UdpProxy::set_scenario(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open scenario file: " + path;
        return false;
    }
    std::vector<ScenarioStep> steps;
    std::string line;
    size_t lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        // 去注释
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        // trim
        auto isspace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!line.empty() && isspace((unsigned char)line.front())) line.erase(line.begin());
        while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
        if (line.empty()) continue;

        auto colon = line.find(':');
        if (colon == std::string::npos) {
            err = "scenario line " + std::to_string(lineno) + ": missing ':' (duration: cmd; ...)";
            return false;
        }
        std::string durStr = line.substr(0, colon);
        std::string body = line.substr(colon + 1);
        char* end = nullptr;
        double dur = strtod(durStr.c_str(), &end);
        if (end == durStr.c_str() || dur <= 0) {
            err = "scenario line " + std::to_string(lineno) + ": bad duration '" + durStr + "'";
            return false;
        }
        ScenarioStep step;
        step.duration_s = dur;
        // 按 ';' 切分命令
        size_t start = 0;
        while (start <= body.size()) {
            size_t semi = body.find(';', start);
            std::string cmd = (semi == std::string::npos) ? body.substr(start) : body.substr(start, semi - start);
            while (!cmd.empty() && isspace((unsigned char)cmd.front())) cmd.erase(cmd.begin());
            while (!cmd.empty() && isspace((unsigned char)cmd.back())) cmd.pop_back();
            if (!cmd.empty()) step.cmds.push_back(cmd);
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
        steps.push_back(std::move(step));
    }
    if (steps.empty()) {
        err = "scenario file has no steps: " + path;
        return false;
    }
    scenario_ = std::move(steps);
    printf("scenario loaded: %zu steps from %s\n", scenario_.size(), path.c_str());
    fflush(stdout);
    return true;
}

void UdpProxy::resetToBaseline() {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    double before = cfg_.bw_enabled ? engine_.bandwidth(cfg_) : 0.0;
    cfg_ = baseline_;
    markBwChanged(before);
}

void UdpProxy::scenarioLoop() {
    size_t idx = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (idx == 0) resetToBaseline();
        for (auto& c : scenario_[idx].cmds) {
            apply_command(c);
            printf("scenario[%zu]: %s\n", idx, c.c_str());
            fflush(stdout);
        }
        // 分段休眠，响应 stop 与实时控制命令
        auto end = Clock::now() +
                   std::chrono::duration_cast<Clock::duration>(
                       std::chrono::duration<double>(scenario_[idx].duration_s));
        while (!stop_.load(std::memory_order_relaxed) && Clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        idx = (idx + 1) % scenario_.size();
    }
}

void UdpProxy::markBwChanged(double before_bw) {
    bwWasEnabled_.store(before_bw > 0);
    bwNowEnabled_.store(cfg_.bw_enabled);
    bwBefore_.store(before_bw > 0 ? before_bw : 1.0);
    bwAfter_.store(cfg_.bw_enabled ? engine_.bandwidth(cfg_) : 1.0);
    rescaleFwd_.store(true);
    rescaleRev_.store(true);
}

void UdpProxy::rescaleDirection(PktQueue& q, Clock::time_point& drain,
                                Clock::time_point& bw_drain, std::atomic<bool>& flag) {
    flag.store(false);
    auto now = Clock::now();
    bool was = bwWasEnabled_.load(), now_en = bwNowEnabled_.load();
    double before = bwBefore_.load(), after = bwAfter_.load();
    if (!was || !now_en || before <= 0 || after <= 0) {
        q.rebase(now);
        drain = now;
        bw_drain = now;
        return;
    }
    if (before == after) return;
    double ratio = before / after;
    q.rescale(now, ratio);
    if (drain > now) {
        drain = now + std::chrono::duration_cast<Clock::duration>(
                          std::chrono::duration<double, Clock::period>(
                              (drain - now).count() * ratio));
    }
    // 纯带宽积压指针同步缩放（与 drain 一致，CE 判定保持带宽队列语义）
    if (bw_drain > now) {
        bw_drain = now + std::chrono::duration_cast<Clock::duration>(
                             std::chrono::duration<double, Clock::period>(
                                 (bw_drain - now).count() * ratio));
    } else {
        bw_drain = now;
    }
}

void UdpProxy::applyStallSchedule() {
    if (!stall_.active) return;
    auto now = Clock::now();
    std::lock_guard<std::mutex> lk(cfgMutex_);
    double before_bw = cfg_.bw_enabled ? engine_.bandwidth(cfg_) : 0.0;
    if (stall_.stalled_now) {
        if (now >= stall_.restore_at) {
            cfg_.bw_enabled = true;
            cfg_.bw_min_bps = cfg_.bw_max_bps = stall_.normal_bps;
            stall_.stalled_now = false;
            stall_.next_start = now + std::chrono::milliseconds(stall_.period_ms);
            printf("stall: restored bw=%.0f bps\n", stall_.normal_bps);
            fflush(stdout);
            markBwChanged(before_bw);
        }
    } else if (now >= stall_.next_start) {
        cfg_.bw_enabled = true;
        cfg_.bw_min_bps = cfg_.bw_max_bps = stall_.low_bps;
        stall_.stalled_now = true;
        stall_.restore_at = now + std::chrono::milliseconds(stall_.dur_ms);
        printf("stall: bw dropped to %.0f bps for %llu ms\n", stall_.low_bps,
               (unsigned long long)stall_.dur_ms);
        fflush(stdout);
        markBwChanged(before_bw);
    }
}

void UdpProxy::controlLoop() {
    std::vector<uint8_t> buf(2048);
    while (!stop_.load(std::memory_order_relaxed)) {
#ifdef _WIN32
        DWORD t = 200;
        setsockopt(ctrlSock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
        timeval tv{};
        tv.tv_usec = 200000;
        setsockopt(ctrlSock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        applyStallSchedule();
        int r = (int)recvfrom(ctrlSock_, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0,
                              nullptr, nullptr);
        if (r <= 0) continue;
        std::string cmd(reinterpret_cast<char*>(buf.data()), (size_t)r);
        apply_command(cmd);
        printf("control: %s\n", cmd.c_str());
        fflush(stdout);
    }
}

void UdpProxy::process(DirStats& st, PktQueue& q, std::atomic<bool>& rescale,
                       Clock::time_point& drain, Clock::time_point& bw_drain,
                       const uint8_t* data, size_t n,
                       const sockaddr_in& dst, bool disturb) {
    if (rescale.load(std::memory_order_relaxed)) {
        rescaleDirection(q, drain, bw_drain, rescale);
    }
    DisturbConfig cfg;
    {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        cfg = cfg_;
    }
    st.recv.fetch_add(1);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    // L4S：入包一律按 L4S 处理（Windows 读不到 TOS，仿真只有单条 L4S 流；
    // ECT(0) 的 Classic 区分留待平台支持读取后启用）。
    bool l4s = cfg.l4s_enabled;
    if (l4s) st.l4s_seen.fetch_add(1);

    if (disturb && !l4s) {
        if (u01(engine_.rng()) < cfg.loss_rate) {
            st.lost.fetch_add(1);
            return;
        }
        if (u01(engine_.rng()) < cfg.block_rate) {
            st.blocked.fetch_add(1);
            return;
        }
    }

    // 报告丢弃（链路严重卡顿仿真）：tight Report 报文（magic 线上字节
    // 'T''G''H''T' = 54 47 48 54（kMagic 0x54474854 大端编码）+ type=8
    // @p[5]，头部明文）按概率丢弃——发送端报告超时检测（3×报告周期
    // 收不到）触发 btl 乘性下降。
    if (disturb && cfg.report_drop_rate > 0.0 && n >= 48) {
        if (data[0] == 'T' && data[1] == 'G' && data[2] == 'H' && data[3] == 'T' &&
            data[5] == 8 && u01(engine_.rng()) < cfg.report_drop_rate) {
            st.report_dropped.fetch_add(1);
            return;
        }
    }

    Clock::time_point due = Clock::now();

    if (disturb && cfg.delay_enabled) {
        double dms = 0.0;
        if (cfg.delay_normal) {
            // 真实网络延迟：陡峭正态主体（裁剪 ±2σ）+ 偶发长尾（如 1% 概率
            // +50~200ms，即 FEC 要覆盖的长尾报文）
            std::normal_distribution<double> nd(cfg.delay_mean_ms, cfg.delay_sigma_ms);
            double v = nd(engine_.rng());
            double lo = cfg.delay_mean_ms - 2.0 * cfg.delay_sigma_ms;
            double hi = cfg.delay_mean_ms + 2.0 * cfg.delay_sigma_ms;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            dms = v;
            if (u01(engine_.rng()) < cfg.delay_tail_prob) {
                std::uniform_int_distribution<uint32_t> t(cfg.delay_tail_min_ms,
                                                          cfg.delay_tail_max_ms);
                dms += t(engine_.rng());
            }
        } else {
            // 旧模式：delay_prob 概率均匀分布延迟
            if (u01(engine_.rng()) < cfg.delay_prob) {
                uint32_t lo = cfg.delay_min_ms, hi = cfg.delay_max_ms;
                if (lo > hi) std::swap(lo, hi);
                std::uniform_int_distribution<uint32_t> d(lo, hi);
                dms = d(engine_.rng());
            }
        }
        if (dms > 0.0) {
            due += std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double, std::milli>(dms));
            st.delayed.fetch_add(1);
        }
    }

    if (disturb && cfg.reorder_enabled && u01(engine_.rng()) < cfg.reorder_prob) {
        std::uniform_int_distribution<uint32_t> d(0, cfg.reorder_max_ms);
        due += std::chrono::milliseconds(d(engine_.rng()));
        st.reordered.fetch_add(1);
    }

    if (disturb && cfg.bw_enabled) {
        double bw = engine_.bandwidth(cfg);
        auto tms = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>((double)n * 8.0 * 1000.0 / bw));
        // 带宽串行（due/drain 原语义不变）：串行时间 tms 推进发送队列。
        // CE 判定用独立的纯带宽积压指针 bw_drain（只被 tms 推进——delay/
        // reorder 的 due 推后不参与，正常延迟仿真不算拥塞积压）。
        Clock::time_point base = std::max(due, drain);
        // 调度前的既有积压指针（drain 领先 now 的量 = 队列积压时延）。
        Clock::time_point prev_drain = drain;
        due = base + tms;
        drain = due;
        // 纯带宽积压：只按串行时间推进（delay/reorder 不影响）
        Clock::time_point bw_base = std::max(bw_drain, Clock::now());
        Clock::time_point prev_bw = bw_drain;
        bw_drain = bw_base + tms;
        // L4S 标记：带宽队列积压超阈值 → 给接收方发 CE 标记（标记替代
        // 丢包，让传输协议提前降速）。backlog 基于纯带宽积压（bw_drain）
        // ——正常延迟（delay-normal 仿真）不算拥塞。
        if (l4s) {
            auto backlog = prev_bw - Clock::now();
            if (backlog > std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double, std::milli>(cfg.l4s_threshold_ms))) {
                uint32_t magic = tight::tight_detail::ecn::kCeMarkMagic;
                sendto(sock_, reinterpret_cast<const char*>(&magic), sizeof(magic), 0,
                       reinterpret_cast<const sockaddr*>(&dst), (int)sizeof(dst));
                st.ce_marked.fetch_add(1);
            }
        }
    }

    bool dup = disturb && u01(engine_.rng()) < cfg.dup_rate;
    bool corrupt = disturb && u01(engine_.rng()) < cfg.corrupt_rate;

    std::vector<uint8_t> buf(data, data + n);
    if (corrupt && !buf.empty()) {
        std::uniform_int_distribution<size_t> pos(0, buf.size() - 1);
        std::uniform_int_distribution<int> bit(0, 7);
        int flips = 1 + (int)(u01(engine_.rng()) * 3.0);
        for (int i = 0; i < flips; ++i) buf[pos(engine_.rng())] ^= (uint8_t)(1u << bit(engine_.rng()));
        st.corrupt.fetch_add(1);
    }
    if (dup) st.dup.fetch_add(1);

    auto make = [&](const std::vector<uint8_t>& d, Clock::time_point t) {
        Packet p;
        p.data = d;
        p.dst = dst;
        p.due = t;
        return p;
    };

    if (!q.push(make(buf, due))) {
        st.qfull.fetch_add(1);
        return;
    }
    if (dup && !q.push(make(buf, due + std::chrono::microseconds(50)))) {
        st.qfull.fetch_add(1);
    }
}

void UdpProxy::recvLoop() {
    std::vector<uint8_t> buf(65536);
    while (!stop_.load(std::memory_order_relaxed)) {
        sockaddr_in src{};
        int slen = (int)sizeof(src);
        int r = (int)recvfrom(sock_, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0,
                              reinterpret_cast<sockaddr*>(&src), &slen);
        if (r < 0) {
            int e = lastErr();
#ifdef _WIN32
            if (e == WSAETIMEDOUT) continue;
#else
            if (e == EAGAIN || e == EWOULDBLOCK) continue;
#endif
            if (stop_.load(std::memory_order_relaxed)) break;
            continue;
        }
        if (sameAddr(src, target_)) {
            DisturbConfig cfg; { std::lock_guard<std::mutex> lk(cfgMutex_); cfg = cfg_; } process(revStats_, revQ_, rescaleRev_, drainRev_, bwDrainRev_, buf.data(), (size_t)r, lastClient(), !cfg.clean_reverse);
        } else {
            {
                std::lock_guard<std::mutex> lk(clientM_);
                lastClient_ = src;
            }
            process(fwdStats_, fwdQ_, rescaleFwd_, drainFwd_, bwDrainFwd_, buf.data(), (size_t)r, target_, true);
        }
    }
}

void UdpProxy::senderLoop(DirStats& st, PktQueue& q) {
    Packet p;
    while (q.pop_or_wait(p, stop_)) {
        sendto(sock_, reinterpret_cast<const char*>(p.data.data()), (int)p.data.size(), 0,
               reinterpret_cast<const sockaddr*>(&p.dst), (int)sizeof(p.dst));
        st.sent.fetch_add(1);
        st.bytes.fetch_add(p.data.size());
    }
}

void UdpProxy::statsLoop() {
    uint64_t prevFwdBytes = 0, prevRevBytes = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds((int)(statsInterval_ * 1000.0)));
        uint64_t fwdBytes = fwdStats_.bytes.load(), revBytes = revStats_.bytes.load();
        double bwFwd = (fwdBytes - prevFwdBytes) * 8.0 / statsInterval_;
        double bwRev = (revBytes - prevRevBytes) * 8.0 / statsInterval_;
        prevFwdBytes = fwdBytes;
        prevRevBytes = revBytes;

        DisturbConfig cfg;
        {
            std::lock_guard<std::mutex> lk(cfgMutex_);
            cfg = cfg_;
        }
        std::string wf;
        switch (cfg.wave) {
        case DisturbConfig::Wave::Rect: wf = "rect"; break;
        case DisturbConfig::Wave::Sine: wf = "sine"; break;
        case DisturbConfig::Wave::Sawtooth: wf = "sawtooth"; break;
        case DisturbConfig::Wave::Random: wf = "random"; break;
        }

        printf("------------------------------------------------------------\n");
        printf("  forward (client->target): recv=%llu sent=%llu bytes=%llu bw=%.0f bps | loss=%llu block=%llu qfull=%llu delay=%llu dup=%llu corrupt=%llu reorder=%llu | l4s=%llu ce=%llu repDrop=%llu\n",
               (unsigned long long)fwdStats_.recv.load(), (unsigned long long)fwdStats_.sent.load(),
               (unsigned long long)fwdStats_.bytes.load(), bwFwd,
               (unsigned long long)fwdStats_.lost.load(), (unsigned long long)fwdStats_.blocked.load(),
               (unsigned long long)fwdStats_.qfull.load(), (unsigned long long)fwdStats_.delayed.load(),
               (unsigned long long)fwdStats_.dup.load(), (unsigned long long)fwdStats_.corrupt.load(),
               (unsigned long long)fwdStats_.reordered.load(),
               (unsigned long long)fwdStats_.l4s_seen.load(), (unsigned long long)fwdStats_.ce_marked.load(),
               (unsigned long long)fwdStats_.report_dropped.load());
        printf("  reverse (target->client): recv=%llu sent=%llu bytes=%llu bw=%.0f bps | loss=%llu block=%llu qfull=%llu delay=%llu dup=%llu corrupt=%llu reorder=%llu | l4s=%llu ce=%llu\n",
               (unsigned long long)revStats_.recv.load(), (unsigned long long)revStats_.sent.load(),
               (unsigned long long)revBytes, bwRev,
               (unsigned long long)revStats_.lost.load(), (unsigned long long)revStats_.blocked.load(),
               (unsigned long long)revStats_.qfull.load(), (unsigned long long)revStats_.delayed.load(),
               (unsigned long long)revStats_.dup.load(), (unsigned long long)revStats_.corrupt.load(),
               (unsigned long long)revStats_.reordered.load(),
               (unsigned long long)revStats_.l4s_seen.load(), (unsigned long long)revStats_.ce_marked.load());
        if (cfg.bw_enabled)
            printf("  bandwidth wave=%s range=[%.0f, %.0f] bps period=%ums duty=%.2f\n",
                   wf.c_str(), cfg.bw_min_bps, cfg.bw_max_bps, cfg.bw_period_ms, cfg.bw_duty);
        printf("------------------------------------------------------------\n");
        fflush(stdout);
    }
}

}  // namespace udpsim
