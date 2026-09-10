// jl_overlay.cpp — blending a rendered overlay onto outgoing frames.
//
// The panel takes whole baseline 4:2:0 JPEGs under kMaxJpegBytes and nothing
// else: no overlay plane, no alpha, no partial update. So anything drawn on top
// of the video costs a decode, a blend and a re-encode of every frame, and this
// is where that happens.
//
// WIC does the codec work; the encoder half lives in jl_jpeg.cpp, shared with
// the pack builder, and pins the chroma subsampling the panel insists on. What
// stays here is the decode, the blend, and the rate control — the last of which
// is this file's alone: a compositor cannot know what the next frame costs, so
// it steers quality from what the last ones did.

#include "jl_internal.h"

#include <wincodec.h>

#include <algorithm>
#include <atomic>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace jl {

    namespace {

        // Quality here is WIC's 0..1 ImageQuality, which is unrelated to
        // ffmpeg's -q:v. Measured against this panel's content: 0.85 is about
        // -q:v 3, 0.55 about -q:v 7.
        constexpr float kStartQuality = 0.75f;
        constexpr float kMaxQuality = 0.92f;
        constexpr float kQualityRecover = 0.02f;

        // How far quality falls for one over-budget frame, scaled by how far
        // over it was. A flat step cannot serve both ends: 0.07 was picked when
        // the limit was the 80 KB cap and was almost never reached, so its only
        // job was to escape an emergency. The budget binds all the time, and a
        // flat 0.07 against it dived to 0.14 and spent a busy stretch at 61% of
        // the rate it was allowed — measured, not supposed. Proportional keeps
        // the lunge for the frame that is twice over and gives the frame that
        // is barely over a nudge.
        constexpr float kMinStep = 0.015f;
        constexpr float kMaxStep = 0.10f;

        // 0.30 reads like a floor and is not one: a dense 960x480 frame still
        // encodes to 73 KB there. The pack builder's ladder was extended down
        // for exactly this reason and this floor moves with it, or a busy scene
        // under an overlay has nowhere to go and parks over budget.
        constexpr float kMinQuality = 0.05f;

        // Frames comfortably under the budget before quality is nudged back up.
        // Slow enough that a busy scene cannot make this oscillate, but 90 was
        // far too slow once the budget started binding: recovering the 0.33 a
        // single hard scene cost took the better part of two minutes, so the
        // picture stayed soft long after the content stopped being hard.
        constexpr int   kRecoverAfter = 20;
        constexpr float kRecoverFraction = 0.85f;

        constexpr int kMaxEncodeAttempts = 4;

        // Until a playback worker says otherwise. Matches what FrameBudget gives
        // for the default rate at 30 fps, so an overlay composed before any
        // opts arrive is not briefly allowed to spend the whole 80 KB.
        constexpr size_t kFallbackBudget = kDefaultByteRate / 30;

        template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

        struct Guard {
            CRITICAL_SECTION& cs;
            explicit Guard(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
            ~Guard() { LeaveCriticalSection(&cs); }
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;
        };

        double NowMs()
        {
            static LARGE_INTEGER freq = [] {
                LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f;
            }();
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return freq.QuadPart ? 1000.0 * c.QuadPart / freq.QuadPart : 0.0;
        }

        // A rolling mean that needs no history buffer. The diagnostics only ever
        // want "roughly what is this costing", not a distribution.
        void Roll(double& mean, double sample)
        {
            mean = mean == 0.0 ? sample : mean * 0.95 + sample * 0.05;
        }

    }  // namespace

    // -----------------------------------------------------------------------

    struct Overlay::Impl {
        // Two surfaces and an index. The producer fills the back one outside the
        // lock and swaps under it, so the playback thread never waits on a
        // render and never sees a half-written overlay.
        std::vector<uint8_t> surface[2];
        int                  front = 0;
        CRITICAL_SECTION     cs{};

        std::atomic<bool>     enabled{ false };
        std::atomic<uint32_t> version{ 0 };
        std::atomic<bool>     hasSurface{ false };

        // Bounding box of the front surface's non-zero alpha, so the blend
        // touches only the rows and columns that can possibly change.
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

        // Reused across frames: allocating 1.4 MB per frame at 30 fps is the
        // difference between this being cheap and not.
        std::vector<uint8_t> pixels;   // decoded frame, 24bpp BGR
        static constexpr UINT kStride = kPanelWidth * 3;

        float quality = kStartQuality;
        int   goodRun = 0;

        // Once the floor has failed there is no point spending four encodes a
        // frame discovering that again; one is enough to notice it recovering.
        bool atFloor = false;

        // What one composed frame may spend, from the configured data rate.
        // Atomic because the host sets it when an item starts, off whatever
        // thread called in, while a playback worker is reading it per frame.
        std::atomic<size_t> budget{ kFallbackBudget };

        Stats stats{};

        Impl()
        {
            InitializeCriticalSection(&cs);
            pixels.resize((size_t)kPanelWidth * kPanelHeight * 3);
            stats.quality = kStartQuality;
        }

        ~Impl() { DeleteCriticalSection(&cs); }

        // Decodes straight to 24bpp BGR — the JPEG-native layout, so the format
        // converter is very nearly a no-op, and the destination needs no alpha
        // channel because video frames are opaque by construction.
        bool Decode(const std::vector<uint8_t>& jpeg)
        {
            IWICImagingFactory* factory = detail::WicFactory();
            if (!factory) return false;

            IWICStream* stream = nullptr;
            IWICBitmapDecoder* decoder = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* conv = nullptr;
            bool ok = false;

            if (SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(
                    const_cast<BYTE*>(jpeg.data()), (DWORD)jpeg.size())) &&
                SUCCEEDED(factory->CreateDecoderFromStream(
                    stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) &&
                SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppBGR,
                    WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                UINT w = 0, h = 0;
                if (SUCCEEDED(frame->GetSize(&w, &h)) &&
                    w == (UINT)kPanelWidth && h == (UINT)kPanelHeight)
                {
                    ok = SUCCEEDED(conv->CopyPixels(
                        nullptr, kStride, (UINT)pixels.size(), pixels.data()));
                }
            }

            SafeRelease(conv);
            SafeRelease(frame);
            SafeRelease(decoder);
            SafeRelease(stream);
            return ok;
        }

        // Source-over with a premultiplied source onto an opaque destination:
        //   dst = src + dst * (1 - a)
        void Blend()
        {
            Guard lock(cs);
            if (x1 < 0) return;

            const std::vector<uint8_t>& ov = surface[front];
            if (ov.size() != (size_t)kPanelWidth * kPanelHeight * 4) return;

            for (int y = y0; y <= y1; ++y) {
                const uint8_t* s = &ov[((size_t)y * kPanelWidth + x0) * 4];
                uint8_t* d = &pixels[(size_t)y * kStride + (size_t)x0 * 3];

                for (int x = x0; x <= x1; ++x, s += 4, d += 3) {
                    const unsigned a = s[3];
                    if (!a) continue;
                    if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; continue; }
                    const unsigned inv = 255u - a;
                    d[0] = (uint8_t)(s[0] + (d[0] * inv + 127) / 255);
                    d[1] = (uint8_t)(s[1] + (d[1] * inv + 127) / 255);
                    d[2] = (uint8_t)(s[2] + (d[2] * inv + 127) / 255);
                }
            }
        }

        bool EncodeOnce(float q, std::vector<uint8_t>& out)
        {
            return detail::EncodeJpeg(pixels.data(), q, out);
        }

        // Rate control. The calibrated -q:v governs what ffmpeg hands us; it
        // says nothing about what this encoder produces, so the limit has to be
        // enforced here.
        //
        // The limit that matters is the DATA RATE, not the 80 KB frame cap.
        // Steering on the cap is what this did originally and it was wrong in a
        // way nothing reported: the target worked out to 49.5 KB a frame, which
        // at 30 fps is 1.45 MB/s, and on easy content the recovery rule walked
        // quality up to 0.91 and parked it there. Measured against real frames,
        // a busy stretch went out at 1667 KB/s against a configured 1150 —
        // p90 pinned at the cap — and the panel stuttered while the host
        // reported a clean 30 fps and zero drops. The overlay costs about 2 ms
        // of CPU a frame; it was never the time, it was always the bytes.
        bool Encode(std::vector<uint8_t>& out)
        {
            const size_t target = (std::min)(budget.load(), kMaxJpegBytes);
            const size_t recover = (size_t)(target * kRecoverFraction);
            const int    attempts = atFloor ? 1 : kMaxEncodeAttempts;

            for (int i = 0; i < attempts; ++i) {
                if (!EncodeOnce(quality, out)) return false;

                if (out.size() <= target) {
                    if (i > 0) ++stats.reencodes;
                    atFloor = false;

                    // Creep back up only after a long clean run, so recovering
                    // from one busy scene cannot start an oscillation.
                    if (out.size() < recover) {
                        if (++goodRun >= kRecoverAfter) {
                            goodRun = 0;
                            quality = (std::min)(kMaxQuality, quality + kQualityRecover);
                        }
                    }
                    else {
                        goodRun = 0;
                    }
                    Roll(stats.outBytes, (double)out.size());
                    return true;
                }

                goodRun = 0;
                if (quality <= kMinQuality) break;

                // out.size() > target here, so the ratio is above 1. Twice over
                // takes the whole step; a few percent over takes almost none.
                const double over = (double)out.size() / (double)target - 1.0;
                const float  step = (std::clamp)((float)over * kMaxStep,
                    kMinStep, kMaxStep);
                quality = (std::max)(kMinQuality, quality - step);
            }

            // Over the rate budget with nothing left to give. Stop paying for
            // four encodes a frame to keep rediscovering that; one is enough to
            // notice it recovering.
            atFloor = true;
            ++stats.overBudget;

            // Being over the budget is not a reason to drop the frame — the
            // budget is a rate, and a gap costs more than an expensive frame.
            // Only the panel's hard cap decides what cannot be sent at all.
            Roll(stats.outBytes, (double)out.size());
            if (out.size() <= kMaxJpegBytes) return true;

            ++stats.drops;
            return false;
        }
    };

    // -----------------------------------------------------------------------

    Overlay::Overlay() : p_(new Impl()) {}

    Overlay::~Overlay() { delete p_; }

    bool Overlay::BindThread()
    {
        if (detail::WicBindThread()) return true;

        Log(LogLevel::Warn, L"could not start the image compositor; "
            L"the overlay will not be drawn");
        return false;
    }

    void Overlay::UnbindThread()
    {
        detail::WicUnbindThread();
    }

    void Overlay::SetBudget(size_t bytesPerFrame)
    {
        if (bytesPerFrame == 0) bytesPerFrame = kFallbackBudget;
        p_->budget.store((std::min)(bytesPerFrame, kMaxJpegBytes));
    }

    void Overlay::SetEnabled(bool on)
    {
        if (p_->enabled.exchange(on) == on) return;

        // Toggling changes what a frame should look like just as surely as new
        // pixels do, and a held still only rebuilds when the version moves. Skip
        // this and turning the overlay on leaves the still underneath it bare
        // until something else happens.
        p_->version.fetch_add(1);
    }

    bool Overlay::Enabled() const
    {
        return p_->enabled.load() && p_->hasSurface.load();
    }

    uint32_t Overlay::Version() const { return p_->version.load(); }

    bool Overlay::Update(const uint8_t* bgra, int width, int height)
    {
        if (!bgra || width != kPanelWidth || height != kPanelHeight) return false;

        const size_t bytes = (size_t)kPanelWidth * kPanelHeight * 4;
        const int back = 1 - p_->front;

        std::vector<uint8_t>& dst = p_->surface[back];
        dst.resize(bytes);
        memcpy(dst.data(), bgra, bytes);

        // Bounding box of anything not fully transparent. Computed once here so
        // the per-frame blend does not have to scan for it 30 times a second.
        int x0 = kPanelWidth, y0 = kPanelHeight, x1 = -1, y1 = -1;
        for (int y = 0; y < kPanelHeight; ++y) {
            const uint8_t* row = &dst[(size_t)y * kPanelWidth * 4];
            for (int x = 0; x < kPanelWidth; ++x) {
                if (row[(size_t)x * 4 + 3]) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
        }

        {
            Guard lock(p_->cs);
            p_->front = back;
            p_->x0 = x0; p_->y0 = y0; p_->x1 = x1; p_->y1 = y1;
        }

        p_->hasSurface.store(x1 >= 0);
        p_->version.fetch_add(1);
        return true;
    }

    void Overlay::Clear()
    {
        {
            Guard lock(p_->cs);
            p_->x1 = -1;
            p_->y1 = -1;
        }
        p_->hasSurface.store(false);
        p_->version.fetch_add(1);
    }

    bool Overlay::Compose(std::vector<uint8_t>& jpeg)
    {
        // The path taken whenever nobody is drawing anything. It must stay free.
        if (!Enabled()) return true;

        if (!detail::WicFactory() && !BindThread()) return true;   // degrade to no overlay

        const double t0 = NowMs();
        if (!p_->Decode(jpeg)) {
            // A frame we cannot decode is one we cannot draw on; sending it
            // untouched is better than dropping it.
            return true;
        }
        p_->Blend();
        const double t1 = NowMs();

        std::vector<uint8_t> out;
        const bool ok = p_->Encode(out);
        const double t2 = NowMs();

        Roll(p_->stats.composeMs, t1 - t0);
        Roll(p_->stats.encodeMs, t2 - t1);
        p_->stats.quality = p_->quality;

        if (!ok) return false;

        jpeg.swap(out);
        return true;
    }

    Overlay::Stats Overlay::Snapshot() const { return p_->stats; }

}  // namespace jl
