#pragma once

// H.264 hard-encode (Intel QSV) wrapper: ffmpeg.exe child process (h264_qsv).
// Input: NV12 frames (1280x720, written to stdin);
// Output: H.264 Annex-B frames (read from stdout).
// Process isolation avoids the ffmpeg-lib/MF in-process crash; the hardware
// rate controller converges in ~1 frame (MF software encoder took ~1-2s to
// converge after set_bitrate -- the root cause of drain/recover loops on
// weak links). Bitrate change / forced keyframe = restart child process
// (first frame of the new process is an IDR).
// Not thread-safe; all public methods take m_mu (bitrate loop on main thread
// vs. worker thread writing frames).

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace video {

class QsvVideoEncoder {
public:
    QsvVideoEncoder();
    ~QsvVideoEncoder();
    QsvVideoEncoder(const QsvVideoEncoder&) = delete;
    QsvVideoEncoder& operator=(const QsvVideoEncoder&) = delete;

    // Spawn the child (w x h NV12, fps, target bitrate). false on failure.
    bool start(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
               std::uint32_t bitrate_bps);

    // Write one NV12 frame (w*h*1.5 bytes) to ffmpeg stdin. Blocks until
    // ffmpeg consumes it (throttled by encode rate). false on failure
    // (process exited / write error).
    bool write_frame(const std::uint8_t* nv12, std::size_t size);

    // Read one complete H.264 AU (Annex-B). true: out holds the frame and
    // keyframe flags IDR; false: no complete frame yet (retry later).
    bool read_frame(std::vector<std::uint8_t>& out, bool& keyframe);

    // Bitrate change / forced keyframe: restart child (new IDR, hardware RC
    // applies the new bitrate immediately).
    bool set_bitrate(std::uint32_t bitrate_bps);
    bool force_keyframe();

    bool alive() const;
    std::uint32_t bitrate() const { return m_bitrate_bps; }

    void shutdown();

private:
    bool spawn(std::uint32_t bitrate_bps);
    void kill_child();

    mutable std::mutex m_mu;
    HANDLE m_proc = nullptr;
    HANDLE m_stdin_w = nullptr;
    HANDLE m_stdout_r = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_fps = 0;
    std::uint32_t m_bitrate_bps = 0;
    std::vector<std::uint8_t> m_buf;   // stdout accumulation buffer
};

}  // namespace video
