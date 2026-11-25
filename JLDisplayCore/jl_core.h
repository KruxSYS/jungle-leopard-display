// jl_core.h — public interface of the Jungle Leopard display core.
//
// Everything the CLI and the tray app share lives behind this header: device
// discovery, the wire protocol, ffmpeg transcoding, and the video calibration
// cache. Nothing in here writes to stdout; diagnostics go to a log sink the
// caller installs, so the same code serves a console tool and a GUI.
//
// Protocol recovered from the vendor Electron app (resources/app.asar,
// main/_baseClass/device.js). Verified against firmware version 3.1,
// model TXW818-ST7701S-5.5inch-hor, 960x480.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace jl {

    // -----------------------------------------------------------------------
    // Protocol constants
    //
    // The full opcode table, not just the three this code sends. Each comment
    // is the condition the vendor app checks before it will send that command,
    // read out of main/_baseClass/device.js and evaluated against this panel
    // (TXW818-ST7701S-5.5inch-hor, firmware 3.1). A command marked "not ours"
    // is one the vendor app would refuse to send here at all.
    //
    // OtaBegin and SetSerialNum are recorded and deliberately not implemented.
    // One flashes firmware and the other rewrites the panel identity and
    // reboots, and neither is gated away from this hardware.
    // -----------------------------------------------------------------------

    namespace cmd {
        constexpr uint8_t Restart = 0x01;
        constexpr uint8_t SetLight = 0x03;
        constexpr uint8_t GetDeviceInfo = 0x06;
        constexpr uint8_t OtaBegin = 0x0C;  // F2 FF + u32 LE size, then a raw .bin
        constexpr uint8_t Live = 0x11;  // start / hold live mode
        constexpr uint8_t SetMotionBeforeOff = 0x14;
        constexpr uint8_t SetMotionTimeout = 0x15;  // version >= 2.8
        constexpr uint8_t SetRegion = 0x20;  // UTF-8 string; this is what arms SetMotor
        constexpr uint8_t Close = 0x21;  // version >= 3.1, so ours
        constexpr uint8_t SetSerialNum = 0x23;  // rewrites the serial, then reboots
        constexpr uint8_t SetMotor = 0x25;  // region == "ycc28_v1", not ours
        constexpr uint8_t SetRealTimeTimeout = 0x26;  // version >= 4.1, not ours
    }

    constexpr DWORD  kLiveKeepAliveMs = 1500;        // vendor app's interval
    constexpr DWORD  kStillRefreshMs = 250;         // re-send rate for a held still
    constexpr size_t kMaxJpegBytes = 80 * 1024;   // maxSize for this panel
    constexpr size_t kSafeJpegBytes = 64 * 1024;   // calibration target, leaves headroom
    constexpr int    kPanelWidth = 960;
    constexpr int    kPanelHeight = 480;

    // -----------------------------------------------------------------------
    // Per-frame quality
    //
    // Calibration picks ONE -q:v for a whole file, set by its busiest frame, so
    // every simple frame in that file is encoded far worse than it needed to be.
    // A preprocessed pack has no reason to accept that: nothing about the panel
    // requires two frames to have been encoded the same way, so the builder can
    // give each frame the best quality it can afford under kSafeJpegBytes.
    //
    // The ceiling is what stops that being wasteful. Past it a frame is
    // spending kilobytes on a difference nobody can see at 960x480, and pack
    // size is the whole cost of this feature. Percent of the encoder's range;
    // it is not a -q:v and does not map onto one.
    //
    // 84 is the default because it was measured, not guessed. On a 30 s clip
    // whose worst keyframe calibrated to -q:v 3, against the source frames:
    //
    //     -q:v 3 (uniform)   32 MB   SSIM 0.911 - 0.954
    //     ceiling 84         43 MB   SSIM 0.982 - 0.991   1.3 encodes/frame
    //     ceiling 92         49 MB   SSIM 0.983 - 0.993   3.9 encodes/frame
    //
    // The step from uniform to 84 is the whole of the visible difference. The
    // step from 84 to 92 costs 14% more disk and three times the encodes for
    // the fourth decimal place, so it is offered and not taken by default.
    // -----------------------------------------------------------------------

    constexpr int kDefaultQualityCeiling = 84;
    constexpr int kMinQualityCeiling = 50;
    constexpr int kMaxQualityCeiling = 98;

    // The other half of the per-frame question, and the one that actually
    // decides whether the panel keeps up.
    //
    // kSafeJpegBytes is a per-frame MAXIMUM, not a rate. Calibration never had
    // to think about the difference: -q:v stops at 3, which on this panel's
    // 960x480 lands around 37 KB a frame, so the data rate came out at roughly
    // 1.1 MB/s whatever the target said. Filling the 64 KB target on every
    // frame instead pushes that past 1.4 MB/s, and the panel stutters.
    //
    // It is not the link. Measured on this hardware, writes flat out sustain
    // 3.05 MB/s with no single frame's write blocking longer than 20 ms, so the
    // host can push far more than the panel can draw. What runs out is the
    // panel's own decode: a bigger JPEG takes its controller longer, and past
    // some size per frame it stops finishing 30 of them a second.
    //
    // Nothing reports that. The panel applies no backpressure — it takes the
    // bytes and falls behind quietly — so the host sees a clean 30 fps and zero
    // drops while the picture visibly hitches. The only instrument is somebody
    // looking at it, which is why this is a setting with a conservative default
    // rather than something measured at runtime.
    //
    // So a per-frame build budgets BYTES PER SECOND and divides by the frame
    // rate, and only then clamps to what one frame may be. The default is the
    // rate the app has always run at and is known to work here.
    constexpr uint32_t kDefaultByteRate = 1150u * 1024;   // per second
    constexpr uint32_t kMinByteRate = 400u * 1024;
    constexpr uint32_t kMaxByteRate = 4096u * 1024;

    // What one frame may spend. The rate comes first and kSafeJpegBytes is only
    // the ceiling on the answer.
    //
    // Inline and shared because the pack builder is no longer the only caller.
    // The overlay compositor re-encodes every frame it draws on, so it has to
    // answer to the same rate — it did not, and an overlay silently spent up to
    // twice what the setting allowed. Anything that decides how big a frame may
    // be must come through here, or the setting means nothing.
    inline size_t FrameBudget(uint32_t byteRate, int fps)
    {
        if (byteRate == 0) byteRate = kDefaultByteRate;
        if (byteRate < kMinByteRate) byteRate = kMinByteRate;
        if (byteRate > kMaxByteRate) byteRate = kMaxByteRate;

        const size_t perFrame = byteRate / (uint32_t)(fps > 0 ? fps : 30);
        return perFrame < kSafeJpegBytes ? perFrame : kSafeJpegBytes;
    }

    // -----------------------------------------------------------------------
    // Logging
    //
    // Progress messages are transient: a console sink rewrites them in place on
    // one line, a GUI sink drops them into a status label. A sink that receives
    // a non-Progress message after a Progress one is responsible for clearing
    // whatever the progress line left behind.
    //
    // The sink is PER THREAD. A host that plays on one thread while calibrating
    // on another gets two clean streams instead of one interleaved one, and a
    // thread that installs no sink is silent. Every worker thread must install
    // its own.
    // -----------------------------------------------------------------------

    enum class LogLevel { Info, Warn, Error, Progress };

    using LogFn = void (*)(LogLevel level, const wchar_t* text, void* user);

    void SetLogSink(LogFn fn, void* user);
    void Log(LogLevel level, const wchar_t* fmt, ...);

    // -----------------------------------------------------------------------
    // Render options
    //
    // These describe how a source file becomes panel-ready JPEG. Two option
    // sets that differ anywhere here produce different pixels, so the whole
    // struct feeds the calibration cache key.
    // -----------------------------------------------------------------------

    struct RenderOpts {
        bool         stretch = false;   // fill the panel, ignore aspect ratio
        int          rotate = 0;       // 0, 90, 180 or 270, applied before scaling
        int          fps = 30;      // liveRate for this panel
        int          quality = 0;       // 0 = calibrate automatically, else 2..31
        bool         loop = false;
        bool         recalibrate = false;   // ignore any cached calibration

        // Per-frame quality, honoured by the pack builder only: streaming has
        // one encoder setting for the whole run by construction. Ignored when
        // `quality` pins a -q:v, because an explicit figure means the caller
        // wants that figure everywhere.
        bool         perFrameQuality = false;
        int          qualityCeiling = 0;    // 0 = kDefaultQualityCeiling, else 50..98

        // What the link and the panel behind it can swallow, in bytes per
        // second. Divided by `fps` this becomes the per-frame byte target,
        // which is what a per-frame build spends rather than the whole of
        // kSafeJpegBytes. 0 = kDefaultByteRate.
        uint32_t     byteRate = 0;

        std::wstring hwaccel;               // empty = software, "auto" = resolve
    };

    // Returns false and writes a reason into `error` if the combination cannot
    // be rendered (bad rotation, fps or quality out of range).
    bool ValidateOpts(const RenderOpts& opts, std::wstring& error);

    // -----------------------------------------------------------------------
    // Cancellation
    //
    // Every long-running call in this library polls one of these. Nothing here
    // is interruptible by any other means: ffmpeg is a child process and the
    // port is a blocking handle, so a host that must stay responsive has to
    // pass an abort callback and run the call off its UI thread.
    // -----------------------------------------------------------------------

    // Return true to stop work at the next opportunity — checked per frame.
    using AbortFn = bool (*)(void* user);

    // How a pack build says how far it has got: the frames written so far and
    // the bytes they came to, called every few hundred milliseconds. It exists
    // because a build is the one thing in here that takes a minute with nothing
    // on screen to show for it, and a host that cannot say "43%" has to say
    // nothing at all.
    using PackProgressFn = void (*)(size_t frames, uint64_t bytes, void* user);

    // Handed the ffmpeg child's process handle as each pass starts, and nullptr
    // as it ends.
    //
    // It exists because "checked per frame" is a promise the abort cannot keep
    // on its own. The play loop spends nearly all its time blocked in ReadFile
    // on ffmpeg's pipe, and a source that stops producing frames — a stalled
    // network stream, a connection being refused — never reaches the next check
    // at all. The abort is then invisible and the caller waits for as long as
    // ffmpeg does.
    //
    // A host that holds this handle can end that wait by terminating the child:
    // the pipe closes, ReadFile returns, and the loop unwinds normally. Anything
    // that stops playback from a thread that must not block needs it.
    //
    // The handle is only valid until the next call hands over nullptr, so a
    // holder must serialise its use against that.
    using ChildFn = void (*)(void* processHandle, void* user);

    // -----------------------------------------------------------------------
    // Seeking
    //
    // A seek is a request the player collects rather than a call into it: both
    // play loops are blocking, and the only safe moment to move is between two
    // frames. The host raises one from whatever thread it likes; the loop takes
    // it at the next frame boundary.
    // -----------------------------------------------------------------------

    // Returns the position to jump to in seconds, or a negative number when no
    // seek is pending. Taking one consumes it, so a request fires exactly once.
    using SeekFn = double (*)(void* user);

    // Where playback has reached, in seconds, and how long the item runs for.
    // A duration of zero means it could not be determined — ffprobe is
    // optional, and a stream need not know its own length.
    using PositionFn = void (*)(double seconds, double duration, void* user);

    // -----------------------------------------------------------------------
    // Compositing
    //
    // The panel has no overlay plane, no alpha and no text primitive: it takes
    // whole JPEGs and nothing else. So drawing anything on top of the video
    // means decoding each outgoing frame, blending, and re-encoding it — which
    // is what a Compositor does, once per frame, on the playback thread.
    //
    // Every playback loop takes one optionally. A null Compositor, or one whose
    // overlay is disabled, is the path taken whenever nobody is drawing
    // anything, and it must cost nothing: no decode, no encode, no allocation.
    // -----------------------------------------------------------------------

    // Observes each frame as it goes out, for a live preview. Called on the
    // playback thread; keep it cheap and copy anything you keep. A frame
    // reported here is the one that actually reached the panel, overlay and
    // all, so a preview built from it shows what is really on the glass.
    using FrameFn = void (*)(const std::vector<uint8_t>& jpeg, void* user);

    // Composites the current overlay onto a panel-ready frame, in place, and
    // re-encodes it under the panel's size cap. False means the frame could not
    // be produced at any acceptable quality; the caller drops it exactly as it
    // drops an oversized one.
    using ComposeFn = bool (*)(std::vector<uint8_t>& jpeg, void* user);

    // What the overlay currently looks like, as a counter that changes whenever
    // it does. A still holds one composited frame until this moves, so a static
    // overlay on a still costs nothing after the first frame — without it, a
    // still would re-encode four times a second forever.
    using OverlayVersionFn = uint32_t (*)(void* user);

    struct Compositor {
        ComposeFn        compose = nullptr;
        OverlayVersionFn version = nullptr;
        void*            user = nullptr;

        bool Active() const { return compose != nullptr; }

        bool Apply(std::vector<uint8_t>& jpeg) const
        {
            return compose ? compose(jpeg, user) : true;
        }

        uint32_t Version() const { return version ? version(user) : 0; }
    };

    // -----------------------------------------------------------------------
    // Device
    // -----------------------------------------------------------------------

    // Walks the ports class for the panel's hardware ID. Empty if not attached.
    std::wstring FindPort();

    // A held-open serial session. Only one process on the machine can own the
    // port at a time, so this is deliberately non-copyable and explicit about
    // its lifetime — the tray app keeps exactly one alive for as long as it runs.
    //
    // Every method below is serialised against the others, at the granularity of
    // one whole message. That is the granularity that matters: a brightness
    // command arriving from the UI thread lands cleanly between two video
    // frames rather than halfway through a JPEG.
    class Device {
    public:
        Device();
        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        // `port` empty means autodetect. On failure `error` explains why, with
        // the access-denied case called out — that one means another program
        // (the vendor app, or the CLI) is holding the device.
        bool Open(const std::wstring& port, std::wstring& error);
        void Close();

        bool IsOpen() const { return h_ != INVALID_HANDLE_VALUE; }
        const std::wstring& Port() const { return port_; }

        // deviceClear(): the raw sentinels the vendor app sends before talking.
        void Clear();

        bool SendCommand(uint8_t command, const std::vector<uint8_t>& payload = {});
        bool SendImageFrame(const std::vector<uint8_t>& jpeg);
        bool ReadReply(std::string& body, DWORD timeoutMs = 3000);

        // Live mode lapses unless this is re-sent about every kLiveKeepAliveMs.
        bool KeepAlive() { return SendCommand(cmd::Live); }

        // Holds a still until `abort` says stop, by re-sending it at
        // kStillRefreshMs. The panel is a live-stream device and only draws a
        // frame once the following one arrives, so a still has to be sent
        // repeatedly rather than once. Blocking; this is what "showing a
        // picture" actually means on this panel.
        //
        // With a Compositor, the overlay is drawn onto `jpeg` and the result
        // re-sent instead — recomposited only when the overlay's version moves,
        // so the four-a-second refresh does not become four encodes a second.
        //
        // `onFrame` fires whenever that composited frame is rebuilt, not on
        // every refresh: a still sends the same bytes over and over, and only
        // the moments they change are worth telling anyone about.
        bool HoldStill(const std::vector<uint8_t>& jpeg, AbortFn abort, void* abortUser,
            const Compositor* comp = nullptr,
            FrameFn onFrame = nullptr, void* frameUser = nullptr);

        // Flushes any partial frame with a bare JPEG end-of-image marker.
        void FlushEoi();

        bool SetBrightness(int percent);
        bool GetInfo(std::string& out);

    private:
        HANDLE       h_ = INVALID_HANDLE_VALUE;
        std::wstring port_;

        // Recursive, so GetInfo can hold it across a command and its reply and
        // still call through the public methods.
        mutable CRITICAL_SECTION cs_{};
    };

    // -----------------------------------------------------------------------
    // Media
    // -----------------------------------------------------------------------

    // Transcodes any image to a 960x480 JPEG under the panel's size cap. Skips
    // ffmpeg entirely when the input already conforms and no rotation is asked
    // for. Blocking; takes up to a few seconds on a large source.
    bool PrepareImage(const std::wstring& path, const RenderOpts& opts,
        std::vector<uint8_t>& jpeg);

    // Resolves the -q:v to use for a video: an explicit opts.quality, else the
    // cached calibration, else a fresh survey (which is then cached). Returns
    // -1 if no quality keeps frames under the cap, or -2 if aborted. Blocking,
    // and a fresh survey on a long video takes tens of seconds — it reports
    // through the Progress log level throughout, and honours `abort`, so a
    // caller that cannot block must pass one.
    int ResolveQuality(const std::wstring& path, const RenderOpts& opts,
        AbortFn abort = nullptr, void* abortUser = nullptr);


    struct PlaybackStats {
        size_t sent = 0;
        size_t dropped = 0;   // frames over the panel's cap, skipped
        size_t passes = 0;   // ffmpeg runs; more than one means it looped
        double fps = 0.0;
    };

    // A live network source, for PlayVideo's last argument.
    //
    // What makes a stream different is not the transport, it is that none of
    // the machinery built around files applies to it. There is no path to key a
    // pack on, no second pass to survey for a quality, and no file to probe for
    // a length — so all three arrive from the caller instead, already decided.
    struct StreamSource {
        // Extra ffmpeg input options, verbatim: -user_agent, -headers.
        std::wstring inputArgs;

        // The -q:v to use throughout. A stream cannot be calibrated — that
        // costs a pass over the source, which here means downloading it twice —
        // so the caller commits to one figure up front.
        int quality = 3;

        // How long the source runs, from wherever the caller knows it. 0 when
        // it does not.
        double duration = 0.0;

        // Where to join, in seconds. Following something already playing is the
        // normal case, so this is usually not 0.
        double startAt = 0.0;
    };

    // Streams MJPEG out of ffmpeg and pushes each frame, pacing against the
    // wall clock and holding live mode. Blocks until the source ends, `abort`
    // returns true, or the port dies. Leaves the device in live mode with a
    // flush marker sent.
    //
    // A seek restarts ffmpeg with the new position as its input offset, which
    // is the only way to move within a stream that is being transcoded as it
    // plays. Position is counted from frames emitted since that offset, since
    // the frames arrive at a known rate by construction.
    bool PlayVideo(Device& device, const std::wstring& path, const RenderOpts& opts,
        AbortFn abort, void* abortUser,
        FrameFn onFrame, void* frameUser,
        PlaybackStats* stats,
        SeekFn takeSeek = nullptr, void* seekUser = nullptr,
        PositionFn onPosition = nullptr, void* positionUser = nullptr,
        const Compositor* comp = nullptr,
        const StreamSource* stream = nullptr,
        ChildFn onChild = nullptr, void* childUser = nullptr);

    // -----------------------------------------------------------------------
    // Preprocessing
    //
    // Everything this panel accepts is a stream of 960x480 baseline 4:2:0
    // JPEGs under kMaxJpegBytes, and what ffmpeg produces for a given source is
    // a pure function of that source's bytes and the render options — which is
    // exactly what the calibration cache already assumes. So the frames can be
    // computed once and replayed, and ffmpeg need not run during playback at
    // all.
    //
    // Off stays the fallback everywhere: a source too big for the memory
    // budget, or a pack that cannot be written, degrades to streaming rather
    // than failing.
    // -----------------------------------------------------------------------

    enum class Preprocess {
        Off = 0,      // transcode continuously while playing, as this always did
        Memory = 1,   // transcode once into RAM, then let ffmpeg exit
        Disk = 2      // transcode once into a pack file, reused across runs
    };

    // How much either mode is allowed to use. Both are settable at runtime —
    // what counts as reasonable depends entirely on the machine, and a limit
    // baked into the binary is a limit nobody can fix.
    //
    // Memory mode refuses a source that would exceed its budget, and says so,
    // which the caller turns back into streaming. Disk packs are instead
    // evicted least-recently-used to stay under theirs, so a full cache slows
    // the next play down rather than failing it.
    constexpr uint64_t kDefaultMemoryBudget = 512ull * 1024 * 1024;   // ~7 min of video
    constexpr uint64_t kDefaultDiskBudget = 8ull * 1024 * 1024 * 1024;

    // Below these, a budget is too small to hold anything useful and every item
    // would fall back to streaming — which is what Off is for. Anything lower is
    // raised to the floor rather than honoured.
    constexpr uint64_t kMinMemoryBudget = 32ull * 1024 * 1024;
    constexpr uint64_t kMinDiskBudget = 128ull * 1024 * 1024;

    // 0 restores the default. The memory budget applies to the next pack built;
    // the disk budget is enforced the next time one is written.
    void     SetMemoryBudget(uint64_t bytes);
    void     SetDiskBudget(uint64_t bytes);
    uint64_t MemoryBudget();
    uint64_t DiskBudget();

    struct PackStats {
        size_t frames = 0;          // frames in playback order
        size_t uniqueFrames = 0;    // distinct frames actually stored
        size_t bytes = 0;
        double buildSeconds = 0.0;  // 0 when it came straight from the cache
        bool   fromCache = false;
    };

    // A built sequence of panel-ready frames.
    //
    // A memory pack owns its bytes. A disk pack maps its file, so the pages stay
    // under the OS's control rather than being read wholesale into the process —
    // which is what makes a 350 MB pack of a five-minute video reasonable.
    //
    // Frame pointers stay valid until Close(), so the pack must outlive any
    // playback reading from it.
    class FramePack {
    public:
        struct Ref { uint64_t offset; uint32_t length; };

        FramePack() = default;
        ~FramePack();

        FramePack(const FramePack&) = delete;
        FramePack& operator=(const FramePack&) = delete;

        bool   IsOpen() const { return base_ != nullptr && !index_.empty(); }
        int    Fps() const { return fps_; }
        size_t FrameCount() const { return index_.size(); }
        size_t Bytes() const { return bytes_; }

        // The bytes of frame `i`, or nullptr past the end.
        const uint8_t* Frame(size_t i, size_t& len) const;

        void Close();

        // Filled in by GetPack. Public only because the builder lives in another
        // translation unit; callers have no reason to touch either.
        void AdoptMemory(std::vector<uint8_t>&& blob, std::vector<Ref>&& index, int fps);
        bool AdoptFile(const std::wstring& path);

    private:
        std::vector<uint8_t> owned_;                 // memory mode
        HANDLE               file_ = INVALID_HANDLE_VALUE;
        HANDLE               mapping_ = nullptr;
        const uint8_t*       view_ = nullptr;        // disk mode
        const uint8_t*       base_ = nullptr;        // whichever is in use
        std::vector<Ref>     index_;
        int                  fps_ = 30;
        size_t               bytes_ = 0;
    };

    // Produces a pack for `path`: loaded from the disk cache when `mode` is Disk
    // and a valid one is already there, otherwise built by running ffmpeg once
    // to completion.
    //
    // With opts.perFrameQuality the frames are encoded here, one search per
    // frame, and no calibration happens at all. Without it, calibration settles
    // one -q:v first and packed frames are byte-for-byte what streaming sends.
    //
    // False means "fall back to streaming" — over the memory budget, ffmpeg
    // failed, or `abort` fired. Blocking, and building a long video takes a
    // while; it reports throughout at the Progress log level.
    //
    // `adoptOnly` takes a pack that is already on disk and otherwise returns
    // false without building one. It is for a caller that preprocesses ahead of
    // playback and must not have a build happen inside a play call — the music
    // follower, whose panel is showing a card that it would hold for the length
    // of the build.
    bool GetPack(const std::wstring& path, const RenderOpts& opts, Preprocess mode,
        FramePack& pack, AbortFn abort, void* abortUser, PackStats* stats,
        bool adoptOnly = false,
        PackProgressFn onProgress = nullptr, void* progressUser = nullptr);

    // Plays a built pack, pacing and holding live mode exactly as PlayVideo
    // does. No child process is involved.
    //
    // Seeking here is just an index: the frames are already built and evenly
    // spaced, so a jump costs nothing and lands exactly where it was asked to.
    // `startSeconds` is the same arithmetic applied to the first pass, which is
    // how a pack joins something already in progress; later laps of a loop
    // start at the top, as they do after a seek.
    bool PlayPack(Device& device, const FramePack& pack, const RenderOpts& opts,
        AbortFn abort, void* abortUser,
        FrameFn onFrame, void* frameUser,
        PlaybackStats* stats,
        SeekFn takeSeek = nullptr, void* seekUser = nullptr,
        PositionFn onPosition = nullptr, void* positionUser = nullptr,
        const Compositor* comp = nullptr,
        double startSeconds = 0.0);

    // PrepareImage with the answer remembered, so a still in a playlist is
    // transcoded once rather than once per rotation. Off transcodes every time.
    bool PrepareImageCached(const std::wstring& path, const RenderOpts& opts,
        Preprocess mode, std::vector<uint8_t>& jpeg);

    // Size of the on-disk pack cache, and a way to empty it. Both are safe to
    // call while something is playing; a mapped pack survives its file being
    // deleted underneath it.
    uint64_t PackCacheBytes();
    void     PackCacheClear();

    // -----------------------------------------------------------------------
    // Overlay
    //
    // A 960x480 premultiplied BGRA surface, and the machinery to blend it onto
    // outgoing frames. The surface is produced elsewhere — the tray app renders
    // its layers with WPF and pushes the pixels down — because everything about
    // fonts, layout and styling belongs where the editor is, and everything
    // about JPEG belongs here.
    //
    // Measured cost at 960x480: decode ~0.9 ms, blend ~0.3 ms, encode ~0.9 ms.
    // Against a 33 ms frame budget at 30 fps that is comfortable, but only
    // because none of it happens at all when no overlay is enabled.
    // -----------------------------------------------------------------------

    class Overlay {
    public:
        Overlay();
        ~Overlay();

        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        // WIC objects and the COM apartment they live in belong to the thread
        // that composites. Every playback worker calls Bind on entry and Unbind
        // on exit; composing on an unbound thread fails rather than guessing.
        bool BindThread();
        void UnbindThread();

        void SetEnabled(bool on);
        bool Enabled() const;

        // What one composed frame may spend, from FrameBudget(). Without this
        // the compositor steers on the panel's 80 KB cap instead of the
        // configured data rate and quietly spends up to twice what the setting
        // allows — which is invisible from the host, because the panel takes
        // every byte offered and just falls behind. Set it whenever an item
        // starts; 0 restores the default-rate figure.
        void SetBudget(size_t bytesPerFrame);

        // Replaces the surface, which must be exactly kPanelWidth x
        // kPanelHeight of premultiplied BGRA. Safe from any thread, and never
        // blocks the playback thread for longer than a pointer swap.
        bool Update(const uint8_t* bgraPremultiplied, int width, int height);
        void Clear();

        // Moves whenever the surface does. See OverlayVersionFn.
        uint32_t Version() const;

        // Decode, blend, re-encode. Returns false only when the frame could not
        // be encoded under the panel's cap at any quality down to the floor.
        bool Compose(std::vector<uint8_t>& jpeg);

        struct Stats {
            double   composeMs = 0.0;   // rolling mean of decode + blend
            double   encodeMs = 0.0;   // rolling mean
            float    quality = 0.0f;  // the working quality, 0..1
            uint32_t reencodes = 0;     // frames that needed more than one encode
            uint32_t drops = 0;     // frames that would not fit at all

            // The two that make a rate bug visible. Without them the only
            // symptom of the compositor overspending is somebody noticing the
            // panel hitch: it stays at 30 fps with no drops the whole time,
            // because the panel never says it is behind.
            double   outBytes = 0.0;   // rolling mean of the composed frame
            uint32_t overBudget = 0;     // frames sent above the rate budget
        };

        Stats Snapshot() const;

    private:
        struct Impl;
        Impl* p_ = nullptr;
    };

    // -----------------------------------------------------------------------
    // ffmpeg
    // -----------------------------------------------------------------------

    // Looks beside the running executable first, then PATH. Empty if absent.
    std::wstring FindFfmpeg();
    std::wstring FindFfprobe(const std::wstring& ffmpegPath);

    // Turns "auto" into a concrete decode method by asking the ffmpeg build what
    // it supports; anything else passes through untouched. Costs a process
    // launch, so resolve once and reuse the answer rather than passing "auto"
    // into every call.
    std::wstring ResolveHwaccel(const std::wstring& ffmpegPath, const std::wstring& requested);

    // Duration in seconds, or 0 if ffprobe is missing or the file is unreadable.
    double ProbeDurationSeconds(const std::wstring& ffmpegPath, const std::wstring& input);

}  // namespace jl
