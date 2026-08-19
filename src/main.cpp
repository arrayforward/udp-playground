#include "udp_proxy.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

volatile sig_atomic_t g_interrupt = 0;

void onSignal(int) { g_interrupt = 1; }

void usage(const char* prog) {
    printf(
        "UDP weak-network proxy (for testing UDP transport protocols like creek/tight)\n"
        "\n"
        "Usage: %s --target IP:PORT [options]\n"
        "\n"
        "The proxy listens on a local UDP port, forwards every datagram to the target\n"
        "port, and routes replies back to the client. Both directions pass through the\n"
        "configured disturbances (use --clean-reverse to leave the return path clean).\n"
        "\n"
        "Required:\n"
        "  --target IP:PORT          target (server) address\n"
        "\n"
        "Listen:\n"
        "  --listen IP:PORT          listen address, default 0.0.0.0:5555\n"
        "\n"
        "Loss / block:\n"
        "  --loss 0.00~1.00          random packet loss ratio\n"
        "  --block 0.00~1.00         random packet block ratio (dropped, simulates\n"
        "                            congestion discard; counted separately)\n"
        "\n"
        "Delay:\n"
        "  --delay-prob 0.00~1.00    probability a packet gets delayed\n"
        "  --delay-min MS            lower bound of random delay\n"
        "  --delay-max MS            upper bound of random delay (uniform in [min,max])\n"
        "\n"
        "Bandwidth limit / jitter:\n"
        "  --bw BPS                  fixed bandwidth cap (no jitter)\n"
        "  --bw-jitter-wave WAVE     jitter waveform: rect | sine | sawtooth | random\n"
        "                                rect    = square wave (high level for duty)\n"
        "                                sine    = smooth sine between min and max\n"
        "                                sawtooth= sawtooth ramp, start phase randomized\n"
        "                                          every cycle (random sawtooth)\n"
        "                                random  = per-packet uniform random value\n"
        "  --bw-min BPS              lower bound of bandwidth jitter range\n"
        "  --bw-max BPS              upper bound of bandwidth jitter range\n"
        "  --bw-period MS            jitter period (fixed interval), default 1000\n"
        "  --bw-duty 0.00~1.00       rect wave duty cycle, default 0.5\n"
        "  Bandwidth pacing: transmit time of a datagram = size*8 / current bandwidth;\n"
        "  when traffic exceeds the current bandwidth the queue grows and packets are\n"
        "  delayed; when the queue is full, excess packets are dropped (qfull).\n"
        "\n"
        "Reorder:\n"
        "  --reorder-prob 0.00~1.00  probability a packet gets an extra hold-off delay\n"
        "  --reorder-max MS          max hold-off, causing later packets to pass first\n"
        "\n"
        "Other disturbances:\n"
        "  --dup 0.00~1.00           duplicate ratio (two copies are sent)\n"
        "  --corrupt 0.00~1.00       corruption ratio (random bit flips, for CRC/AEAD\n"
        "                            robustness tests)\n"
        "\n"
        "Misc:\n"
        "  --seed N                  RNG seed for reproducibility\n"
        "  --queue N                 per-direction queue capacity in packets, default 4096\n"
        "  --clean-reverse           do not disturb target->client return path\n"
        "  --control PORT            UDP control port (127.0.0.1) for runtime reconfiguration:\n"
        "                            e.g. \"bw 400000\", \"wave sine\", \"loss 0.2\", \"reset\"\n"
        "  --scenario FILE           load a disturbance scenario script (time-line of\n"
        "                            loss/delay/reorder/bandwidth segments, loops forever).\n"
        "                            Each line: <seconds> : cmd1 ; cmd2 ; ... (see control cmd)\n"
        "  --stats-interval S        stats print interval in seconds, default 5, 0=off\n"
        "  -h, --help                show this help\n"
        "\n"
        "Examples:\n"
        "  %s --target 192.168.1.10:9999 --loss 0.05\n"
        "  %s --target 127.0.0.1:9999 --bw-jitter-wave sine --bw-min 2000000 --bw-max 8000000 \\\n"
        "      --bw-period 2000 --delay-prob 0.1 --delay-min 100 --delay-max 300\n"
        "  %s --target 127.0.0.1:9999 --bw 1000000 --loss 0.1 --block 0.1 \\\n"
        "      --reorder-prob 0.05 --reorder-max 200\n",
        prog, prog, prog, prog);
}

bool parseDouble(const char* s, double& out) {
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s || *end != '\0') return false;
    out = v;
    return true;
}

bool parseUint(const char* s, uint64_t& out) {
    char* end = nullptr;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(
        [](DWORD type) -> BOOL {
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
                g_interrupt = 1;
                return TRUE;
            }
            return FALSE;
        },
        TRUE);
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif

    udpsim::DisturbConfig cfg;
    std::string listen = "0.0.0.0:5555";
    std::string target;
    std::string scenarioPath;
    double statsInterval = 5.0;
    uint16_t controlPort = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", name);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--target") {
            target = next("--target");
        } else if (a == "--listen") {
            listen = next("--listen");
        } else if (a == "--loss") {
            double v; if (!parseDouble(next("--loss"), v)) { fprintf(stderr, "bad --loss\n"); return 1; }
            cfg.loss_rate = v;
        } else if (a == "--block") {
            double v; if (!parseDouble(next("--block"), v)) { fprintf(stderr, "bad --block\n"); return 1; }
            cfg.block_rate = v;
        } else if (a == "--dup") {
            double v; if (!parseDouble(next("--dup"), v)) { fprintf(stderr, "bad --dup\n"); return 1; }
            cfg.dup_rate = v;
        } else if (a == "--corrupt") {
            double v; if (!parseDouble(next("--corrupt"), v)) { fprintf(stderr, "bad --corrupt\n"); return 1; }
            cfg.corrupt_rate = v;
        } else if (a == "--delay-prob") {
            double v; if (!parseDouble(next("--delay-prob"), v)) { fprintf(stderr, "bad --delay-prob\n"); return 1; }
            cfg.delay_enabled = true;
            cfg.delay_prob = v;
        } else if (a == "--delay-min") {
            uint64_t v; if (!parseUint(next("--delay-min"), v)) { fprintf(stderr, "bad --delay-min\n"); return 1; }
            cfg.delay_enabled = true;
            cfg.delay_min_ms = (uint32_t)v;
        } else if (a == "--delay-max") {
            uint64_t v; if (!parseUint(next("--delay-max"), v)) { fprintf(stderr, "bad --delay-max\n"); return 1; }
            cfg.delay_enabled = true;
            cfg.delay_max_ms = (uint32_t)v;
        } else if (a == "--bw") {
            uint64_t v; if (!parseUint(next("--bw"), v)) { fprintf(stderr, "bad --bw\n"); return 1; }
            cfg.bw_enabled = true;
            cfg.bw_min_bps = cfg.bw_max_bps = (double)v;
        } else if (a == "--bw-jitter-wave") {
            std::string w = next("--bw-jitter-wave");
            if (w == "rect") cfg.wave = udpsim::DisturbConfig::Wave::Rect;
            else if (w == "sine") cfg.wave = udpsim::DisturbConfig::Wave::Sine;
            else if (w == "sawtooth") cfg.wave = udpsim::DisturbConfig::Wave::Sawtooth;
            else if (w == "random") cfg.wave = udpsim::DisturbConfig::Wave::Random;
            else { fprintf(stderr, "bad --bw-jitter-wave: %s\n", w.c_str()); return 1; }
        } else if (a == "--bw-min") {
            uint64_t v; if (!parseUint(next("--bw-min"), v)) { fprintf(stderr, "bad --bw-min\n"); return 1; }
            cfg.bw_min_bps = (double)v;
            cfg.bw_enabled = true;
        } else if (a == "--bw-max") {
            uint64_t v; if (!parseUint(next("--bw-max"), v)) { fprintf(stderr, "bad --bw-max\n"); return 1; }
            cfg.bw_max_bps = (double)v;
            cfg.bw_enabled = true;
        } else if (a == "--bw-period") {
            uint64_t v; if (!parseUint(next("--bw-period"), v)) { fprintf(stderr, "bad --bw-period\n"); return 1; }
            cfg.bw_period_ms = (uint32_t)v;
        } else if (a == "--bw-duty") {
            double v; if (!parseDouble(next("--bw-duty"), v)) { fprintf(stderr, "bad --bw-duty\n"); return 1; }
            cfg.bw_duty = v;
        } else if (a == "--reorder-prob") {
            double v; if (!parseDouble(next("--reorder-prob"), v)) { fprintf(stderr, "bad --reorder-prob\n"); return 1; }
            cfg.reorder_enabled = true;
            cfg.reorder_prob = v;
        } else if (a == "--reorder-max") {
            uint64_t v; if (!parseUint(next("--reorder-max"), v)) { fprintf(stderr, "bad --reorder-max\n"); return 1; }
            cfg.reorder_enabled = true;
            cfg.reorder_max_ms = (uint32_t)v;
        } else if (a == "--seed") {
            uint64_t v; if (!parseUint(next("--seed"), v)) { fprintf(stderr, "bad --seed\n"); return 1; }
            cfg.seed = v;
        } else if (a == "--queue") {
            uint64_t v; if (!parseUint(next("--queue"), v)) { fprintf(stderr, "bad --queue\n"); return 1; }
            cfg.queue_cap = (size_t)v;
        } else if (a == "--clean-reverse") {
            cfg.clean_reverse = true;
        } else if (a == "--l4s") {
            cfg.l4s_enabled = true;
        } else if (a == "--l4s-threshold") {
            double v; if (!parseDouble(next("--l4s-threshold"), v)) { fprintf(stderr, "bad --l4s-threshold\n"); return 1; }
            cfg.l4s_threshold_ms = v;
        } else if (a == "--control") {
            uint64_t v; if (!parseUint(next("--control"), v)) { fprintf(stderr, "bad --control\n"); return 1; }
            controlPort = (uint16_t)v;
        } else if (a == "--stats-interval") {
            double v; if (!parseDouble(next("--stats-interval"), v)) { fprintf(stderr, "bad --stats-interval\n"); return 1; }
            statsInterval = v;
        } else if (a == "--scenario") {
            scenarioPath = next("--scenario");
        } else {
            fprintf(stderr, "unknown option: %s (use --help)\n", a.c_str());
            return 1;
        }
    }

    if (target.empty()) {
        fprintf(stderr, "--target IP:PORT is required (use --help)\n");
        return 1;
    }

    udpsim::UdpProxy proxy(listen, target, cfg, statsInterval, controlPort);
    if (!scenarioPath.empty()) {
        std::string err;
        if (!proxy.set_scenario(scenarioPath, err)) {
            fprintf(stderr, "failed to load scenario: %s\n", err.c_str());
            return 1;
        }
    }
    std::string err;
    if (!proxy.start(err)) {
        fprintf(stderr, "failed to start proxy: %s\n", err.c_str());
        return 1;
    }

    printf("UDP weak-network proxy running\n");
    printf("  listen : %s   ->   target: %s\n", listen.c_str(), target.c_str());
    if (cfg.bw_enabled)
        printf("  bandwidth: wave=%s range=[%.0f,%.0f] bps period=%ums duty=%.2f\n",
               cfg.wave == udpsim::DisturbConfig::Wave::Rect ? "rect" :
               cfg.wave == udpsim::DisturbConfig::Wave::Sine ? "sine" :
               cfg.wave == udpsim::DisturbConfig::Wave::Sawtooth ? "sawtooth" : "random",
               cfg.bw_min_bps, cfg.bw_max_bps, cfg.bw_period_ms, cfg.bw_duty);
    printf("  loss=%.2f block=%.2f delay=%s[%.0f-%.0fms,p=%.2f] dup=%.2f corrupt=%.2f reorder=%s[p=%.2f,max=%ums]\n",
           cfg.loss_rate, cfg.block_rate,
           cfg.delay_enabled ? "on" : "off", (double)cfg.delay_min_ms, (double)cfg.delay_max_ms, cfg.delay_prob,
           cfg.dup_rate, cfg.corrupt_rate,
           cfg.reorder_enabled ? "on" : "off", cfg.reorder_prob, cfg.reorder_max_ms);
    printf("Press Ctrl+C to stop.\n");

    while (!g_interrupt) {
#ifdef _WIN32
        Sleep(100);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    }

    proxy.stop();
    printf("\nproxy stopped\n");
    return 0;
}
