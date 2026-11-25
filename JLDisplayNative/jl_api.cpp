// jl_api.cpp — the single process-wide session behind jl_api.h.
//
// The shape here is dictated by two facts. The device is a blocking serial
// handle driven by a child ffmpeg process, so nothing it does can be made
// non-blocking; and a UI thread must never block. So every content request
// becomes: stop the current worker, start a new one, return. The caller learns
// what happened by polling.

#include "jl_api.h"

#include "jl_core.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

// The managed side (Interop/NativeMethods.cs) copies these structs by layout,
// and VerifyLayout() checks ITS sizes against these same figures at startup.
// That only catches a change made on the C# side; these catch one made here.
// Change a struct and both of these, and VerifyLayout, move together - see
// AGENTS.md.
static_assert(sizeof(JlRenderOpts) == 100, "JlRenderOpts changed: update NativeMethods.cs and VerifyLayout()");
static_assert(sizeof(JlStatus) == 1184, "JlStatus changed: update NativeMethods.cs and VerifyLayout()");
static_assert(sizeof(JlMediaStatus) == 1832, "JlMediaStatus changed: update NativeMethods.cs and VerifyLayout()");

namespace {

    // -----------------------------------------------------------------------

    struct Guard {
        CRITICAL_SECTION& cs;
        explicit Guard(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
        ~Guard() { LeaveCriticalSection(&cs); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    enum class ItemKind { None, Image, Video, Stream };

    struct Session {
        jl::Device  device;

        // Lives for the process, like the device: the host pushes surfaces into
        // it from its render thread whether or not anything is playing, and each
        // worker binds its own WIC state to it while it runs.
        jl::Overlay overlay;

        // Two locks, never held together in the other order. `cmd` serialises
        // whole operations (stop-then-start); `state` guards only the fields the
        // poller reads, so the worker can publish progress without ever blocking
        // on an operation in flight.
        CRITICAL_SECTION cmd{};
        CRITICAL_SECTION state{};

        HANDLE            worker = nullptr;
        std::atomic<bool> abortItem{ false };
        std::atomic<bool> shuttingDown{ false };

        // The ffmpeg the play loop is currently reading from, and a lock that
        // makes terminating it safe.
        //
        // Stopping an item sets abortItem and waits for the worker — but the
        // worker spends its time blocked in ReadFile on this child's pipe, and
        // checks the flag only between frames. A source that has stopped
        // producing frames therefore never notices, and the wait is as long as
        // ffmpeg's own. Killing the child ends the read at once and the worker
        // unwinds through its ordinary path.
        //
        // Held whenever the handle is read or replaced, so a terminate cannot
        // land on a handle the worker is closing.
        CRITICAL_SECTION child{};
        HANDLE           childProcess = nullptr;

        // A pending seek in seconds, or negative for none. Written by whichever
        // thread the host calls from and taken by the playback loop at its next
        // frame; an unconsumed request is simply replaced, so dragging a
        // timeline leaves only the position the user let go of.
        std::atomic<double> seekTo{ -1.0 };

        // Read once by each worker as it starts, so changing the mode never
        // disturbs an item already running. Atomic rather than under `cmd`
        // because the host sets it from its UI thread whenever the user picks a
        // different option.
        std::atomic<int32_t> preprocess{ JL_PRE_OFF };

        // Worker input. Written under `cmd` before the thread starts, read only
        // by that thread, so it needs no lock of its own.
        std::wstring   itemPath;
        jl::RenderOpts itemOpts;
        ItemKind       itemKind = ItemKind::None;

        // Only for ItemKind::Stream. Not part of RenderOpts on purpose: a URL
        // and the headers it needs are not rendering options, and RenderOpts is
        // key material for the calibration and pack caches — putting a signed
        // URL that changes every play into a cache key would make every entry a
        // miss.
        jl::StreamSource itemStream;

        // Guarded by `state`.
        //
        // `connected` is a mirror of device.IsOpen() rather than a call to it.
        // Asking the device directly would mean taking its lock while holding
        // `state`, and the core takes `state` (through the log sink) while
        // holding the device lock — the inversion is a real deadlock.
        bool     connected = false;
        int32_t  st = JL_STATE_DISCONNECTED;
        int64_t  framesSent = 0;
        int64_t  framesDropped = 0;
        double   fps = 0.0;
        double   positionSeconds = 0.0;
        double   durationSeconds = 0.0;
        int32_t  finishedCount = 0;
        int32_t  frameCount = 0;
        std::wstring message;
        std::wstring error;
        std::wstring port;
        std::vector<uint8_t> lastFrame;
        DWORD    lastFrameAt = 0;
        DWORD    itemStartedAt = 0;   // for the running fps average

        // Resolved once per process and reused: asking ffmpeg what it supports
        // costs a process launch, and the answer cannot change while we run.
        //
        // Under its own lock, because three threads want it: the UI thread
        // starting an item (under `cmd`), and the device-free jl_preprocess and
        // jl_pack_ready, which take no session lock on purpose and are called
        // from background threads at the same time. Unlocked, two of them could
        // write the string at once. See ResolvedAuto().
        CRITICAL_SECTION hw{};
        std::wstring hwaccelResolved;
        bool         hwaccelKnown = false;

        // How far the jl_preprocess call in flight has got, in frames, or -1
        // when there is none. Atomic rather than under `state` because it is
        // written from whatever background thread the host preprocesses on and
        // polled from the UI thread, and it belongs to neither the session nor
        // the item being played.
        std::atomic<int64_t> preprocessFrames{ -1 };
        std::atomic<int64_t> preprocessBytes{ 0 };

        // The last thing a preprocess said for itself. Guarded by `state` like
        // the other strings, and deliberately not `message`: that one belongs
        // to whatever is on the panel, and a build runs behind it.
        std::wstring preprocessMessage;
        bool         preprocessHadProblem = false;

        Session()
        {
            InitializeCriticalSection(&cmd);
            InitializeCriticalSection(&state);
            InitializeCriticalSection(&child);
            InitializeCriticalSection(&hw);
        }
    };

    Session g;

    // -----------------------------------------------------------------------
    // Status helpers
    // -----------------------------------------------------------------------

    void SetState(int32_t s)
    {
        Guard lock(g.state);
        g.st = s;
    }

    void SetError(const std::wstring& text)
    {
        Guard lock(g.state);
        g.error = text;
        g.st = JL_STATE_ERROR;
    }

    // The device stopped answering — unplugged, or the port went away. The
    // handle is not closed here: the worker cannot take the command lock, and
    // the next jl_open tears everything down anyway. Reporting it is enough for
    // the host to start reconnecting.
    void SetDeviceLost(const std::wstring& text)
    {
        Guard lock(g.state);
        g.error = text;
        g.st = JL_STATE_ERROR;
        g.connected = false;
    }

    // The worker's log sink. Errors are sticky until the next item starts;
    // everything else is the transient "what is it doing right now" line.
    void WorkerLog(jl::LogLevel level, const wchar_t* text, void*)
    {
        Guard lock(g.state);
        if (level == jl::LogLevel::Error) g.error = text;
        else                              g.message = text;
    }

    // The log sink for a preprocess. Warnings and errors are what a caller
    // needs — "ffmpeg exited with code 1 partway through" is the whole answer to
    // why a pack did not appear — so those stick and progress chatter does not.
    void PreprocessLog(jl::LogLevel level, const wchar_t* text, void*)
    {
        if (level == jl::LogLevel::Progress) return;

        Guard lock(g.state);

        // The FIRST problem, not the last. The last one is always the outermost
        // complaint - "the pack just written would not load" - and the reason it
        // would not is in the warning before it, which overwriting threw away.
        const bool problem = level == jl::LogLevel::Warn || level == jl::LogLevel::Error;

        if (problem && !g.preprocessHadProblem) {
            g.preprocessMessage = text;
            g.preprocessHadProblem = true;
        }
        else if (!problem && !g.preprocessHadProblem) {
            g.preprocessMessage = text;
        }
    }

    void CopyTo(wchar_t* dst, size_t cch, const std::wstring& src)
    {
        if (!dst || cch == 0) return;
        size_t n = src.size() < cch - 1 ? src.size() : cch - 1;
        if (n) memcpy(dst, src.c_str(), n * sizeof(wchar_t));
        dst[n] = L'\0';
    }

    // -----------------------------------------------------------------------
    // Worker
    // -----------------------------------------------------------------------

    bool ShouldAbort(void*)
    {
        return g.abortItem.load() || g.shuttingDown.load();
    }

    // Called for every frame that reaches the panel. The counters are cheap and
    // have to be exact, so they update every time; copying the frame itself is
    // up to 80 KB and only a preview, so that part is throttled to 5 Hz.
    void OnFrame(const std::vector<uint8_t>& jpeg, void*)
    {
        const DWORD now = GetTickCount();

        Guard lock(g.state);
        ++g.framesSent;

        const double elapsed = (now - g.itemStartedAt) / 1000.0;
        if (elapsed > 0.0) g.fps = g.framesSent / elapsed;

        if (now - g.lastFrameAt >= 200) {
            g.lastFrameAt = now;
            g.lastFrame = jpeg;
            ++g.frameCount;
        }
    }

    // Consumes the pending seek, so one request moves playback exactly once.
    // Called by the play loop as each ffmpeg starts and ends.
    void OnChild(void* processHandle, void*)
    {
        Guard lock(g.child);
        g.childProcess = (HANDLE)processHandle;
    }

    double TakeSeek(void*)
    {
        return g.seekTo.exchange(-1.0);
    }

    void OnPosition(double seconds, double duration, void*)
    {
        Guard lock(g.state);
        g.positionSeconds = seconds;
        g.durationSeconds = duration;
    }

    // The two hooks every playback loop takes. Kept as free functions rather
    // than lambdas so the Compositor stays a plain C-callable pair.
    bool ComposeFrame(std::vector<uint8_t>& jpeg, void*)
    {
        return g.overlay.Compose(jpeg);
    }

    uint32_t OverlayVersion(void*)
    {
        return g.overlay.Version();
    }

    // Always installed, never conditional on the overlay being on right now:
    // the user can enable it in the middle of a two-hour video and expect it to
    // appear. Costing nothing when off is Overlay::Compose's job — it is two
    // atomic loads and a return — not something to decide once per item.
    jl::Compositor MakeCompositor()
    {
        jl::Compositor c;
        c.compose = ComposeFrame;
        c.version = OverlayVersion;
        return c;
    }

    void PublishFrame(const std::vector<uint8_t>& jpeg)
    {
        Guard lock(g.state);
        g.lastFrameAt = GetTickCount();
        g.lastFrame = jpeg;
        ++g.frameCount;
    }

    // A still's overlay redraw. Deliberately not OnFrame: that one counts
    // frames sent and computes a running fps, and a still has never reported
    // either. All that has changed is which pixels the preview should show.
    void OnStillFrame(const std::vector<uint8_t>& jpeg, void*)
    {
        PublishFrame(jpeg);
    }

    // Whether a "stream" is really a file sitting on this machine.
    //
    // The music follower's videos are: it downloads them and then plays them
    // through the stream path for one reason, its start offset, which the video
    // path has no way to express. So the objections to preprocessing a stream —
    // no file to key a pack on, a survey that would fetch the source twice —
    // do not apply to them, and this is how that case is told apart.
    bool IsLocalFile(const std::wstring& path)
    {
        if (path.empty()) return false;
        if (path.find(L"://") != std::wstring::npos) return false;

        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    // The host's render options as the core wants them, validated, with
    // "auto" hwaccel resolved once for the process.
    //
    // Shared by the two device-free entry points — jl_preprocess and
    // jl_pack_ready — because they have to agree with each other exactly: the
    // pack cache keys on these, so a field translated differently in one of
    // them means asking about a pack nobody built.
    // What "auto" hwaccel means on this machine, decided once. The first
    // caller launches ffmpeg to ask; everyone else waits for that answer
    // rather than asking again or reading a string half-written.
    std::wstring ResolvedAuto()
    {
        Guard lock(g.hw);
        if (!g.hwaccelKnown) {
            g.hwaccelResolved = jl::ResolveHwaccel(jl::FindFfmpeg(), L"auto");
            g.hwaccelKnown = true;
        }
        return g.hwaccelResolved;
    }

    bool OptsFromApi(const JlRenderOpts& in, jl::RenderOpts& out)
    {
        out.stretch = in.stretch != 0;
        out.rotate = in.rotate;
        out.fps = in.fps;
        out.quality = in.quality;
        out.recalibrate = in.recalibrate != 0;
        out.perFrameQuality = in.perFrameQuality != 0;
        out.qualityCeiling = in.qualityCeiling;
        out.byteRate = in.byteRate;

        wchar_t hw[32];
        CopyTo(hw, _countof(hw), in.hwaccel);
        out.hwaccel = hw;
        if (out.hwaccel == L"none") out.hwaccel.clear();

        std::wstring problem;
        if (!jl::ValidateOpts(out, problem)) return false;

        if (out.hwaccel == L"auto") out.hwaccel = ResolvedAuto();

        return true;
    }

    jl::Preprocess CurrentMode()
    {
        switch (g.preprocess.load()) {
        case JL_PRE_MEMORY: return jl::Preprocess::Memory;
        case JL_PRE_DISK:   return jl::Preprocess::Disk;
        default:            return jl::Preprocess::Off;
        }
    }

    // The tail every video path shares, whether its frames came from a pack or
    // straight off ffmpeg's pipe.
    void FinishPlayback(bool ok, const jl::PlaybackStats& stats)
    {
        {
            // framesSent already counted live in OnFrame; taking it from stats
            // too would be the same number, but dropped frames never reach
            // OnFrame so they can only come from here.
            Guard lock(g.state);
            g.framesDropped = (int64_t)stats.dropped;
            if (stats.fps > 0.0) g.fps = stats.fps;
        }

        if (ShouldAbort(nullptr)) return;

        if (!ok) {
            SetDeviceLost(L"playback stopped: the device or ffmpeg failed");
            return;
        }

        // Reached the end on its own. A looping item only gets here if it was
        // stopped, which the abort check above already returned for.
        Guard lock(g.state);
        ++g.finishedCount;
        g.st = JL_STATE_IDLE;
    }

    DWORD WINAPI WorkerProc(LPVOID)
    {
        jl::SetLogSink(WorkerLog, nullptr);   // per-thread; see jl_core.h

        // WIC objects and the COM apartment they live in belong to this thread.
        // Binding costs nothing when no overlay is ever enabled, and unbinding
        // on every exit path is what keeps the apartment from outliving it.
        g.overlay.BindThread();
        struct Unbind {
            ~Unbind() { g.overlay.UnbindThread(); }
        } unbind;

        const jl::Compositor comp = MakeCompositor();

        const std::wstring   path = g.itemPath;
        const ItemKind       kind = g.itemKind;
        jl::RenderOpts       opts = g.itemOpts;
        const jl::Preprocess mode = CurrentMode();

        if (kind == ItemKind::Image) {
            SetState(JL_STATE_PREPARING);

            std::vector<uint8_t> jpeg;
            if (!jl::PrepareImageCached(path, opts, mode, jpeg)) {
                if (!ShouldAbort(nullptr)) SetState(JL_STATE_ERROR);
                return 0;
            }
            if (ShouldAbort(nullptr)) return 0;

            SetState(JL_STATE_PLAYING);
            PublishFrame(jpeg);

            // A still has no natural end: hold it until something else is asked
            // for. finishedCount deliberately does not advance — dwell timing is
            // the host's playlist decision, not ours.
            if (!g.device.HoldStill(jpeg, ShouldAbort, nullptr, &comp,
                    OnStillFrame, nullptr) && !ShouldAbort(nullptr))
                SetDeviceLost(L"lost the device while holding a still");
            else if (!ShouldAbort(nullptr))
                SetState(JL_STATE_IDLE);

            return 0;
        }

        if (kind == ItemKind::Stream) {
            jl::StreamSource stream = g.itemStream;

            // A live URL cannot be packed and cannot be calibrated: nothing to
            // key either on, and a survey would fetch the whole thing a second
            // time. Streaming is the only mode it has.
            //
            // A downloaded music video is not that, and playing it as though it
            // were cost the feature the two things preprocessing is for. Its
            // frames were transcoded live at a fixed guess of a quality — which
            // a busy video beats, and the frames it makes too big are dropped
            // rather than sent, which is what "the video skips" is. And every
            // seek restarted ffmpeg to land on a keyframe, where a pack seeks
            // by index, exactly, for nothing.
            //
            // Disk regardless of the mode, and adopt rather than build: the
            // host preprocesses this file before it asks for playback (see
            // jl_preprocess), a memory pack cannot survive that gap, and a
            // build inside this call would hold the panel on the card for as
            // long as it took. Not there — a fresh download, a full cache — and
            // streaming still works.
            if (mode != jl::Preprocess::Off && IsLocalFile(path)) {
                jl::FramePack pack;

                if (jl::GetPack(path, opts, jl::Preprocess::Disk, pack,
                        ShouldAbort, nullptr, nullptr, /*adoptOnly*/ true)) {
                    if (ShouldAbort(nullptr)) return 0;

                    SetState(JL_STATE_PLAYING);

                    jl::PlaybackStats stats;
                    bool ok = jl::PlayPack(g.device, pack, opts, ShouldAbort, nullptr,
                        OnFrame, nullptr, &stats, TakeSeek, nullptr, OnPosition, nullptr,
                        &comp, stream.startAt);

                    FinishPlayback(ok, stats);
                    return 0;
                }

                if (ShouldAbort(nullptr)) return 0;
            }

            SetState(JL_STATE_PLAYING);

            jl::PlaybackStats stats;
            bool ok = jl::PlayVideo(g.device, path, opts, ShouldAbort, nullptr,
                OnFrame, nullptr, &stats, TakeSeek, nullptr, OnPosition, nullptr,
                &comp, &stream, OnChild, nullptr);

            FinishPlayback(ok, stats);
            return 0;
        }

        if (kind == ItemKind::Video) {
            // Resolve the quality separately from playing it, so the two states
            // are distinguishable: calibration can take a minute on a long file
            // and the UI has to be able to say so. Doing it here rather than
            // inside each path also means the pack builder and the streamer
            // agree on the answer by construction.
            //
            // Returns false only when it failed or was cancelled; the state is
            // already set by then.
            auto calibrate = [&] {
                if (opts.quality > 0) return true;

                SetState(JL_STATE_CALIBRATING);

                const int quality = jl::ResolveQuality(path, opts, ShouldAbort, nullptr);
                if (quality == -2 || ShouldAbort(nullptr)) return false;   // cancelled
                if (quality < 0) {
                    SetState(JL_STATE_ERROR);
                    return false;
                }
                opts.quality = quality;   // whatever runs next skips calibration
                return true;
            };

            // A per-frame pack needs no calibrated figure at all, and a survey
            // costs a pass over the file — so it is deferred until something
            // actually wants one, which now means only the streaming fallback.
            const bool perFramePack = mode != jl::Preprocess::Off &&
                opts.perFrameQuality && opts.quality <= 0;

            if (!perFramePack && !calibrate()) return 0;

            // The pack has to outlive playback: PlayPack reads straight out of
            // it, and for a disk pack that means straight out of the mapping.
            jl::FramePack pack;

            if (mode != jl::Preprocess::Off) {
                SetState(JL_STATE_PREPROCESSING);

                if (jl::GetPack(path, opts, mode, pack, ShouldAbort, nullptr, nullptr)) {
                    if (ShouldAbort(nullptr)) return 0;

                    SetState(JL_STATE_PLAYING);

                    jl::PlaybackStats stats;
                    bool ok = jl::PlayPack(g.device, pack, opts, ShouldAbort, nullptr,
                        OnFrame, nullptr, &stats, TakeSeek, nullptr, OnPosition, nullptr,
                        &comp);

                    FinishPlayback(ok, stats);
                    return 0;
                }

                if (ShouldAbort(nullptr)) return 0;

                // Could not be packed — over the memory budget, or the cache
                // would not take it. GetPack has already said why; streaming
                // still works, so fall through to it rather than failing.
            }

            // Streaming commits to one figure for the whole run, so this is
            // where a per-frame item finally pays for a survey — and only if it
            // got this far, which is the uncommon path.
            if (!calibrate()) return 0;

            SetState(JL_STATE_PLAYING);

            jl::PlaybackStats stats;
            bool ok = jl::PlayVideo(g.device, path, opts, ShouldAbort, nullptr,
                OnFrame, nullptr, &stats, TakeSeek, nullptr, OnPosition, nullptr,
                &comp, nullptr, OnChild, nullptr);

            FinishPlayback(ok, stats);
            return 0;
        }

        return 0;
    }

    // Both must be called with `cmd` held.

    void StopWorkerLocked()
    {
        g.abortItem.store(true);

        // Before the wait, not after: the whole point is to stop the worker
        // blocking on a child that has nothing to say. Terminating one that was
        // about to exit anyway costs nothing — the frames it had already sent
        // are on the panel, and the loop treats a closed pipe as the end of a
        // pass either way.
        {
            Guard lock(g.child);
            if (g.childProcess) {
                TerminateProcess(g.childProcess, 0);
                g.childProcess = nullptr;
            }
        }

        if (g.worker) {
            WaitForSingleObject(g.worker, INFINITE);
            CloseHandle(g.worker);
            g.worker = nullptr;
        }
        g.abortItem.store(false);
    }

    int32_t StartWorkerLocked(const wchar_t* path, const JlRenderOpts* opts, ItemKind kind,
        const jl::StreamSource* stream = nullptr)
    {
        if (!path || !*path || !opts) return JL_ERR_BAD_ARGS;
        if (!g.device.IsOpen()) return JL_ERR_NOT_OPEN;

        jl::RenderOpts o;
        o.stretch = opts->stretch != 0;
        o.rotate = opts->rotate;
        o.fps = opts->fps;
        o.quality = opts->quality;
        o.loop = opts->loop != 0;
        o.recalibrate = opts->recalibrate != 0;
        o.perFrameQuality = opts->perFrameQuality != 0;
        o.qualityCeiling = opts->qualityCeiling;
        o.byteRate = opts->byteRate;

        wchar_t hw[32];
        CopyTo(hw, _countof(hw), opts->hwaccel);
        o.hwaccel = hw;
        if (o.hwaccel == L"none") o.hwaccel.clear();

        std::wstring problem;
        if (!jl::ValidateOpts(o, problem)) {
            SetError(problem);
            return JL_ERR_BAD_ARGS;
        }

        // Resolve "auto" once for the life of the session rather than launching
        // ffmpeg to ask the same question for every item.
        if (o.hwaccel == L"auto") o.hwaccel = ResolvedAuto();

        StopWorkerLocked();

        // The compositor answers to the same data rate the pack builder does.
        // It has to be told: it sees frames, not options, and steering on the
        // panel's frame cap instead let an overlay spend nearly half again what
        // the setting allowed.
        g.overlay.SetBudget(jl::FrameBudget(o.byteRate, o.fps));

        {
            Guard lock(g.state);
            g.error.clear();
            g.message.clear();
            g.framesSent = 0;
            g.framesDropped = 0;
            g.fps = 0.0;
            g.positionSeconds = 0.0;
            g.durationSeconds = 0.0;
            g.itemStartedAt = GetTickCount();
        }

        // A seek aimed at the item being replaced must not land on the new one.
        g.seekTo.store(-1.0);

        g.itemPath = path;
        g.itemOpts = o;
        g.itemKind = kind;
        g.itemStream = stream ? *stream : jl::StreamSource{};

        g.worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
        if (!g.worker) {
            SetError(L"could not start the playback thread");
            return JL_ERR_FAILED;
        }
        return JL_OK;
    }

}  // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

int32_t jl_find_port(wchar_t* buf, int32_t cch)
{
    std::wstring port = jl::FindPort();
    CopyTo(buf, (size_t)cch, port);
    return port.empty() ? JL_ERR_FAILED : JL_OK;
}

int32_t jl_open(const wchar_t* port)
{
    Guard lock(g.cmd);

    StopWorkerLocked();
    g.device.Close();

    std::wstring problem;
    if (!g.device.Open(port ? port : L"", problem)) {
        Guard s(g.state);
        g.error = problem;
        g.st = JL_STATE_DISCONNECTED;
        g.connected = false;
        g.port.clear();
        return JL_ERR_FAILED;
    }

    g.device.Clear();

    {
        Guard s(g.state);
        g.error.clear();
        g.message.clear();
        g.port = g.device.Port();
        g.connected = true;
        g.st = JL_STATE_IDLE;
    }
    return JL_OK;
}

void jl_close(void)
{
    Guard lock(g.cmd);

    StopWorkerLocked();
    if (g.device.IsOpen()) {
        g.device.FlushEoi();
        g.device.Close();
    }

    Guard s(g.state);
    g.st = JL_STATE_DISCONNECTED;
    g.connected = false;
    g.port.clear();
    g.lastFrame.clear();
}

int32_t jl_get_info(wchar_t* buf, int32_t cch)
{
    Guard lock(g.cmd);
    if (!g.device.IsOpen()) return JL_ERR_NOT_OPEN;

    std::string body;
    if (!g.device.GetInfo(body)) return JL_ERR_FAILED;

    // The panel answers in ASCII JSON, so a straight widening is exact.
    std::wstring wide(body.begin(), body.end());
    CopyTo(buf, (size_t)cch, wide);
    return JL_OK;
}

int32_t jl_set_brightness(int32_t percent)
{
    // Deliberately not under `cmd`: brightness must work while an item is
    // playing. Device methods are serialised internally, so the command lands
    // between two frames rather than inside one.
    if (!g.device.IsOpen()) return JL_ERR_NOT_OPEN;
    return g.device.SetBrightness(percent) ? JL_OK : JL_ERR_FAILED;
}

int32_t jl_show_image(const wchar_t* path, const JlRenderOpts* opts)
{
    Guard lock(g.cmd);
    return StartWorkerLocked(path, opts, ItemKind::Image);
}

int32_t jl_play_video(const wchar_t* path, const JlRenderOpts* opts)
{
    Guard lock(g.cmd);
    return StartWorkerLocked(path, opts, ItemKind::Video);
}

int32_t jl_play_stream(const wchar_t* url, const wchar_t* inputArgs,
    double startSeconds, double durationSeconds, const JlRenderOpts* opts)
{
    Guard lock(g.cmd);

    jl::StreamSource stream;
    stream.inputArgs = inputArgs ? inputArgs : L"";
    stream.startAt = startSeconds > 0 ? startSeconds : 0.0;
    stream.duration = durationSeconds > 0 ? durationSeconds : 0.0;

    // A stream normally has no calibrated figure, so it commits to the one
    // calibration lands on for nearly everything anyway — about 37 KB a frame,
    // which at 30 fps is kDefaultByteRate.
    //
    // "Normally", because -q:v 3 is a guess and not a guarantee: a busy source
    // encodes frames over kMaxJpegBytes at it, and those are dropped rather
    // than sent, which is visible as the picture skipping. Music videos are
    // where that showed up, and they are also the case where the guess is
    // unnecessary — the follower downloads the video before playing it, so
    // there is a local file to calibrate and a path to key the answer on. A
    // caller that has done so passes the figure in opts->quality and gets
    // frames that are known to fit.
    stream.quality = opts && opts->quality > 0 ? opts->quality : 3;

    return StartWorkerLocked(url, opts, ItemKind::Stream, &stream);
}

int32_t jl_calibrate(const wchar_t* path, const JlRenderOpts* opts)
{
    if (!path || !*path || !opts) return JL_ERR_BAD_ARGS;

    // Touches no device state and takes no session lock, so a host can pre-warm
    // several videos on background threads while something else is playing.
    // Blocking by design: the caller already has a thread to spare.
    jl::RenderOpts o;
    o.stretch = opts->stretch != 0;
    o.rotate = opts->rotate;
    o.fps = opts->fps;
    o.quality = 0;                     // calibrating an explicit quality is a no-op
    o.recalibrate = opts->recalibrate != 0;

    wchar_t hw[32];
    CopyTo(hw, _countof(hw), opts->hwaccel);
    o.hwaccel = hw;
    if (o.hwaccel == L"none") o.hwaccel.clear();

    std::wstring problem;
    if (!jl::ValidateOpts(o, problem)) return JL_ERR_BAD_ARGS;

    // Only shutdown can cancel this one; it has no item to be replaced by.
    auto abortOnShutdown = [](void*) -> bool { return g.shuttingDown.load(); };

    int quality = jl::ResolveQuality(path, o, abortOnShutdown, nullptr);
    return quality > 0 ? quality : JL_ERR_FAILED;
}

int32_t jl_preprocess_frames(void)
{
    return (int32_t)g.preprocessFrames.load();
}

int32_t jl_preprocess_message(wchar_t* buf, int32_t cch)
{
    Guard lock(g.state);
    CopyTo(buf, (size_t)cch, g.preprocessMessage);
    return g.preprocessMessage.empty() ? JL_ERR_FAILED : JL_OK;
}

int32_t jl_pack_ready(const wchar_t* path, const JlRenderOpts* opts)
{
    if (!path || !*path || !opts) return JL_ERR_BAD_ARGS;

    jl::RenderOpts o;
    if (!OptsFromApi(*opts, o)) return JL_ERR_BAD_ARGS;

    // Adopt-only, so this asks the question without answering it the expensive
    // way. Closed again immediately: the point is whether playback will find a
    // pack, not to hold one open here.
    jl::FramePack pack;
    auto abortOnShutdown = [](void*) -> bool { return g.shuttingDown.load(); };

    bool have = jl::GetPack(path, o, jl::Preprocess::Disk, pack,
        abortOnShutdown, nullptr, nullptr, /*adoptOnly*/ true);
    pack.Close();

    return have ? JL_OK : JL_ERR_FAILED;
}

int32_t jl_preprocess(const wchar_t* path, const JlRenderOpts* opts)
{
    if (!path || !*path || !opts) return JL_ERR_BAD_ARGS;

    // The same bargain as jl_calibrate, one step further: no device state, no
    // session lock, blocking, and safe on a background thread while something
    // else is on the panel. What it leaves behind is a pack in the disk cache,
    // which is the only place one can wait between being built and being
    // played — so this is how a host that wants its frames ready before it
    // asks for playback gets them there.
    jl::RenderOpts o;
    if (!OptsFromApi(*opts, o)) return JL_ERR_BAD_ARGS;

    auto abortOnShutdown = [](void*) -> bool { return g.shuttingDown.load(); };

    // The core's log sink is per-thread, and this thread had none — so every
    // diagnostic a failing build produced went nowhere, and "it came to nothing"
    // was all anybody could be told. See jl_preprocess_message.
    {
        Guard lock(g.state);
        g.preprocessMessage.clear();
        g.preprocessHadProblem = false;
    }
    jl::SetLogSink(PreprocessLog, nullptr);

    // Published for jl_preprocess_frames, so a host can say how far this has
    // got rather than leaving a minute of silence. Cleared on every exit path.
    g.preprocessFrames.store(0);
    g.preprocessBytes.store(0);
    struct Done {
        ~Done()
        {
            g.preprocessFrames.store(-1);
            g.preprocessBytes.store(0);
        }
    } done;

    auto onProgress = [](size_t frames, uint64_t bytes, void*) {
        g.preprocessFrames.store((int64_t)frames);
        g.preprocessBytes.store((int64_t)bytes);
    };

    // Closed again immediately. The pack is wanted on disk, not in this
    // caller's hands: playback opens its own view of the same file, and holding
    // a mapping here would only keep the eviction sweep off it.
    jl::FramePack pack;
    bool ok = jl::GetPack(path, o, jl::Preprocess::Disk, pack,
        abortOnShutdown, nullptr, nullptr, /*adoptOnly*/ false,
        onProgress, nullptr);
    pack.Close();

    if (ok) return JL_OK;

    // GetPack ends by mapping what it built, and reports failure if it cannot.
    // That is right for a caller about to play the thing and wrong for this
    // one: what was asked for was a pack in the cache, and if one is there and
    // loads, the job is done however this call went. A minute of encoding is
    // not worth discarding over the last step of it.
    jl::FramePack check;
    bool onDisk = jl::GetPack(path, o, jl::Preprocess::Disk, check,
        abortOnShutdown, nullptr, nullptr, /*adoptOnly*/ true);
    check.Close();

    return onDisk ? JL_OK : JL_ERR_FAILED;
}

void jl_stop(void)
{
    Guard lock(g.cmd);
    StopWorkerLocked();

    if (g.device.IsOpen()) g.device.FlushEoi();

    g.seekTo.store(-1.0);

    Guard s(g.state);
    if (g.st != JL_STATE_DISCONNECTED) g.st = JL_STATE_IDLE;
    g.message.clear();
    g.positionSeconds = 0.0;
    g.durationSeconds = 0.0;
}

void jl_seek(double seconds)
{
    // No lock and no worker check: the request is a single atomic, and the
    // playback loop is the only thing that reads it. Taking g.cmd here would
    // block the UI thread behind whatever the worker is doing, which for a
    // dragged timeline is exactly the wrong trade.
    if (seconds < 0.0) seconds = 0.0;
    g.seekTo.store(seconds);
}

void jl_get_status(JlStatus* out)
{
    if (!out) return;

    Guard lock(g.state);
    out->state = g.st;
    out->connected = g.connected ? 1 : 0;
    out->framesSent = g.framesSent;
    out->framesDropped = g.framesDropped;
    out->fps = g.fps;
    out->positionSeconds = g.positionSeconds;
    out->durationSeconds = g.durationSeconds;
    out->finishedCount = g.finishedCount;
    out->frameCount = g.frameCount;
    CopyTo(out->port, _countof(out->port), g.port);
    CopyTo(out->message, _countof(out->message), g.message);
    CopyTo(out->error, _countof(out->error), g.error);

    // Read straight off the overlay rather than mirrored into the session:
    // these are its own atomics and plain arithmetic, and nothing here needs
    // them to agree with the fields above to the exact frame.
    const jl::Overlay::Stats ov = g.overlay.Snapshot();
    out->overlayComposeMs = ov.composeMs;
    out->overlayEncodeMs = ov.encodeMs;
    out->overlayOutBytes = ov.outBytes;
    out->overlayQuality = (int32_t)(ov.quality * 100.0f + 0.5f);
    out->overlayDrops = (int32_t)ov.drops;
    out->overlayOverBudget = (int32_t)ov.overBudget;
    out->overlayReserved = 0;
}

int32_t jl_get_last_frame(uint8_t* buf, int32_t cap)
{
    Guard lock(g.state);
    const int32_t need = (int32_t)g.lastFrame.size();
    if (need == 0) return 0;
    if (!buf || cap < need) return need;   // asking for the size, or too small
    memcpy(buf, g.lastFrame.data(), (size_t)need);
    return need;
}

void jl_set_preprocess(int32_t mode)
{
    // Anything unrecognised means Off. A host built against a later header must
    // not be able to turn this into an unplayable state.
    if (mode != JL_PRE_MEMORY && mode != JL_PRE_DISK) mode = JL_PRE_OFF;
    g.preprocess.store(mode);
}

int32_t jl_get_preprocess(void)
{
    return g.preprocess.load();
}

void jl_set_pack_budgets(int64_t memoryBytes, int64_t diskBytes)
{
    // A negative figure is nonsense rather than "unlimited"; treat it as 0,
    // which the core reads as "put the default back".
    jl::SetMemoryBudget(memoryBytes > 0 ? (uint64_t)memoryBytes : 0);
    jl::SetDiskBudget(diskBytes > 0 ? (uint64_t)diskBytes : 0);
}

int64_t jl_memory_budget(void)
{
    return (int64_t)jl::MemoryBudget();
}

int64_t jl_disk_budget(void)
{
    return (int64_t)jl::DiskBudget();
}

int64_t jl_pack_cache_bytes(void)
{
    return (int64_t)jl::PackCacheBytes();
}

void jl_pack_cache_clear(void)
{
    jl::PackCacheClear();
}

// ---------------------------------------------------------------------------
// Overlay
//
// Deliberately independent of the session's command lock. The host renders on
// its own thread and pushes surfaces whether or not anything is playing, and
// making that wait on a stop-then-start would stall the render thread behind
// whatever ffmpeg happens to be doing.
// ---------------------------------------------------------------------------

void jl_overlay_set_enabled(int32_t on)
{
    g.overlay.SetEnabled(on != 0);
}

int32_t jl_overlay_update(const uint8_t* bgraPremultiplied, int32_t w, int32_t h)
{
    if (!bgraPremultiplied) return JL_ERR_BAD_ARGS;
    return g.overlay.Update(bgraPremultiplied, w, h) ? JL_OK : JL_ERR_BAD_ARGS;
}

void jl_overlay_clear(void)
{
    g.overlay.Clear();
}

int32_t jl_ffmpeg_path(wchar_t* buf, int32_t cch)
{
    std::wstring path = jl::FindFfmpeg();
    CopyTo(buf, (size_t)cch, path);
    return path.empty() ? JL_ERR_FAILED : JL_OK;
}

// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        // Signal only. Joining a thread from under the loader lock deadlocks, so
        // a well-behaved host calls jl_close before unloading; this just makes
        // sure a worker that is still running stops touching the device.
        g.shuttingDown.store(true);
        g.abortItem.store(true);
    }
    return TRUE;
}
