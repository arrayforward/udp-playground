// file/data 通道端到端测试：
//   node 启动，leaf 连接后 node 发送一个文件 + 多条 data 消息，
//   leaf 端校验内容完整（文件重组、data 去重 only once）。
#include "tight/tight.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using namespace std::chrono;

static int runNode(std::uint16_t port, int seconds, bool arq) {
    tight::TightConfig cfg;
    cfg.bind = tight::NetAddress("0.0.0.0", port);
    cfg.id = "node";
    cfg.token = "tok";
    cfg.role = tight::LinkRole::Node;
    cfg.retransmit_enabled = arq;
    cfg.channel_reliable[2] = true;  // file
    cfg.channel_reliable[3] = true;  // data
    tight::TightTransport node(cfg);
    std::atomic<bool> peer_online{false};
    std::string peer_id;
    node.set_peer_callback([&](const tight::PeerEvent& e) {
        printf("[node peer] %s state=%d\n", e.id.c_str(), static_cast<int>(e.state));
        fflush(stdout);
        if (e.state == tight::LinkState::Online) {
            peer_online = true;
            peer_id = e.id;
        }
    });
    if (!node.start()) { fprintf(stderr, "[node] start failed\n"); return 1; }
    printf("[node] file/data server on %u\n", port);
    fflush(stdout);

    auto t0 = steady_clock::now();
    int file_sent = 0;
    while (duration_cast<std::chrono::seconds>(steady_clock::now() - t0).count() < seconds) {
        if (peer_online.load() && file_sent == 0) {
            std::string hello = "hello-plain";
            node.send(peer_id, tight::Bytes(hello.begin(), hello.end()));
            std::string name = "test.bin";
            tight::Bytes content;
            content.reserve(300 * 1024);
            for (int i = 0; i < 300 * 1024; ++i) content.push_back((std::uint8_t)(i & 0xFF));
            bool ok = node.send_file(peer_id, name, content);
            printf("[node] send_file ok=%d size=%zu\n", (int)ok, content.size());
            fflush(stdout);
            ++file_sent;
            std::this_thread::sleep_for(std::chrono::seconds(2));  // 分开发送，避免瞬间突发
            for (int i = 0; i < 100; ++i) {
                std::string msg = "data-" + std::to_string(i);
                node.send_data(peer_id, tight::Bytes(msg.begin(), msg.end()));
            }
            printf("[node] send_data x100\n");
            fflush(stdout);
        }
        std::this_thread::sleep_for(milliseconds(200));
    }
    node.stop();
    return 0;
}

static int runLeaf(std::uint16_t port, int seconds, std::uint16_t proxy_port, bool arq) {
    tight::TightConfig cfg;
    cfg.bind = tight::NetAddress("0.0.0.0", 0);
    cfg.id = "leaf";
    cfg.token = "tok";
    cfg.role = tight::LinkRole::Leaf;
    cfg.retransmit_enabled = arq;
    cfg.channel_reliable[2] = true;
    cfg.channel_reliable[3] = true;
    tight::TightTransport leaf(cfg);
    std::atomic<int> files{0}, datas{0};
    std::atomic<std::uint64_t> file_bytes{0};
    leaf.set_message_loss_callback([&](const std::string&, std::uint8_t ch) {
        printf("[leaf LOSS] channel=%u\n", (unsigned)ch);
        fflush(stdout);
    });
    leaf.set_message_callback([&](const std::string&, tight::Bytes payload) {
        std::string s(payload.begin(), payload.end());
        printf("[leaf msg] %s\n", s.c_str());
        fflush(stdout);
    });
    leaf.set_file_callback([&](const std::string& peer, const std::string& name, tight::Bytes data) {
        bool ok = (name == "test.bin") && (data.size() == 300 * 1024);
        if (ok) {
            for (std::size_t i = 0; i < data.size(); ++i) {
                if (data[i] != (std::uint8_t)(i & 0xFF)) { ok = false; break; }
            }
        }
        printf("[leaf file] %s size=%zu ok=%d\n", name.c_str(), data.size(), (int)ok);
        fflush(stdout);
        files.fetch_add(1);
        file_bytes.fetch_add(data.size());
    });
    std::mutex dm;
    std::set<std::string> seen;
    leaf.set_data_callback([&](const std::string&, tight::Bytes data) {
        std::string s(data.begin(), data.end());
        std::lock_guard<std::mutex> lk(dm);
        bool dup = !seen.insert(s).second;
        if (dup) printf("[leaf data] DUP: %s\n", s.c_str());
        datas.fetch_add(1);
    });
    if (!leaf.start()) { fprintf(stderr, "[leaf] start failed\n"); return 1; }
    std::uint16_t connect_port = proxy_port > 0 ? proxy_port : port;
    if (!leaf.connect({"node", tight::NetAddress("127.0.0.1", connect_port)})) {
        fprintf(stderr, "[leaf] connect failed\n");
        return 1;
    }
    printf("[leaf] connected (target port %u%s)\n", (unsigned)connect_port,
           proxy_port > 0 ? " via proxy" : "");
    fflush(stdout);
    auto t0 = steady_clock::now();
    while (duration_cast<std::chrono::seconds>(steady_clock::now() - t0).count() < seconds) {
        std::this_thread::sleep_for(milliseconds(500));
        printf("[leaf] files=%d datas=%d bytes=%llu\n",
               files.load(), datas.load(), (unsigned long long)file_bytes.load());
        fflush(stdout);
    }
    leaf.stop();
    std::size_t unique = 0;
    { std::lock_guard<std::mutex> lk(dm); unique = seen.size(); }
    printf("=== leaf final: files=%d datas=%d unique=%zu\n",
           files.load(), datas.load(), unique);
    return 0;
}

int main(int argc, char** argv) {
    std::string role = argc > 1 ? argv[1] : "leaf";
    std::uint16_t port = argc > 2 ? (std::uint16_t)atoi(argv[2]) : 7777;
    int seconds = argc > 3 ? atoi(argv[3]) : 12;
    std::uint16_t proxy_port = argc > 4 ? (std::uint16_t)atoi(argv[4]) : 0;
    bool arq = argc > 5 && std::string(argv[5]) == "arq";
    printf("=== role=%s port=%u seconds=%d proxy=%u arq=%d ===\n",
           role.c_str(), (unsigned)port, seconds, (unsigned)proxy_port, (int)arq);
    if (role == "node") return runNode(port, seconds, arq);
    if (role == "leaf") return runLeaf(port, seconds, proxy_port, arq);
    fprintf(stderr, "bad role: %s (node|leaf)\n", role.c_str());
    return 1;
}
