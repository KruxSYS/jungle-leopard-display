// core_tests.cpp - tests for JLDisplayCore's pure functions, with no device.
//
// A console exe with plain checks and no framework, so the solution needs
// nothing it did not already have. Run it; exit code 0 means every check
// passed, and each failure prints its file, line and expression.
//
// What is here is what can be decided without the panel: frame budgets, option
// validation, cache keys, the ffmpeg command lines, the JPEG ladder (WIC is on
// every Windows), and how child processes are started. Anything about what
// the panel does with the bytes is out of reach and is not pretended at.

#include "jl_internal.h"

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

    int g_pass = 0;
    int g_fail = 0;
    const char* g_test = "";

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else {                                                                   \
            ++g_fail;                                                            \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_test, __FILE__,      \
                __LINE__, #cond);                                                \
        }                                                                        \
    } while (0)

    bool Contains(const std::wstring& hay, const std::wstring& needle)
    {
        return hay.find(needle) != std::wstring::npos;
    }

    size_t Find(const std::wstring& hay, const std::wstring& needle)
    {
        return hay.find(needle);
    }

    // ---------------------------------------------------------------------

    void FrameBudget()
    {
        g_test = "frame_budget";
        using jl::FrameBudget;

        // The default rate at 30 fps: what calibration has always sent.
        CHECK(FrameBudget(0, 30) == jl::kDefaultByteRate / 30);
        CHECK(FrameBudget(0, 30) < jl::kSafeJpegBytes);

        // Twice the frame rate, half the bytes a frame: it is a RATE.
        CHECK(FrameBudget(jl::kDefaultByteRate, 60) == jl::kDefaultByteRate / 60);

        // Clamped both ways, and never above one frame's safe size.
        CHECK(FrameBudget(1, 30) == jl::kMinByteRate / 30);
        CHECK(FrameBudget(0xFFFFFFFFu, 30) == jl::kSafeJpegBytes);
        CHECK(FrameBudget(jl::kMaxByteRate, 1) == jl::kSafeJpegBytes);

        // A nonsense frame rate falls back to 30 rather than dividing by zero.
        CHECK(FrameBudget(0, 0) == FrameBudget(0, 30));
        CHECK(FrameBudget(0, -5) == FrameBudget(0, 30));
    }

    void ValidateOpts()
    {
        g_test = "validate_opts";
        std::wstring why;

        jl::RenderOpts o;
        CHECK(jl::ValidateOpts(o, why));

        auto with = [](auto set) { jl::RenderOpts r; set(r); return r; };
        auto ok = [&](const jl::RenderOpts& r) { std::wstring w; return jl::ValidateOpts(r, w); };

        CHECK(!ok(with([](jl::RenderOpts& r) { r.rotate = 45; })));
        CHECK(ok(with([](jl::RenderOpts& r) { r.rotate = 270; })));
        CHECK(!ok(with([](jl::RenderOpts& r) { r.fps = 0; })));
        CHECK(!ok(with([](jl::RenderOpts& r) { r.fps = 61; })));
        CHECK(ok(with([](jl::RenderOpts& r) { r.fps = 60; })));
        CHECK(!ok(with([](jl::RenderOpts& r) { r.quality = 1; })));
        CHECK(ok(with([](jl::RenderOpts& r) { r.quality = 2; })));
        CHECK(!ok(with([](jl::RenderOpts& r) { r.quality = 32; })));
        CHECK(!ok(with([](jl::RenderOpts& r) { r.qualityCeiling = 49; })));
        CHECK(ok(with([](jl::RenderOpts& r) { r.qualityCeiling = 50; })));
        CHECK(!ok(with([](jl::RenderOpts& r) { r.qualityCeiling = 99; })));

        // Audit M6: the hwaccel name goes onto ffmpeg's command line unquoted,
        // so only a name may pass.
        for (const wchar_t* good : { L"", L"auto", L"none", L"cuda", L"qsv", L"d3d11va",
                                     L"dxva2", L"vulkan", L"some_name9" })
            CHECK(ok(with([&](jl::RenderOpts& r) { r.hwaccel = good; })));

        for (const wchar_t* bad : { L"cuda -i x -y C:\\out.mp4", L"CUDA", L"cuda\"",
                                    L"d3d11va,", L"a b", L"x\ty",
                                    L"abcdefghijabcdefghijabcdefghijab" })   // 32 chars
            CHECK(!ok(with([&](jl::RenderOpts& r) { r.hwaccel = bad; })));
    }

    void BuildFilter()
    {
        g_test = "build_filter";
        using jl::detail::BuildFilter;

        CHECK(!Contains(BuildFilter(false, 0), L"transpose"));
        CHECK(Contains(BuildFilter(false, 0), L"pad=960:480"));
        CHECK(Contains(BuildFilter(false, 90), L"transpose=1,"));
        CHECK(Contains(BuildFilter(false, 180), L"hflip,vflip,"));
        CHECK(Contains(BuildFilter(false, 270), L"transpose=2,"));
        CHECK(BuildFilter(false, -90) == BuildFilter(false, 270));
        CHECK(BuildFilter(false, 450) == BuildFilter(false, 90));

        // Stretch fills the panel and never pads.
        CHECK(Contains(BuildFilter(true, 0), L"scale=960:480"));
        CHECK(!Contains(BuildFilter(true, 0), L"pad="));
    }

    void Commands()
    {
        g_test = "ffmpeg_commands";
        using namespace jl::detail;

        const std::wstring ffmpeg = L"C:\\Program Files\\ff mpeg\\ffmpeg.exe";
        const std::wstring input = L"C:\\My Videos\\clip (1).mp4";
        const std::wstring filter = BuildFilter(false, 0);

        const std::wstring v = VideoCommand(ffmpeg, input, filter, L"cuda",
            30, 3, false, false, 0.0, false, 12.5);

        // Both paths quoted, so spaces and brackets survive.
        CHECK(v.rfind(L"\"" + ffmpeg + L"\"", 0) == 0);
        CHECK(Contains(v, L" -i \"" + input + L"\""));
        // Input options before -i, or ffmpeg applies them to the output.
        CHECK(Find(v, L"-hwaccel cuda") < Find(v, L" -i "));
        CHECK(Find(v, L" -ss ") < Find(v, L" -i "));
        // The panel decodes baseline 4:2:0 only.
        CHECK(Contains(v, L"-pix_fmt yuvj420p"));
        CHECK(Contains(v, L"fps=30"));
        CHECK(Contains(v, L"-q:v 3"));

        const std::wstring remote = StreamCommand(ffmpeg, L"https://example.com/v.mp4",
            L"-user_agent \"x\"", filter, L"", 30, 3, 0.0);
        CHECK(Contains(remote, L"-rw_timeout"));
        CHECK(Contains(remote, L"-reconnect 1"));
        CHECK(Contains(remote, L" -an"));
        CHECK(Find(remote, L"-user_agent") < Find(remote, L" -i "));

        const std::wstring local = StreamCommand(ffmpeg, input, L"", filter, L"", 30, 3, 40.0);
        CHECK(!Contains(local, L"-reconnect"));
        CHECK(!Contains(local, L"-rw_timeout"));
        CHECK(Find(local, L" -ss ") < Find(local, L" -i "));

        const std::wstring raw = RawVideoCommand(ffmpeg, input, filter, L"", 30);
        CHECK(Contains(raw, L"-pix_fmt bgr24"));
        CHECK(Contains(raw, L"-f rawvideo -"));
    }

    void CacheKeys()
    {
        g_test = "cache_keys";
        using jl::detail::CalibrationKey;
        using jl::detail::Fnv1aHex;

        // The hash: stable, 16 hex digits, and FNV-1a 64 of "" is its offset basis.
        CHECK(Fnv1aHex("") == "cbf29ce484222325");
        CHECK(Fnv1aHex("abc") == Fnv1aHex("abc"));
        CHECK(Fnv1aHex("abc") != Fnv1aHex("abd"));
        CHECK(Fnv1aHex("x").size() == 16);

        // A real file, so size and timestamp are part of the key.
        wchar_t dir[MAX_PATH] = L"";
        GetTempPathW(MAX_PATH, dir);
        const std::wstring path = std::wstring(dir) + L"jl_core_tests_key.bin";
        {
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD n = 0;
            WriteFile(h, "0123456789", 10, &n, nullptr);
            CloseHandle(h);
        }

        const std::wstring f0 = jl::detail::BuildFilter(false, 0);
        const std::wstring f90 = jl::detail::BuildFilter(false, 90);

        const std::string k = CalibrationKey(path, f0, jl::kSafeJpegBytes);
        CHECK(k == CalibrationKey(path, f0, jl::kSafeJpegBytes));       // deterministic
        CHECK(k != CalibrationKey(path, f90, jl::kSafeJpegBytes));      // filter matters
        CHECK(k != CalibrationKey(path, f0, jl::kSafeJpegBytes / 2));   // target matters

        // Replacing the file's contents invalidates the key, which is how a
        // re-encoded video gets surveyed again without anyone clearing a cache.
        {
            HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD n = 0;
            WriteFile(h, "more", 4, &n, nullptr);
            CloseHandle(h);
        }
        CHECK(k != CalibrationKey(path, f0, jl::kSafeJpegBytes));

        DeleteFileW(path.c_str());
    }

    // Audit H1: a child started by StartFfmpegPipe must not walk off with
    // handles it was not given. The test makes an inheritable pipe of its own,
    // starts a long-lived child the way playback does, then closes its write
    // end: if the child inherited a copy, the read end cannot see EOF until the
    // child exits, seconds later.
    struct ReadArgs { HANDLE read; };

    DWORD WINAPI ReadUntilEof(LPVOID p)
    {
        auto* a = static_cast<ReadArgs*>(p);
        char buf[64];
        DWORD got = 0;
        while (ReadFile(a->read, buf, sizeof(buf), &got, nullptr) && got > 0) {}
        return 0;
    }

    void PipeNotInherited()
    {
        g_test = "pipe_not_inherited";

        // Sanity first: the child's own stdout still arrives.
        {
            jl::detail::FfmpegPipe p;
            CHECK(jl::detail::StartFfmpegPipe(L"cmd.exe /d /c echo hello", p));
            const std::string text = jl::detail::ReadAllText(p);
            p.Close();
            CHECK(text.find("hello") != std::string::npos);
        }

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE myRead = nullptr, myWrite = nullptr;
        CHECK(CreatePipe(&myRead, &myWrite, &sa, 0));
        SetHandleInformation(myRead, HANDLE_FLAG_INHERIT, 0);   // only the write end leaks

        // About three seconds of a child that writes nothing.
        jl::detail::FfmpegPipe child;
        CHECK(jl::detail::StartFfmpegPipe(L"cmd.exe /d /c ping -n 4 127.0.0.1 >nul", child));

        CloseHandle(myWrite);

        ReadArgs args{ myRead };
        HANDLE reader = CreateThread(nullptr, 0, ReadUntilEof, &args, 0, nullptr);
        const DWORD wait = WaitForSingleObject(reader, 1000);
        CHECK(wait == WAIT_OBJECT_0);   // EOF at once: nobody else holds the write end

        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(child.process, 0);
            WaitForSingleObject(reader, 5000);
        }
        CloseHandle(reader);
        CloseHandle(myRead);
        TerminateProcess(child.process, 0);
        child.Close();
    }

    // The per-frame ladder: fits the budget when it can, never exceeds the
    // panel's cap, always 960x480 baseline 4:2:0, and is a pure function of
    // the pixels - the pack builder's dedupe depends on that last one.
    void EncodeToFit()
    {
        g_test = "encode_jpeg_to_fit";
        using namespace jl::detail;

        CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        CHECK(WicBindThread());

        const size_t pixels = (size_t)jl::kPanelWidth * jl::kPanelHeight;
        std::vector<uint8_t> dense(pixels * 3), flat(pixels * 3, 40);

        // Busy, foliage-like content: coarse value noise plus fine grain, from
        // a fixed seed so the test sees the same frame every run.
        uint32_t seed = 12345;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 24; };
        for (int y = 0; y < jl::kPanelHeight; ++y)
            for (int x = 0; x < jl::kPanelWidth; ++x) {
                const uint32_t base = (uint32_t)(((x / 6) * 37 + (y / 6) * 91) * 2654435761u >> 24);
                for (int c = 0; c < 3; ++c)
                    dense[((size_t)y * jl::kPanelWidth + x) * 3 + c] =
                        (uint8_t)((base + rnd() / 2 + c * 40) & 0xFF);
            }

        const size_t target = jl::FrameBudget(0, 30);
        const int ceiling = QualityStepForPercent(jl::kDefaultQualityCeiling);

        std::vector<uint8_t> a, b, c;
        int stepA = 0, stepB = 0, stepC = 0, encodes = 0;

        CHECK(EncodeJpegToFit(dense.data(), ceiling, target, jl::kMaxJpegBytes, a, stepA, encodes));
        CHECK(a.size() <= jl::kMaxJpegBytes);
        CHECK(a.size() <= target || stepA == 0);   // over budget only at the floor
        CHECK(JpegIsBaseline420(a));
        int w = 0, h = 0;
        CHECK(JpegSize(a, w, h) && w == jl::kPanelWidth && h == jl::kPanelHeight);

        // Deterministic, with or without a hint.
        CHECK(EncodeJpegToFit(dense.data(), ceiling, target, jl::kMaxJpegBytes, b, stepB, encodes));
        CHECK(a == b && stepA == stepB);
        CHECK(EncodeJpegToFit(dense.data(), ceiling, target, jl::kMaxJpegBytes, c, stepC, encodes,
            ceiling));
        CHECK(a == c && stepA == stepC);

        // A flat frame fits at the ceiling: the budget does not drag down a
        // frame that never needed it.
        std::vector<uint8_t> f;
        int stepF = 0;
        CHECK(EncodeJpegToFit(flat.data(), ceiling, target, jl::kMaxJpegBytes, f, stepF, encodes));
        CHECK(stepF == ceiling);
        CHECK(f.size() <= target);

        WicUnbindThread();
        CoUninitialize();
    }

}  // namespace

int wmain()
{
    // The core logs through a per-thread sink; nothing installed means quiet.
    FrameBudget();
    ValidateOpts();
    BuildFilter();
    Commands();
    CacheKeys();
    PipeNotInherited();
    EncodeToFit();

    std::printf("core tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
