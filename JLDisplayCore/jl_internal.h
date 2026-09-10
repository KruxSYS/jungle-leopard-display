// jl_internal.h — shared internals of the core library. Not installed; the
// calibration survey and the player both drive ffmpeg the same way, and this is
// where that machinery lives.

#pragma once

#include "jl_core.h"

// Declared rather than included: only the two files that actually talk to WIC
// pull in wincodec.h, and a pointer needs no more than this.
struct IWICImagingFactory;

namespace jl {
    namespace detail {

        // -------------------------------------------------------------------
        // Small helpers
        // -------------------------------------------------------------------

        std::wstring Widen(const std::string& s);
        bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out);

        // FNV-1a as 16 hex digits. Both cache keys — calibration and packs —
        // are built from it, so they invalidate on exactly the same events.
        std::string Fnv1aHex(const std::string& s);

        // Pulls width/height out of a JPEG's SOFn marker.
        bool JpegSize(const std::vector<uint8_t>& j, int& w, int& h);

        // True only for the one JPEG flavour the panel's decoder accepts:
        // baseline SOF0, three components, 4:2:0 chroma subsampling.
        bool JpegIsBaseline420(const std::vector<uint8_t>& j);

        uint16_t Sum16(const std::vector<uint8_t>& b, size_t from, size_t to);

        // -------------------------------------------------------------------
        // Framing
        // -------------------------------------------------------------------

        // Mirrors handleData(): 55 AA | len(u16 LE) | cmd | payload | sum(u16 LE)
        std::vector<uint8_t> BuildFrame(uint8_t command, const uint8_t* payload = nullptr,
            size_t payloadLen = 0);

        // Image frames use a DIFFERENT envelope: u32 LE length, raw JPEG, u16 LE sum.
        std::vector<uint8_t> BuildImageFrame(const std::vector<uint8_t>& jpeg);

        // -------------------------------------------------------------------
        // ffmpeg process plumbing
        // -------------------------------------------------------------------

        struct FfmpegPipe {
            HANDLE process = nullptr;
            HANDLE readEnd = nullptr;

            void Close();
        };

        bool StartFfmpegPipe(const std::wstring& cmdline, FfmpegPipe& pipe);

        // Runs a command to completion with no window. True on exit code 0.
        bool RunHidden(const std::wstring& cmdline, DWORD timeoutMs = 60000);

        // Pulls one complete JPEG (SOI..EOI) out of the stream, refilling as
        // needed. False at end of stream.
        bool ReadNextJpeg(FfmpegPipe& pipe, std::vector<uint8_t>& acc,
            std::vector<uint8_t>& frame);

        // Fills `dst` with exactly `bytes` bytes of raw frame. False at end of
        // stream, which includes a truncated final frame — a partial frame is
        // not a frame.
        bool ReadRawFrame(FfmpegPipe& pipe, uint8_t* dst, size_t bytes);

        // Reads a child process's entire stdout as text.
        std::string ReadAllText(FfmpegPipe& pipe);

        // -------------------------------------------------------------------
        // Filter and command construction
        // -------------------------------------------------------------------

        // Rotate BEFORE scaling, so the result is always 960x480 whatever the
        // angle. transpose=1 is clockwise, transpose=2 counter-clockwise;
        // hflip+vflip is an exact, cheap 180 with no resampling.
        std::wstring BuildFilter(bool stretch, int rotate);

        // -------------------------------------------------------------------
        // Frame pacing
        //
        // Streaming and pack playback differ only in where the frames come
        // from; the timing, the keep-alive and the resync after a stall are the
        // same problem and live here once.
        // -------------------------------------------------------------------

        // Timing runs on QueryPerformanceCounter, and the wait is a
        // high-resolution waitable timer rather than Sleep.
        //
        // Both halves of that matter and neither is premature. GetTickCount only
        // advances every ~15.6 ms however fine Sleep's resolution happens to be,
        // so computing the next slot from it quantised every frame onto a tick
        // boundary: measured over 300 frames at 30 fps, gaps came out bimodal at
        // 31 ms and 46 ms — 16.8 ms of jitter, half a frame period, with 14% of
        // frames landing more than 5 ms out. That is visible on the panel on its
        // own, at any bitrate, and it is why fixing the data rate alone did not
        // stop the stutter.
        //
        // The slot is also computed from a frame counter rather than by adding a
        // rounded period, because 1000/30 is 33 and the missing third of a
        // millisecond is a drift of one whole frame every 100 seconds.
        struct Pacer {
            Pacer() = default;
            ~Pacer();

            Pacer(const Pacer&) = delete;
            Pacer& operator=(const Pacer&) = delete;

            void Start(int fps);

            // Waits until this frame's slot, then reports the time it woke at as
            // milliseconds since Start. A caller more than half a second late is
            // resynced rather than left trying to catch up frame by frame.
            DWORD WaitForSlot();

            // Milliseconds since Start, for callers that just want the elapsed
            // time without waiting for anything.
            DWORD ElapsedMs() const;

            // Live mode lapses without this; false means the device went away.
            bool KeepAliveIfDue(Device& device, DWORD now);

        private:
            void WaitUntil(long long targetCount);

            long long freq_ = 0;      // QPC ticks per second
            long long origin_ = 0;      // QPC count at Start
            long long frame_ = 0;      // slots handed out so far
            long long slip_   = 0;      // time absorbed by resyncs, kept out of elapsed
            int       fps_ = 30;
            DWORD     lastKeepAlive_ = 0;
            HANDLE    timer_ = nullptr;
            bool      raisedTimerPeriod_ = false;
        };

        // ffmpeg's own "-hwaccel auto" only considers methods needing no explicit
        // device setup, so on Windows it lands on d3d11va or nothing and never
        // picks CUDA. Resolve "auto" ourselves against what the build offers.
        //
        // Note this reports what was COMPILED IN, not whether the hardware is
        // present and working; ffmpeg falls back to software if the device fails.
        std::wstring ResolveHwaccel(const std::wstring& ffmpeg, const std::wstring& requested);

        // `startSeconds` seeks the input before decoding; `seconds` caps how much
        // is decoded after that.
        std::wstring VideoCommand(const std::wstring& ffmpeg, const std::wstring& input,
            const std::wstring& filter, const std::wstring& hwaccel,
            int fps, int quality, bool loop, bool paced,
            double seconds, bool keyframesOnly = false, double startSeconds = 0.0);

        // A live network source. Separate from VideoCommand rather than four
        // more parameters on it — that one already takes eleven — and because
        // what a stream needs is genuinely different: caller-supplied input
        // options (a URL that 403s without the right user agent is the normal
        // case), reconnection, and no audio decode at all.
        std::wstring StreamCommand(const std::wstring& ffmpeg, const std::wstring& url,
            const std::wstring& inputArgs, const std::wstring& filter,
            const std::wstring& hwaccel, int fps, int quality, double startSeconds);

        // The same framing and rate conversion, but ffmpeg hands over raw BGR
        // and does no encoding: the per-frame pack builder needs to choose a
        // quality per frame, which nothing on an ffmpeg command line can do.
        std::wstring RawVideoCommand(const std::wstring& ffmpeg, const std::wstring& input,
            const std::wstring& filter, const std::wstring& hwaccel, int fps);

        // -------------------------------------------------------------------
        // JPEG encoding (jl_jpeg.cpp)
        //
        // Everything that is not straight out of ffmpeg is encoded here: the
        // overlay compositor's re-encode after a blend, and the pack builder's
        // per-frame quality search. They share a file because they share the
        // constraint that actually matters — this panel decodes BASELINE 4:2:0
        // and nothing else, and draws anything else as a smeared picture rather
        // than reporting an error.
        //
        // The factory and the COM apartment belong to the calling thread. Bind
        // and unbind are counted, so a builder that binds inside a thread the
        // host already bound does not pull the factory out from under it.
        // -------------------------------------------------------------------

        bool WicBindThread();
        void WicUnbindThread();
        IWICImagingFactory* WicFactory();

        // Encodes a kPanelWidth x kPanelHeight 24bpp BGR buffer as a baseline
        // 4:2:0 JPEG. `quality` is WIC's 0..1 ImageQuality, which is unrelated
        // to ffmpeg's -q:v.
        bool EncodeJpeg(const uint8_t* bgr, float quality, std::vector<uint8_t>& out);

        // The ladder the per-frame search walks, best step last. A fixed ladder
        // rather than a bisection of the float range: it terminates in a known
        // number of encodes, and it makes the quality a frame gets a pure
        // function of that frame's pixels — which the pack builder's dedupe
        // depends on, since two identical frames must encode to identical bytes.
        int   QualityStepPercent(int step);
        int   QualityStepForPercent(int percent);

        // Encodes at the best step no higher than `ceilingStep` whose output
        // still fits `target`. Falls back to the floor when nothing fits, and
        // fails only if even that is over `hardCap`. `encodes` reports what the
        // search cost.
        bool EncodeJpegToFit(const uint8_t* bgr, int ceilingStep, size_t target,
            size_t hardCap, std::vector<uint8_t>& out, int& usedStep, int& encodes);

        // -------------------------------------------------------------------
        // Calibration cache
        //
        // The chosen quality depends only on the video's content and the filter
        // chain, so it is worth remembering. The key includes the file's size and
        // last-write time, so re-encoding or replacing a video invalidates its
        // entry automatically. Shared with the CLI: same file, same format.
        // -------------------------------------------------------------------

        std::wstring CacheDirectory();
        std::wstring CacheFilePath();
        std::string  CalibrationKey(const std::wstring& input, const std::wstring& filter,
            size_t sizeTarget);
        int  CacheLookup(const std::string& key);
        void CacheStore(const std::string& key, int quality);

        // Finds the lowest -q:v whose frames all fit, by encoding every KEYFRAME
        // in the file. Returns -1 if even the worst quality overflows, or -2 if
        // `abort` asked to stop partway.
        int CalibrateQuality(const std::wstring& ffmpeg, const std::wstring& input,
            const std::wstring& filter, const std::wstring& hwaccel,
            AbortFn abort, void* abortUser);

    }  // namespace detail
}  // namespace jl
