// jl_jpeg.cpp — the JPEG encoder for everything that does not come straight out
// of ffmpeg.
//
// Two callers. The overlay compositor decodes a frame, draws on it, and has to
// put it back under kMaxJpegBytes. The pack builder takes raw frames and has to
// find, per frame, the best quality that still fits. They share this file
// because they share the constraint that actually matters: the panel decodes
// BASELINE 4:2:0 and nothing else, and renders anything else as a smeared
// picture rather than reporting an error — which is exactly how GIF playback
// stayed broken for so long. JpegYCrCbSubsampling is therefore set explicitly
// on every frame and never left to the encoder's discretion.
//
// WIC does the codec work. It is in-box, it is fast enough (measured ~0.9 ms
// each way at 960x480), and it is the reason the subsampling can be pinned at
// all; GDI+ does not offer the control.

#include "jl_internal.h"

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace jl {
    namespace detail {

        namespace {

            template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

            // Per-thread WIC state. The factory is created on the thread that
            // uses it and released when that thread unbinds, so nothing
            // outlives the COM apartment it was made in.
            //
            // Counted because two subsystems on one thread want it: the native
            // host binds once for the overlay and keeps it for the life of a
            // playback worker, and a pack build on that same worker binds
            // again for the length of the build. Uncounted, the build's unbind
            // would take the overlay's factory with it.
            thread_local IWICImagingFactory* t_factory = nullptr;
            thread_local bool t_ownsCom = false;
            thread_local int  t_binds = 0;

            // WIC's ImageQuality is 0..1 and is not linear in anything useful,
            // so the search walks these rather than bisecting the float range.
            // Coarse at the bottom, where the difference between two steps is
            // obvious, and finer at the top, where the bytes are.
            //
            // The bottom five steps are not decoration. 0.30 was the floor and
            // it is nowhere near as low as it reads: on dense content — foliage,
            // gravel, anything without a flat region — a 960x480 frame at 0.30
            // measures 73 KB, so a frame needing to fit a 39 KB budget could not
            // get there and EncodeJpegToFit shipped it oversized instead. 15% of
            // the frames in a real pack came out over budget that way, and the
            // stream peaked at 1.85x the configured rate. The same frame is
            // 35 KB at 0.10, which the ladder simply could not reach.
            constexpr float kLadder[] = {
                0.05f, 0.10f, 0.15f, 0.20f, 0.25f,
                0.30f, 0.35f, 0.40f, 0.45f, 0.50f, 0.55f, 0.60f, 0.65f,
                0.70f, 0.75f, 0.80f, 0.84f, 0.88f, 0.92f, 0.95f, 0.98f,
            };

            constexpr int kLadderSize = (int)(sizeof(kLadder) / sizeof(kLadder[0]));

        }  // namespace

        bool WicBindThread()
        {
            if (t_factory) { ++t_binds; return true; }

            // A worker that has already joined an apartment keeps it; only the
            // ones that have not get one here, and only those undo it.
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            t_ownsCom = SUCCEEDED(hr) && hr != S_FALSE;
            if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t_factory))))
            {
                if (t_ownsCom) { CoUninitialize(); t_ownsCom = false; }
                t_factory = nullptr;
                return false;
            }

            t_binds = 1;
            return true;
        }

        void WicUnbindThread()
        {
            if (t_binds > 0 && --t_binds > 0) return;

            t_binds = 0;
            SafeRelease(t_factory);
            if (t_ownsCom) { CoUninitialize(); t_ownsCom = false; }
        }

        IWICImagingFactory* WicFactory() { return t_factory; }

        int QualityStepPercent(int step)
        {
            if (step < 0) step = 0;
            if (step >= kLadderSize) step = kLadderSize - 1;
            return (int)(kLadder[step] * 100.0f + 0.5f);
        }

        // The highest step at or below `percent`, so a ceiling is a promise
        // never to exceed it rather than a nearest match.
        int QualityStepForPercent(int percent)
        {
            int best = 0;
            for (int i = 0; i < kLadderSize; ++i) {
                if (QualityStepPercent(i) <= percent) best = i;
            }
            return best;
        }

        bool EncodeJpeg(const uint8_t* bgr, float quality, std::vector<uint8_t>& out)
        {
            if (!bgr || !t_factory) return false;

            const UINT stride = (UINT)kPanelWidth * 3;
            const UINT bytes = stride * (UINT)kPanelHeight;

            IStream* mem = nullptr;
            IWICStream* stream = nullptr;
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* bag = nullptr;
            bool ok = false;

            if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem))) return false;

            if (SUCCEEDED(t_factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromIStream(mem)) &&
                SUCCEEDED(t_factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, &bag)))
            {
                PROPBAG2 opt[2] = {};
                VARIANT  val[2] = {};

                opt[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                val[0].vt = VT_R4;
                val[0].fltVal = quality;

                // Not optional. See the file header.
                opt[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
                val[1].vt = VT_UI1;
                val[1].bVal = WICJpegYCrCbSubsampling420;

                bag->Write(2, opt, val);

                WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                if (SUCCEEDED(frame->Initialize(bag)) &&
                    SUCCEEDED(frame->SetSize(kPanelWidth, kPanelHeight)) &&
                    SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
                    SUCCEEDED(frame->WritePixels(kPanelHeight, stride, bytes,
                        const_cast<BYTE*>(bgr))) &&
                    SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(encoder->Commit()))
                {
                    HGLOBAL hg = nullptr;
                    STATSTG st{};
                    if (SUCCEEDED(GetHGlobalFromStream(mem, &hg)) &&
                        SUCCEEDED(mem->Stat(&st, STATFLAG_NONAME)))
                    {
                        const size_t n = (size_t)st.cbSize.QuadPart;
                        if (void* p = GlobalLock(hg)) {
                            out.resize(n);
                            memcpy(out.data(), p, n);
                            GlobalUnlock(hg);
                            ok = true;
                        }
                    }
                }
            }

            SafeRelease(bag);
            SafeRelease(frame);
            SafeRelease(encoder);
            SafeRelease(stream);
            SafeRelease(mem);
            return ok;
        }

        // The ceiling is tried first and not as part of the bisection, because
        // most frames fit there: that makes the common case one encode and only
        // a frame that actually needs the search pays for it.
        bool EncodeJpegToFit(const uint8_t* bgr, int ceilingStep, size_t target,
            size_t hardCap, std::vector<uint8_t>& out, int& usedStep, int& encodes)
        {
            encodes = 0;
            usedStep = -1;

            if (ceilingStep < 0) ceilingStep = 0;
            if (ceilingStep >= kLadderSize) ceilingStep = kLadderSize - 1;

            ++encodes;
            if (!EncodeJpeg(bgr, kLadder[ceilingStep], out)) return false;
            if (out.size() <= target) {
                usedStep = ceilingStep;
                return true;
            }

            if (ceilingStep == 0) {
                // Nowhere to search: the probe was already the floor.
                usedStep = 0;
                return out.size() <= hardCap;
            }

            std::vector<uint8_t> best, probe;
            int lo = 0, hi = ceilingStep - 1;

            while (lo <= hi) {
                const int mid = lo + (hi - lo) / 2;
                ++encodes;
                if (!EncodeJpeg(bgr, kLadder[mid], probe)) return false;

                if (probe.size() <= target) {
                    usedStep = mid;
                    best.swap(probe);
                    lo = mid + 1;
                }
                else {
                    hi = mid - 1;
                }
            }

            if (usedStep >= 0) {
                out.swap(best);
                return true;
            }

            // Nothing on the ladder fits the target. The floor is the best this
            // frame can do; it is still worth sending if the panel would take
            // it, and calibration is not in the loop to lower it any further.
            ++encodes;
            if (!EncodeJpeg(bgr, kLadder[0], out)) return false;
            usedStep = 0;
            return out.size() <= hardCap;
        }

    }  // namespace detail
}  // namespace jl
