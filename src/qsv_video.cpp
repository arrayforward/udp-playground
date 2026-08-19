#include "qsv_video.hpp"

#include <cstdio>
#include <cstring>

namespace video {

namespace {
constexpr const char* kFfmpegPath = "D:\\tools\\udp-sim\\ffmpeg\\bin\\ffmpeg.exe";

// Annex-B startcode detection
bool is_startcode(const std::uint8_t* p, std::size_t n, std::size_t& len) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { len = 4; return true; }
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) { len = 3; return true; }
    return false;
}

bool nal_is_idr(const std::uint8_t* p, std::size_t n) {
    // Skip startcode, take low 5 bits of the NAL header
    std::size_t sc = 0;
    if (!is_startcode(p, n, sc)) return false;
    if (n <= sc) return false;
    std::uint8_t type = p[sc] & 0x1F;
    return type == 5;   // 5 = IDR slice
}

}  // namespace

QsvVideoEncoder::QsvVideoEncoder() = default;
QsvVideoEncoder::~QsvVideoEncoder() { shutdown(); }

bool QsvVideoEncoder::start(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                            std::uint32_t bitrate_bps) {
    m_width = width;
    m_height = height;
    m_fps = fps;
    return spawn(bitrate_bps);
}

bool QsvVideoEncoder::spawn(std::uint32_t bitrate_bps) {
    kill_child();
    HANDLE r_in = INVALID_HANDLE_VALUE, w_in = INVALID_HANDLE_VALUE;
    HANDLE r_out = INVALID_HANDLE_VALUE, w_out = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&r_in, &w_in, &sa, 0) || !CreatePipe(&r_out, &w_out, &sa, 0)) {
        fprintf(stderr, "[qsv] pipe create failed\n");
        return false;
    }
    SetHandleInformation(w_in, HANDLE_FLAG_INHERIT, 0);   // child keeps read end only
    SetHandleInformation(r_out, HANDLE_FLAG_INHERIT, 0);  // child keeps write end only

    // QSV rate control: target -b:v, peak -maxrate, buffer -bufsize; GOP 30
    // frames (1s), -preset veryfast low latency. stdin is rawvideo NV12.
    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "\"%s\" -hide_banner -loglevel error "
        "-f rawvideo -pix_fmt nv12 -s %ux%u -r %u -i pipe:0 "
        "-c:v h264_qsv -b:v %u -maxrate %u -bufsize %u -g 30 -keyint_min 30 "
        "-preset veryfast -f h264 pipe:1",
        kFfmpegPath, m_width, m_height, m_fps,
        bitrate_bps / 1000, bitrate_bps * 3 / 2000, bitrate_bps / 500);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = r_in;
    si.hStdOutput = w_out;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(r_in);
    CloseHandle(w_out);
    if (!ok) {
        CloseHandle(w_in);
        CloseHandle(r_out);
        fprintf(stderr, "[qsv] spawn failed\n");
        return false;
    }
    CloseHandle(pi.hThread);
    m_proc = pi.hProcess;
    m_stdin_w = w_in;
    m_stdout_r = r_out;
    m_bitrate_bps = bitrate_bps;
    m_buf.clear();
    printf("[qsv] encoder started: %ux%u@%u %ukbps\n", m_width, m_height, m_fps,
           bitrate_bps / 1000);
    fflush(stdout);
    return true;
}

void QsvVideoEncoder::kill_child() {
    // 优雅退出优先：stdin EOF → ffmpeg flush 编码器后自行退出（干净释放
    // QSV 设备会话）；1s 内未退出再强杀。强杀后 QSV 会话未释放，立即
    // 拉起新实例会 device failed(-17)（实测重启风暴后全部失败、视频停）。
    if (m_stdin_w) { CloseHandle(m_stdin_w); m_stdin_w = nullptr; }
    if (m_stdout_r) { CloseHandle(m_stdout_r); m_stdout_r = nullptr; }
    if (m_proc) {
        if (WaitForSingleObject(m_proc, 1000) != WAIT_OBJECT_0) {
            TerminateProcess(m_proc, 0);
            WaitForSingleObject(m_proc, 500);
        }
        CloseHandle(m_proc);
        m_proc = nullptr;
    }
    // 设备会话释放等待：强杀路径下驱动回收 QSV 句柄需要时间
    Sleep(300);
}

void QsvVideoEncoder::shutdown() {
    std::lock_guard<std::mutex> lk(m_mu);
    kill_child();
}

bool QsvVideoEncoder::write_frame(const std::uint8_t* nv12, std::size_t size) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_proc || !m_stdin_w || !nv12) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(m_stdin_w, nv12, (DWORD)size, &written, nullptr);
    if (!ok || written != size) {
        // ffmpeg exited (pipe broken); caller respawns via force_keyframe
        fprintf(stderr, "[qsv] stdin write failed\n");
        return false;
    }
    return true;
}

bool QsvVideoEncoder::read_frame(std::vector<std::uint8_t>& out, bool& keyframe) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_proc || !m_stdout_r) return false;
    // 1) Collect available stdout data (non-blocking)
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(m_stdout_r, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail == 0) break;
        std::size_t old = m_buf.size();
        m_buf.resize(old + avail);
        DWORD got = 0;
        if (!ReadFile(m_stdout_r, m_buf.data() + old, avail, &got, nullptr) || got == 0) return false;
        m_buf.resize(old + got);
    }
    if (m_buf.size() < 4) return false;
    // 2) Find the first startcode
    std::size_t i = 0;
    std::size_t sc1 = 0;
    for (; i + 3 < m_buf.size(); ++i) {
        std::size_t len = 0;
        if (is_startcode(m_buf.data() + i, m_buf.size() - i, len)) { sc1 = len; break; }
    }
    if (i + 3 >= m_buf.size()) {
        // Not enough data for an AU header: wait for more
        if (m_buf.size() > 4096) m_buf.clear();  // garbage without startcode
        return false;
    }
    // 3) Find the next SLICE boundary (end of AU): skip parameter-set NALs
    // (SPS/PPS/SEI/AUD, types 6/7/8/9) -- they belong to the following slice
    // frame. Splitting them off polluted frame-size stats and FEC planning
    // (measured avg 2.2KB slices -> data_count=2 -> budget fec=300% ->
    // on-wire overshoot). AU = [i, slice start).
    std::size_t j = i + sc1;
    for (; j + 3 < m_buf.size();) {
        std::size_t len = 0;
        if (!is_startcode(m_buf.data() + j, m_buf.size() - j, len)) { ++j; continue; }
        std::size_t hdr = j + len;
        if (hdr + 1 > m_buf.size()) { ++j; continue; }
        std::uint8_t type = m_buf[hdr] & 0x1F;
        if (type == 6 || type == 7 || type == 8 || type == 9) {
            j = hdr + 1;   // parameter set: skip this NAL, keep scanning
            continue;
        }
        break;   // slice start: AU boundary
    }
    if (j + 3 >= m_buf.size()) return false;   // slice incomplete
    out.assign(m_buf.data() + i, m_buf.data() + j);
    keyframe = nal_is_idr(out.data(), out.size());
    m_buf.erase(m_buf.begin(), m_buf.begin() + j);
    return true;
}

bool QsvVideoEncoder::set_bitrate(std::uint32_t bitrate_bps) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (bitrate_bps == m_bitrate_bps) return true;
    return spawn(bitrate_bps);
}

bool QsvVideoEncoder::force_keyframe() {
    std::lock_guard<std::mutex> lk(m_mu);
    // Restart the child: the first frame of the new process is an IDR
    if (!m_proc) return false;
    return spawn(m_bitrate_bps);
}

bool QsvVideoEncoder::alive() const {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_proc) return false;
    DWORD code = 0;
    return GetExitCodeProcess(m_proc, &code) && code == STILL_ACTIVE;
}

}  // namespace video
