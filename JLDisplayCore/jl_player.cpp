// jl_player.cpp — streaming a video to the panel, and the frame pacing that
// pack playback shares with it.

#include "jl_internal.h"

// Only for the pre-1803 fallback in Pacer, where timeBeginPeriod is the one way
// left to get Sleep down to a millisecond.
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace jl {

    namespace detail {

        Pacer::~Pacer()
        {
            if (timer_) CloseHandle(timer_);
            if (raisedTimerPeriod_) timeEndPeriod(1);
        }

        void Pacer::Start(int fps)
        {
            LARGE_INTEGER f{}, c{};
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);

            freq_ = f.QuadPart ? f.QuadPart : 1;
            origin_ = c.QuadPart;
            frame_ = 0;
            slip_ = 0;
            fps_ = fps > 0 ? fps : 30;
            lastKeepAlive_ = 0;

            // Win10 1803 and later. The flag is the whole point: without it a
            // waitable timer is no finer than Sleep and this buys nothing.
            if (!timer_) {
                timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

                // Older than that: raise the global timer resolution instead and
                // put it back in the destructor. Never both — this one is a
                // system-wide setting and costs power everywhere.
                if (!timer_ && !raisedTimerPeriod_)
                    raisedTimerPeriod_ = timeBeginPeriod(1) == TIMERR_NOERROR;
            }
        }

        DWORD Pacer::ElapsedMs() const
        {
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return (DWORD)(1000 * (c.QuadPart - origin_) / freq_);
        }

        void Pacer::WaitUntil(long long targetCount)
        {
            for (;;) {
                LARGE_INTEGER c{};
                QueryPerformanceCounter(&c);

                const long long remain = targetCount - c.QuadPart;
                if (remain <= 0) return;

                // 100 ns units, negative for a relative wait.
                const long long hundredNs = remain * 10'000'000 / freq_;
                if (hundredNs <= 0) return;

                if (timer_) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -hundredNs;
                    if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                        WaitForSingleObject(timer_, INFINITE);
                        return;
                    }
                }

                const DWORD ms = (DWORD)(hundredNs / 10'000);
                if (ms == 0) return;   // inside a millisecond; close enough
                Sleep(ms);
                return;
            }
        }

        DWORD Pacer::WaitForSlot()
        {
            // Pace against the wall clock rather than relying on ffmpeg's -re,
            // which does not throttle reliably when writing to a pipe — and
            // which a pack has no equivalent of at all.
            //
            // The slot is derived from the frame index every time, so rounding
            // cannot accumulate: frame n is due exactly n/fps seconds in.
            ++frame_;
            const long long due = origin_ + slip_ + freq_ * frame_ / fps_;

            WaitUntil(due);

            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);

            // Fell behind by more than half a second — a stalled ffmpeg, a
            // device write that blocked. Absorb it into `slip_` rather than
            // trying to catch up frame by frame. It is deliberately separate
            // from `origin_`, so a resync moves the schedule without ever
            // walking the reported elapsed time backwards.
            if (c.QuadPart - due > freq_ / 2) slip_ += c.QuadPart - due;

            return (DWORD)(1000 * (c.QuadPart - origin_) / freq_);
        }

        bool Pacer::KeepAliveIfDue(Device& device, DWORD now)
        {
            if (now - lastKeepAlive_ < kLiveKeepAliveMs) return true;
            lastKeepAlive_ = now;
            return device.SendCommand(cmd::Live);
        }

    }  // namespace detail

    // -----------------------------------------------------------------------

    bool PlayVideo(Device& device, const std::wstring& path, const RenderOpts& opts,
        AbortFn abort, void* abortUser,
        FrameFn onFrame, void* frameUser,
        PlaybackStats* stats,
        SeekFn takeSeek, void* seekUser,
        PositionFn onPosition, void* positionUser,
        const Compositor* comp,
        const StreamSource* stream,
        ChildFn onChild, void* childUser)
    {
        auto aborted = [&] { return abort && abort(abortUser); };

        if (!device.IsOpen()) {
            Log(LogLevel::Error, L"device is not open");
            return false;
        }

        std::wstring ffmpeg = FindFfmpeg();
        if (ffmpeg.empty()) {
            Log(LogLevel::Error, L"ffmpeg.exe not found; video playback needs it");
            return false;
        }

        // Resolve "auto" once here and hand the concrete method down. Passing the
        // resolved value back through ResolveQuality is free — ResolveHwaccel only
        // spawns ffmpeg when it is actually asked to resolve "auto".
        RenderOpts resolved = opts;
        resolved.hwaccel = detail::ResolveHwaccel(ffmpeg, opts.hwaccel);

        const std::wstring filter = detail::BuildFilter(resolved.stretch, resolved.rotate);

        // A stream commits to a quality up front. Calibrating one would mean a
        // survey pass over a network source — downloading the track twice to
        // decide how to encode it — and by the time that finished the thing
        // being followed would have moved on.
        const int quality = stream
            ? stream->quality
            : ResolveQuality(path, resolved, abort, abortUser);
        if (quality < 0) return false;   // -1 unencodable, -2 cancelled
        if (aborted()) return false;

        device.SendCommand(cmd::Live);
        Sleep(100);

        Log(LogLevel::Info, L"playing at %d fps%s...",
            resolved.fps, resolved.loop ? L", looping" : L"");

        detail::Pacer pace;
        pace.Start(resolved.fps);

        size_t sent = 0, dropped = 0, passes = 0;
        bool ok = true;
        bool formatChecked = false;

        // Resolved once: whether anything is being drawn on top decides both the
        // per-frame work and where the size cap is enforced.
        const bool composing = comp && comp->Active();

        // Only worth asking once, and only if anyone is listening: it spawns
        // ffprobe, and a file that will not say how long it is will not start
        // saying so partway through.
        // ffprobe is skipped entirely for a stream: it is another request to the
        // server for something the caller already knows, and a live source may
        // not answer at all.
        const double duration = stream
            ? stream->duration
            : (onPosition ? ProbeDurationSeconds(ffmpeg, path) : 0.0);

        // Where the current ffmpeg run was told to start. Position is this plus
        // however much has been emitted since, because the frames leave ffmpeg
        // at exactly the rate we asked for.
        double startAt = stream ? stream->startAt : 0.0;
        double pendingSeek = -1.0;

        // Loop at the application level by restarting ffmpeg. -stream_loop proved
        // unreliable here, and this also recovers if ffmpeg dies mid-playback.
        do {
            if (pendingSeek >= 0.0) {
                startAt = pendingSeek;
                pendingSeek = -1.0;
            }

            detail::FfmpegPipe pipe;
            std::wstring cl = stream
                ? detail::StreamCommand(ffmpeg, path, stream->inputArgs, filter,
                    resolved.hwaccel, resolved.fps, quality, startAt)
                : detail::VideoCommand(ffmpeg, path, filter, resolved.hwaccel,
                    resolved.fps, quality, false, false, 0, false, startAt);
            if (!detail::StartFfmpegPipe(cl, pipe)) { ok = false; break; }
            ++passes;

            // Published before the read loop, so an abort raised at any point
            // during this pass has something to interrupt.
            if (onChild) onChild(pipe.process, childUser);

            size_t inPass = 0;
            std::vector<uint8_t> acc, frame;
            while (!aborted() && detail::ReadNextJpeg(pipe, acc, frame)) {
                // Between two frames is the only safe moment to move, and the
                // only way to move a transcode is to restart it at the mark.
                if (takeSeek) {
                    const double want = takeSeek(seekUser);
                    if (want >= 0.0) { pendingSeek = want; break; }
                }

                // Compose BEFORE the size check, not after. While an overlay is
                // being drawn this frame is only an intermediate — the re-encode
                // decides what actually goes on the wire, and an ffmpeg frame
                // over the cap is still perfectly decodable and worth drawing
                // on. While no overlay is being drawn Apply is a no-op and the
                // check below is exactly the one that has always been here.
                if (composing && !comp->Apply(frame)) { ++dropped; continue; }

                if (frame.size() > kMaxJpegBytes) { ++dropped; continue; }

                // Once per playback, on the first frame that will actually be
                // sent. The panel only decodes baseline 4:2:0, and a stream that
                // is not arrives as a mangled picture rather than as an error —
                // which is how GIF playback stayed broken: ffmpeg picks the
                // encoder format from the input, and a GIF decodes to bgra, so
                // mjpeg chose 4:4:4. Both encoders are pinned now — ffmpeg's by
                // -pix_fmt, the compositor's by JpegYCrCbSubsampling — so this
                // should never fire; if it does, one of those pins has been lost.
                if (!formatChecked) {
                    formatChecked = true;
                    if (!detail::JpegIsBaseline420(frame))
                        Log(LogLevel::Warn, L"frames are not baseline 4:2:0 - "
                            L"the panel will draw them wrong");
                }

                DWORD now = pace.WaitForSlot();

                if (!device.SendImageFrame(frame)) { ok = false; break; }
                ++sent;
                ++inPass;

                if (onFrame) onFrame(frame, frameUser);

                if (onPosition && resolved.fps > 0)
                    onPosition(startAt + (double)inPass / resolved.fps, duration, positionUser);

                if (!pace.KeepAliveIfDue(device, now)) { ok = false; break; }

                if ((sent % 30) == 0) {
                    double secs = now / 1000.0;
                    Log(LogLevel::Progress, L"%zu frames, %.1f fps, %zu dropped",
                        sent, secs > 0 ? sent / secs : 0.0, dropped);
                }
            }

            DWORD exitCode = 0;
            if (pipe.process) GetExitCodeProcess(pipe.process, &exitCode);

            // Withdrawn BEFORE the handle is closed. The holder takes a lock
            // around both this and its own use of the handle, so once this
            // returns nobody can be about to terminate a handle we are closing.
            if (onChild) onChild(nullptr, childUser);

            pipe.Close();

            // A seek kills ffmpeg mid-stream on purpose, so its exit code says
            // nothing about whether anything went wrong.
            if (ok && !aborted() && pendingSeek < 0.0
                && exitCode != 0 && exitCode != STILL_ACTIVE)
                Log(LogLevel::Warn, L"ffmpeg exited with code %lu", exitCode);

            // Reaching the end on a pass that had been seeked into means the
            // next lap starts at the top, not back at the mark.
            if (pendingSeek < 0.0) startAt = 0.0;

        } while (ok && !aborted() && (pendingSeek >= 0.0 || (resolved.loop && !stream)));

        const double elapsed = pace.ElapsedMs() / 1000.0;

        Log(LogLevel::Info, L"%zu frames sent over %zu pass(es)%s", sent, passes,
            dropped ? L", some dropped as oversized" : L"");

        if (stats) {
            stats->sent = sent;
            stats->dropped = dropped;
            stats->passes = passes;
            stats->fps = elapsed > 0 ? sent / elapsed : 0.0;
        }

        device.FlushEoi();
        return ok;
    }

}  // namespace jl
