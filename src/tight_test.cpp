#include "tight/tight.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace std::chrono;

namespace {

struct LeafStats {
    std::atomic<uint64_t> sent{0}, skipped{0}, sendFail{0};
    std::atomic<uint64_t> echoed{0}, inOrder{0}, gapEvents{0}, lateOrDup{0};
    std::atomic<uint64_t> rttMin{UINT64_MAX}, rttMax{0}, rttSum{0};
    std::atomic<int64_t> onlineMs{-1};
    std::atomic<bool> online{false};
};

int runNode(uint16_t port, int secs, bool echo) {
    tight::TightConfig cfg;
    cfg.bind = tight::NetAddress("0.0.0.0", port);
    cfg.id = "node";
    cfg.token = "tok";
    cfg.role = tight::LinkRole::Node;

    tight::TightTransport node(cfg);
    std::atomic<uint64_t> recv{0}, sendFail{0};
    std::atomic<int> state{0};

    node.set_peer_callback([](const tight::PeerEvent& e) {
        printf("[node peer] %s state=%d client_id=%u\n", e.id.c_str(),
               static_cast<int>(e.state), e.client_id);
        fflush(stdout);
    });
    node.set_message_callback([&](const std::string& peer, tight::Bytes payload) {
        recv.fetch_add(1);
        if (echo && !node.send(peer, std::move(payload))) sendFail.fetch_add(1);
    });

    if (!node.start()) {
        fprintf(stderr, "[node] start failed\n");
        return 1;
    }
    printf("[node] echo server on %u\n", port);
    fflush(stdout);

    for (int i = 1; i <= secs; ++i) {
        std::this_thread::sleep_for(seconds(1));
        auto peers = node.peers();
        int st = -1;
        if (!peers.empty()) st = static_cast<int>(peers[0].state);
        printf("[node %02ds] msgs=%llu sendFail=%llu peerState=%d\n",
               i, (unsigned long long)recv.load(),
               (unsigned long long)sendFail.load(), st);
        fflush(stdout);
    }
    node.stop();
    return 0;
}

int runLeaf(uint16_t port, int secs, int sendIntervalMs, int msgSize, bool sendAdaptive) {
    tight::TightConfig cfg;
    cfg.bind = tight::NetAddress("0.0.0.0", 0);
    cfg.id = "leaf";
    cfg.token = "tok";
    cfg.lite_mode = true;
    // 内存最小化测量：关测速（100KB train 缓冲）与加密（AES/X25519 上下文）
    cfg.speed_test_enabled = false;
    cfg.encryption_enabled = false;

    tight::TightTransport client(cfg);
    LeafStats st;
    auto t0 = steady_clock::now();

    client.set_peer_callback([&](const tight::PeerEvent& e) {
        printf("[leaf peer] %s state=%d\n", e.id.c_str(), static_cast<int>(e.state));
        fflush(stdout);
        if (e.state == tight::LinkState::Online && st.onlineMs.load() < 0) {
            st.online = true;
            st.onlineMs = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        }
    });

    uint32_t expectedSeq = 1;
    client.set_message_callback([&](const std::string&, tight::Bytes payload) {
        if (payload.size() < 12) return;
        uint64_t ts;
        uint32_t seq;
        memcpy(&ts, payload.data(), 8);
        memcpy(&seq, payload.data() + 8, 4);
        uint64_t rtt = tight::unix_millis() - ts;

        uint64_t curMin = st.rttMin.load();
        while (rtt < curMin && !st.rttMin.compare_exchange_weak(curMin, rtt)) {}
        uint64_t curMax = st.rttMax.load();
        while (rtt > curMax && !st.rttMax.compare_exchange_weak(curMax, rtt)) {}
        st.rttSum.fetch_add(rtt);
        st.echoed.fetch_add(1);

        if (seq == expectedSeq) {
            st.inOrder.fetch_add(1);
            ++expectedSeq;
        } else if (seq > expectedSeq) {
            st.gapEvents.fetch_add(1);
            expectedSeq = seq + 1;
        } else {
            st.lateOrDup.fetch_add(1);
        }
    });

    if (!client.start()) {
        fprintf(stderr, "[leaf] start failed\n");
        return 1;
    }
    if (!client.connect({"node", tight::NetAddress("127.0.0.1", port)})) {
        fprintf(stderr, "[leaf] connect failed\n");
        return 1;
    }

    std::atomic<bool> stop{false};
    std::thread sender([&] {
        auto next = steady_clock::now();
        double tokens = 0.0;  // 分数令牌桶：允许发任意低速率（CE 降速时能排空队列）
        while (!stop.load()) {
            next += milliseconds(sendAdaptive ? 5 : sendIntervalMs);
            std::this_thread::sleep_until(next);
            if (!st.online.load()) {
                st.skipped.fetch_add(1);
                continue;
            }
            if (sendAdaptive) {
                // 速率自适应：app 发送速率 = tight 估算带宽（含增益的
                // 当前发送速率）。估算多少，app 就发多少，使测试需求
                // 始终与估计器联动（避免固定速率 app 成为瓶颈，掩盖
                // 链路带宽的跟进行为）。
                uint64_t bps = client.estimated_bandwidth_bps();
                tokens += static_cast<double>(bps) * 5.0 / 1000.0 /
                          static_cast<double>(msgSize);
                int n = static_cast<int>(tokens);
                if (n > 128) n = 128;  // 单拍上限，防止初始洪泛击穿编码队列
                tokens -= static_cast<double>(n);
                for (int i = 0; i < n; ++i) {
                    tight::Bytes msg(static_cast<std::size_t>(msgSize));
                    uint64_t ts = tight::unix_millis();
                    uint32_t seq = (uint32_t)(st.sent.fetch_add(1) + 1);
                    memcpy(msg.data(), &ts, 8);
                    memcpy(msg.data() + 8, &seq, 4);
                    if (!client.send("node", std::move(msg))) st.sendFail.fetch_add(1);
                }
            } else {
                tight::Bytes msg(static_cast<std::size_t>(msgSize));
                uint64_t ts = tight::unix_millis();
                uint32_t seq = (uint32_t)(st.sent.fetch_add(1) + 1);
                memcpy(msg.data(), &ts, 8);
                memcpy(msg.data() + 8, &seq, 4);
                if (!client.send("node", std::move(msg))) st.sendFail.fetch_add(1);
            }
        }
    });

    auto report = [&]() {
        uint64_t sent = st.sent.load(), echoed = st.echoed.load();
        uint64_t min = st.rttMin.load(), max = st.rttMax.load();
        uint64_t sum = st.rttSum.load();
        printf("  sent=%llu skipped=%llu fail=%llu echoed=%llu unechoed=%llu | inOrder=%llu reorder/gap=%llu late/dup=%llu\n",
               (unsigned long long)sent, (unsigned long long)st.skipped.load(),
               (unsigned long long)st.sendFail.load(), (unsigned long long)echoed,
               (unsigned long long)(sent - echoed),
               (unsigned long long)st.inOrder.load(),
               (unsigned long long)st.gapEvents.load(),
               (unsigned long long)st.lateOrDup.load());
        if (echoed > 0)
            printf("  RTT ms: min=%llu avg=%.1f max=%llu\n",
                   (unsigned long long)min, (double)sum / (double)echoed,
                   (unsigned long long)max);
        printf("  estBw=%.1fKB/s btl=%.1fKB/s appLim=%d pacerLim=%d\n",
               client.estimated_bandwidth_bps() / 1024.0,
               client.btl_bw_bps() / 1024.0,
               (int)client.pacer_app_limited(), (int)client.pacer_limited());
        fflush(stdout);
    };

    for (int i = 1; i <= secs; ++i) {
        std::this_thread::sleep_for(seconds(1));
        printf("[leaf %02ds]\n", i);
        report();
    }

    stop = true;
    sender.join();
    std::this_thread::sleep_for(seconds(3));

    printf("=== leaf final ===\n");
    report();
    if (st.onlineMs.load() >= 0)
        printf("  online after %lld ms\n", (long long)st.onlineMs.load());
    else
        printf("  NEVER ONLINE\n");
    fflush(stdout);

    client.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string role = "leaf";
    uint16_t port = 5555;
    int seconds = 15;
    int sendIntervalMs = 10;
    int msgSize = 1280;
    bool sendAdaptive = false;
    bool echo = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", name);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--role") role = next("--role");
        else if (a == "--port") port = (uint16_t)atoi(next("--port"));
        else if (a == "--seconds") seconds = atoi(next("--seconds"));
        else if (a == "--send-interval") sendIntervalMs = atoi(next("--send-interval"));
        else if (a == "--msg-size") msgSize = atoi(next("--msg-size"));
        else if (a == "--send-adaptive") sendAdaptive = true;
        else if (a == "--no-echo") echo = false;
        else {
            fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 1;
        }
    }

    if (role == "node") return runNode(port, seconds, echo);
    if (role == "leaf") return runLeaf(port, seconds, sendIntervalMs, msgSize, sendAdaptive);
    if (role == "none") {
        // 空跑基线（内存差分测量用）：不创建 tight，仅进程基础 + sleep
        for (int i = 0; i < seconds; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        return 0;
    }
    fprintf(stderr, "unknown role: %s\n", role.c_str());
    return 1;
}
