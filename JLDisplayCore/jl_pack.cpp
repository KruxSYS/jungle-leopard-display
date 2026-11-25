// jl_pack.cpp — turning a source into panel-ready frames once, so ffmpeg does
// not have to run for as long as something is playing.
//
// The panel takes a stream of 960x480 baseline 4:2:0 JPEGs under 80 KB, and
// what ffmpeg produces for a given source is a pure function of that source's
// bytes and the render options. The calibration cache already relies on that;
// this file takes it the rest of the way and stores the frames themselves.
//
// Once the frames are being stored rather than streamed, the one thing
// calibration exists to work around stops applying. Calibration picks a single
// -q:v for a whole file from its BUSIEST frame, because a stream has to commit
// to a figure before it knows what is coming; a pack does not. So the builder
// can encode each frame at the best quality that frame can afford and store
// exactly that, which is what perFrameQuality asks for. Two consequences worth
// knowing before touching either path:
//
//   - the search must stay a pure function of the frame's pixels, or two
//     identical frames encode differently and the dedupe below stops seeing
//     them as the same frame;
//   - packs get bigger, because the whole point is that frames which were
//     being squeezed for the sake of the worst one no longer are.
//
// A pack is deliberately dull:
//
//     header | frame blobs | index
//
// Identical frames share one blob and the index points at it more than once,
// which collapses GIFs and static footage to a fraction of their nominal size.
// The file is mapped rather than read, so a long video costs address space
// instead of working set.

#include "jl_internal.h"

#include <cstdio>
#include <cstdlib>
#include <share.h>      // _SH_DENYWR, for the pack file's sharing mode
#include <algorithm>
#include <atomic>

namespace jl {

    namespace {

        // 2 marks per-frame quality: the header's `quality` became a sentinel
        // rather than the figure every frame was encoded at.
        // 3: the quality ladder gained five steps below 0.30, so a frame that
        // used to be shipped over budget because nothing fit now encodes lower
        // and fits. The key does not change — the ceiling and the budget are
        // the same numbers — but the frames behind it are different, so the
        // version is what retires a pack built before the floor moved.
        constexpr uint32_t kPackVersion = 3;

        // Set from the host, read by whichever worker is building. Atomic rather
        // than locked: a change lands on the next pack, and a build already
        // running keeps the figure it started with.
        std::atomic<uint64_t> g_memoryBudget{ kDefaultMemoryBudget };
        std::atomic<uint64_t> g_diskBudget{ kDefaultDiskBudget };

        // How many recent unique frames stay in RAM for duplicate detection.
        // Bounded on purpose: a disk pack may be far larger than memory allows,
        // and the duplicates worth catching — a static shot, a short cycling
        // animation — are always near each other. 256 x 80 KB is the ceiling.
        constexpr size_t kDedupeWindow = 256;

#pragma pack(push, 1)
        struct PackHeader {
            char     magic[4];        // "JLP1"
            uint32_t version;
            uint32_t fps;
            uint32_t width;
            uint32_t height;
            uint32_t quality;         // the -q:v every frame was encoded at, or
                                      // 0 when each frame chose its own
            uint32_t frameCount;      // entries in the index
            uint32_t uniqueCount;     // distinct blobs actually stored
            uint32_t reserved;        // per-frame packs: the quality ceiling
            uint64_t sourceSize;
            uint64_t sourceMtime;
            uint64_t indexOffset;
            char     key[16];         // the same digest the filename carries
        };

        struct PackIndexEntry {
            uint64_t offset;
            uint32_t length;
        };
#pragma pack(pop)

        static_assert(sizeof(PackHeader) == 76, "pack header layout changed");
        static_assert(sizeof(PackIndexEntry) == 12, "pack index layout changed");

        // -------------------------------------------------------------------
        // Where packs live
        // -------------------------------------------------------------------

        std::wstring PackDirectory()
        {
            std::wstring dir = detail::CacheDirectory() + L"\\packs";
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir;
        }

        std::wstring PackPath(const std::string& key)
        {
            std::wstring wide(key.begin(), key.end());
            return PackDirectory() + L"\\" + wide + L".jlp";
        }

        std::wstring StillPath(const std::string& key)
        {
            std::wstring wide(key.begin(), key.end());
            return PackDirectory() + L"\\" + wide + L".jlf";
        }

        // Everything that changes the bytes we would send: which file, what it
        // contained, how it is framed, how fast, and at what quality. The same
        // material as the calibration key, plus the things calibration does not
        // care about — the frame rate, and either the -q:v it settled on or the
        // ceiling a per-frame build was given.
        std::string PackKey(const std::wstring& input, const std::wstring& filter,
            int fps, int quality, int ceiling, size_t budget)
        {
            std::wstring full = input;
            wchar_t abs[MAX_PATH] = L"";
            if (GetFullPathNameW(input.c_str(), MAX_PATH, abs, nullptr)) full = abs;

            unsigned long long bytes = 0, mtime = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) {
                bytes = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                mtime = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
                    | fad.ftLastWriteTime.dwLowDateTime;
            }

            std::string material;
            for (wchar_t c : full)   material += (char)(c & 0xFF);
            material += "|";
            for (wchar_t c : filter) material += (char)(c & 0xFF);

            char extra[160];
            sprintf_s(extra, sizeof(extra), "|%llu|%llu|%d|%d|%d|%zu|%d|%d|pack%u",
                bytes, mtime, fps, quality, ceiling, budget,
                kPanelWidth, kPanelHeight, kPackVersion);
            material += extra;

            return detail::Fnv1aHex(material);
        }

        // A still is the same question with no frame rate: one JPEG, one filter.
        std::string StillKey(const std::wstring& input, const std::wstring& filter)
        {
            std::wstring full = input;
            wchar_t abs[MAX_PATH] = L"";
            if (GetFullPathNameW(input.c_str(), MAX_PATH, abs, nullptr)) full = abs;

            unsigned long long bytes = 0, mtime = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) {
                bytes = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                mtime = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32)
                    | fad.ftLastWriteTime.dwLowDateTime;
            }

            std::string material;
            for (wchar_t c : full)   material += (char)(c & 0xFF);
            material += "|";
            for (wchar_t c : filter) material += (char)(c & 0xFF);

            char extra[96];
            sprintf_s(extra, sizeof(extra), "|%llu|%llu|%d|%d|still1",
                bytes, mtime, kPanelWidth, kPanelHeight);
            material += extra;

            return detail::Fnv1aHex(material);
        }

        // NTFS stops updating last-access times by default, so a pack's
        // last-WRITE time is stamped forward every time it is used. That makes
        // eviction a real least-recently-used rather than oldest-built-first.
        void Touch(const std::wstring& path)
        {
            HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            SetFileTime(h, nullptr, nullptr, &now);
            CloseHandle(h);
        }

        struct CacheEntry {
            std::wstring name;
            uint64_t     bytes = 0;
            uint64_t     stamp = 0;
        };

        std::vector<CacheEntry> ScanCache()
        {
            std::vector<CacheEntry> out;
            const std::wstring dir = PackDirectory();

            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE) return out;

            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

                std::wstring name = fd.cFileName;
                size_t dot = name.find_last_of(L'.');
                if (dot == std::wstring::npos) continue;
                std::wstring ext = name.substr(dot);
                if (ext != L".jlp" && ext != L".jlf") continue;

                CacheEntry e;
                e.name = name;
                e.bytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                e.stamp = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32)
                    | fd.ftLastWriteTime.dwLowDateTime;
                out.push_back(e);
            } while (FindNextFileW(find, &fd));

            FindClose(find);
            return out;
        }

        // Deletes least-recently-used packs until the cache fits, never touching
        // `keep` — which is the one just written and about to be mapped.
        // Half-written packs from a build that was killed or crashed.
        //
        // ScanCache counts only .jlp and .jlf, so these are invisible to the
        // budget and nothing was ever going to remove them: 15 of them, 1.5 GB,
        // had collected in testing. Swept by age so a build running right now
        // — in this process or another — keeps its own.
        void SweepStaleTemps()
        {
            const std::wstring dir = PackDirectory();

            FILETIME nowFt{};
            GetSystemTimeAsFileTime(&nowFt);
            const uint64_t now =
                ((uint64_t)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;

            // 100-nanosecond units, so an hour is this many.
            const uint64_t hour = 36000000000ull;

            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((dir + L"\\*.tmp").c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE) return;

            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

                const uint64_t stamp =
                    ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32)
                    | fd.ftLastWriteTime.dwLowDateTime;

                if (now > stamp && now - stamp > hour)
                    DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(find, &fd));

            FindClose(find);
        }

        void EnforceDiskBudget(const std::wstring& keep)
        {
            SweepStaleTemps();

            std::vector<CacheEntry> entries = ScanCache();

            const uint64_t budget = DiskBudget();

            uint64_t total = 0;
            for (const CacheEntry& e : entries) total += e.bytes;
            if (total <= budget) return;

            std::sort(entries.begin(), entries.end(),
                [](const CacheEntry& a, const CacheEntry& b) { return a.stamp < b.stamp; });

            const std::wstring dir = PackDirectory();
            for (const CacheEntry& e : entries) {
                if (total <= budget) break;
                if (keep.find(e.name) != std::wstring::npos) continue;
                if (DeleteFileW((dir + L"\\" + e.name).c_str())) {
                    total -= e.bytes;
                    Log(LogLevel::Info, L"pack cache: evicted %s", e.name.c_str());
                }
                else {
                    Log(LogLevel::Info, L"pack cache: could not evict %s (error %lu)",
                        e.name.c_str(), GetLastError());
                }
            }
        }

        // -------------------------------------------------------------------
        // Building
        // -------------------------------------------------------------------

        // Where a build puts its bytes: straight into a vector, or straight into
        // the file it is about to become.
        struct Sink {
            std::vector<uint8_t>* memory = nullptr;
            FILE* file = nullptr;
            uint64_t offset = 0;

            bool Write(const uint8_t* data, size_t len)
            {
                if (memory) {
                    memory->insert(memory->end(), data, data + len);
                }
                else if (file) {
                    if (fwrite(data, 1, len, file) != len) return false;
                }
                offset += len;
                return true;
            }
        };

        // A bounded ring of recently written unique frames, so a repeat can be
        // pointed at the copy already stored rather than stored again.
        class DedupeWindow {
        public:
            const FramePack::Ref* Find(uint64_t hash, const std::vector<uint8_t>& frame) const
            {
                for (const Entry& e : ring_) {
                    if (e.hash != hash || e.bytes.size() != frame.size()) continue;
                    if (memcmp(e.bytes.data(), frame.data(), frame.size()) == 0) return &e.ref;
                }
                return nullptr;
            }

            void Add(uint64_t hash, const FramePack::Ref& ref, const std::vector<uint8_t>& frame)
            {
                if (ring_.size() < kDedupeWindow) {
                    ring_.push_back({ hash, ref, frame });
                    return;
                }
                ring_[next_] = { hash, ref, frame };
                next_ = (next_ + 1) % kDedupeWindow;
            }

        private:
            struct Entry {
                uint64_t             hash;
                FramePack::Ref       ref;
                std::vector<uint8_t> bytes;
            };
            std::vector<Entry> ring_;
            size_t             next_ = 0;
        };

        uint64_t HashFrame(const std::vector<uint8_t>& f)
        {
            uint64_t h = 1469598103934665603ULL;
            for (uint8_t c : f) { h ^= c; h *= 1099511628211ULL; }
            return h;
        }

        struct BuildResult {
            std::vector<FramePack::Ref> index;
            size_t unique = 0;
            size_t dropped = 0;
            bool   overBudget = false;
            bool   cancelled = false;

            // Per-frame builds only. Worth reporting: a run whose lowest
            // quality is the ceiling means the ceiling, not the size limit, is
            // what every frame ran into, and raising it would change something.
            size_t encodes = 0;
            int    qualityMin = 100;
            int    qualityMax = 0;
            double qualitySum = 0.0;
            size_t qualityCount = 0;

            int MeanQuality() const
            {
                return qualityCount ? (int)(qualitySum / qualityCount + 0.5) : 0;
            }
        };

        // Snaps a requested ceiling to what the ladder can actually deliver, so
        // the key and the header record the ceiling that was used rather than
        // the one asked for — two requests landing on the same step then share
        // one pack instead of building it twice.
        int ClampCeiling(int percent)
        {
            if (percent <= 0) percent = kDefaultQualityCeiling;
            if (percent < kMinQualityCeiling) percent = kMinQualityCeiling;
            if (percent > kMaxQualityCeiling) percent = kMaxQualityCeiling;
            return detail::QualityStepPercent(detail::QualityStepForPercent(percent));
        }

        // The tail both builders share: recognise a repeat, respect the budget,
        // write, index. `stop` comes back true when the budget ended the build;
        // false is a failed write, which is fatal to the pack.
        bool StoreFrame(const std::vector<uint8_t>& frame, Sink& sink, size_t budget,
            DedupeWindow& window, BuildResult& result, bool& stop)
        {
            const uint64_t hash = HashFrame(frame);
            if (const FramePack::Ref* seen = window.Find(hash, frame)) {
                result.index.push_back(*seen);
                return true;
            }

            if (budget && sink.offset + frame.size() > budget) {
                result.overBudget = true;
                stop = true;
                return true;
            }

            FramePack::Ref ref{ sink.offset, (uint32_t)frame.size() };
            if (!sink.Write(frame.data(), frame.size())) return false;

            window.Add(hash, ref, frame);
            result.index.push_back(ref);
            ++result.unique;
            return true;
        }

        // Runs ffmpeg once, flat out, and feeds every frame into `sink`. This is
        // the calibration survey's loop with the encode kept instead of thrown
        // away — no -re, no wall-clock pacing, no device.
        bool RunBuild(const std::wstring& ffmpeg, const std::wstring& input,
            const std::wstring& filter, const std::wstring& hwaccel,
            int fps, int quality, Sink& sink, size_t budget,
            AbortFn abort, void* abortUser, BuildResult& result,
            PackProgressFn onProgress, void* progressUser)
        {
            detail::FfmpegPipe pipe;
            std::wstring cl = detail::VideoCommand(ffmpeg, input, filter, hwaccel,
                fps, quality, false, false, 0);
            if (!detail::StartFfmpegPipe(cl, pipe)) return false;

            DedupeWindow window;
            std::vector<uint8_t> acc, frame;
            const DWORD began = GetTickCount();
            DWORD lastPrint = 0;
            bool  formatChecked = false;
            bool  wroteFailed = false;

            while (detail::ReadNextJpeg(pipe, acc, frame)) {
                if (abort && abort(abortUser)) { result.cancelled = true; break; }

                // Calibration should have made this impossible, but a frame the
                // panel would refuse must not go into a pack that outlives this
                // run and replays it forever.
                if (frame.size() > kMaxJpegBytes) { ++result.dropped; continue; }

                if (!formatChecked) {
                    formatChecked = true;
                    if (!detail::JpegIsBaseline420(frame))
                        Log(LogLevel::Warn, L"frames are not baseline 4:2:0 - "
                            L"the panel will draw them wrong");
                }

                bool stop = false;
                if (!StoreFrame(frame, sink, budget, window, result, stop)) {
                    wroteFailed = true;
                    break;
                }
                if (stop) break;

                DWORD now = GetTickCount();
                if (now - lastPrint >= 250) {
                    lastPrint = now;
                    if (onProgress)
                        onProgress(result.index.size(), sink.offset, progressUser);

                    Log(LogLevel::Progress, L"  preprocessing: %zu frames, %llu MB, %.1fs",
                        result.index.size(),
                        (unsigned long long)(sink.offset / (1024 * 1024)),
                        (now - began) / 1000.0);
                }
            }

            // ffmpeg's exit code decides whether this pack is the whole video,
            // and nothing else can. Reading to end of stream is not proof of
            // completeness: a decoder that dies halfway closes the pipe in
            // exactly the way one that finished does, and the pack left behind
            // then plays the first part of the song and "ends" — which from the
            // sofa is indistinguishable from a video that is simply too short,
            // and which the disk cache would replay forever.
            //
            // It went unnoticed while builds ran with nothing else on the
            // panel. Building behind a video that is already playing gives the
            // two ffmpegs something to fall out over, and then it matters.
            DWORD exitCode = 0;
            if (pipe.process) {
                WaitForSingleObject(pipe.process, 5000);
                GetExitCodeProcess(pipe.process, &exitCode);
            }

            pipe.Close();

            if (!result.cancelled && exitCode != 0 && exitCode != STILL_ACTIVE) {
                Log(LogLevel::Warn,
                    L"ffmpeg exited with code %lu partway through - the pack would be "
                    L"short, so it is not being kept", exitCode);
                return false;
            }

            return !wroteFailed;
        }

        // Frames come out of ffmpeg raw and every encode is ours, which is the
        // whole point: a quality chosen per frame is not something an ffmpeg
        // command line can express. One decode pass, same as above — and no
        // calibration pass before it, since there is no single figure to find.
        bool RunBuildPerFrame(const std::wstring& ffmpeg, const std::wstring& input,
            const std::wstring& filter, const std::wstring& hwaccel,
            int fps, int ceiling, size_t frameBudget, Sink& sink, size_t budget,
            AbortFn abort, void* abortUser, BuildResult& result,
            PackProgressFn onProgress, void* progressUser)
        {
            // Counted, so binding here inside a host thread that already bound
            // for its overlay does not take the factory away from it on exit.
            if (!detail::WicBindThread()) {
                Log(LogLevel::Error, L"could not start the JPEG encoder");
                return false;
            }
            struct Unbind { ~Unbind() { detail::WicUnbindThread(); } } unbind;

            const int ceilingStep = detail::QualityStepForPercent(ceiling);

            detail::FfmpegPipe pipe;
            const std::wstring cl =
                detail::RawVideoCommand(ffmpeg, input, filter, hwaccel, fps);
            if (!detail::StartFfmpegPipe(cl, pipe)) return false;

            const size_t rawBytes = (size_t)kPanelWidth * kPanelHeight * 3;

            DedupeWindow window;
            std::vector<uint8_t> raw(rawBytes), frame;
            const DWORD began = GetTickCount();
            DWORD lastPrint = 0;
            bool  formatChecked = false;
            bool  wroteFailed = false;
            bool  stop = false;

            // The previous frame's quality, which is where the next frame's
            // search starts. See EncodeJpegToFit: it is worth about half the
            // encodes on a run, and changes nothing about the answer.
            int hint = -1;

            while (!stop && detail::ReadRawFrame(pipe, raw.data(), rawBytes)) {
                if (abort && abort(abortUser)) { result.cancelled = true; break; }

                int step = 0, encodes = 0;
                const bool fits = detail::EncodeJpegToFit(raw.data(), ceilingStep,
                    frameBudget, kMaxJpegBytes, frame, step, encodes, hint);
                result.encodes += encodes;
                if (fits && step >= 0) hint = step;

                // Either the encoder failed or this frame will not fit at any
                // quality the ladder offers. A frame the panel would refuse must
                // not go into a pack that outlives this run and replays it
                // forever, and one bad frame is no reason to abandon the rest.
                if (!fits) { ++result.dropped; continue; }

                const int percent = detail::QualityStepPercent(step);
                if (percent < result.qualityMin) result.qualityMin = percent;
                if (percent > result.qualityMax) result.qualityMax = percent;
                result.qualitySum += percent;
                ++result.qualityCount;

                // The encoder pins 4:2:0, so this cannot fail — but it could
                // only ever fail silently, and the panel's failure mode for the
                // wrong flavour is a smeared picture rather than an error.
                if (!formatChecked) {
                    formatChecked = true;
                    if (!detail::JpegIsBaseline420(frame))
                        Log(LogLevel::Warn, L"frames are not baseline 4:2:0 - "
                            L"the panel will draw them wrong");
                }

                if (!StoreFrame(frame, sink, budget, window, result, stop)) {
                    wroteFailed = true;
                    break;
                }

                DWORD now = GetTickCount();
                if (now - lastPrint >= 250) {
                    lastPrint = now;

                    if (onProgress)
                        onProgress(result.index.size(), sink.offset, progressUser);

                    Log(LogLevel::Progress,
                        L"  preprocessing: %zu frames, %llu MB, quality %d%%, %.1fs",
                        result.index.size(),
                        (unsigned long long)(sink.offset / (1024 * 1024)),
                        result.MeanQuality(), (now - began) / 1000.0);
                }
            }

            // ffmpeg's exit code decides whether this pack is the whole video,
            // and nothing else can. Reading to end of stream is not proof of
            // completeness: a decoder that dies halfway closes the pipe in
            // exactly the way one that finished does, and the pack left behind
            // then plays the first part of the song and "ends" — which from the
            // sofa is indistinguishable from a video that is simply too short,
            // and which the disk cache would replay forever.
            //
            // It went unnoticed while builds ran with nothing else on the
            // panel. Building behind a video that is already playing gives the
            // two ffmpegs something to fall out over, and then it matters.
            DWORD exitCode = 0;
            if (pipe.process) {
                WaitForSingleObject(pipe.process, 5000);
                GetExitCodeProcess(pipe.process, &exitCode);
            }

            pipe.Close();

            if (!result.cancelled && exitCode != 0 && exitCode != STILL_ACTIVE) {
                Log(LogLevel::Warn,
                    L"ffmpeg exited with code %lu partway through - the pack would be "
                    L"short, so it is not being kept", exitCode);
                return false;
            }

            return !wroteFailed;
        }

        // -------------------------------------------------------------------
        // The prepared-still cache
        //
        // Small, in-process, and shared by both preprocess modes. Disk mode puts
        // a copy on disk as well; this sits in front of it so a playlist coming
        // back round does not even re-read the file.
        // -------------------------------------------------------------------

        constexpr size_t kStillCacheEntries = 32;

        CRITICAL_SECTION& StillLock()
        {
            static CRITICAL_SECTION cs = [] {
                CRITICAL_SECTION c;
                InitializeCriticalSection(&c);
                return c;
            }();
            return cs;
        }

        struct StillEntry {
            std::string          key;
            std::vector<uint8_t> jpeg;
        };

        std::vector<StillEntry>& Stills()
        {
            static std::vector<StillEntry> v;
            return v;
        }

        bool StillCacheGet(const std::string& key, std::vector<uint8_t>& out)
        {
            EnterCriticalSection(&StillLock());
            bool found = false;
            for (const StillEntry& e : Stills()) {
                if (e.key == key) { out = e.jpeg; found = true; break; }
            }
            LeaveCriticalSection(&StillLock());
            return found;
        }

        void StillCachePut(const std::string& key, const std::vector<uint8_t>& jpeg)
        {
            EnterCriticalSection(&StillLock());
            std::vector<StillEntry>& v = Stills();
            bool present = false;
            for (const StillEntry& e : v) {
                if (e.key == key) { present = true; break; }
            }
            if (!present) {
                if (v.size() >= kStillCacheEntries) v.erase(v.begin());
                v.push_back({ key, jpeg });
            }
            LeaveCriticalSection(&StillLock());
        }

    }  // namespace

    // -----------------------------------------------------------------------
    // FramePack
    // -----------------------------------------------------------------------

    FramePack::~FramePack()
    {
        Close();
    }

    void FramePack::Close()
    {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        owned_.clear();
        owned_.shrink_to_fit();
        index_.clear();
        base_ = nullptr;
        bytes_ = 0;
    }

    const uint8_t* FramePack::Frame(size_t i, size_t& len) const
    {
        if (!base_ || i >= index_.size()) { len = 0; return nullptr; }
        len = index_[i].length;
        return base_ + index_[i].offset;
    }

    void FramePack::AdoptMemory(std::vector<uint8_t>&& blob, std::vector<Ref>&& index, int fps)
    {
        Close();
        owned_ = std::move(blob);
        index_ = std::move(index);
        fps_ = fps > 0 ? fps : 30;
        bytes_ = owned_.size();
        base_ = owned_.empty() ? nullptr : owned_.data();
    }

    bool FramePack::AdoptFile(const std::wstring& path)
    {
        Close();

        // FILE_SHARE_DELETE so clearing the cache while this is playing does not
        // fail; the mapping keeps the bytes alive until playback lets go.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            // Not being there is the ordinary answer and says nothing. Being
            // there and refusing to open is worth hearing about: it means
            // something in this process still has it, and the caller that just
            // wrote it is about to be told only that it "would not load".
            const DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
                Log(LogLevel::Warn, L"the pack file would not open (error %lu)", err);
            return false;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart < (LONGLONG)sizeof(PackHeader)) {
            CloseHandle(h);
            return false;
        }

        HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m) { CloseHandle(h); return false; }

        const uint8_t* view = (const uint8_t*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        if (!view) { CloseHandle(m); CloseHandle(h); return false; }

        int failedAt = 0;
        auto fail = [&] {
            UnmapViewOfFile(view);
            CloseHandle(m);
            CloseHandle(h);

            // Numbered rather than described: this runs on every speculative
            // adopt, and the number is enough to find the line.
            if (failedAt)
                Log(LogLevel::Warn, L"the pack file failed check %d", failedAt);
            return false;
        };

        PackHeader head{};
        memcpy(&head, view, sizeof(head));

        // 1 is the unstamped magic of a build that never finished, and is the
        // one expected failure here; it is left unnumbered so it stays quiet.
        if (memcmp(head.magic, "JLP1", 4) != 0)    return fail();

        failedAt = 2; if (head.version != kPackVersion)  return fail();
        failedAt = 3; if (head.width != kPanelWidth)     return fail();
        failedAt = 4; if (head.height != kPanelHeight)   return fail();
        failedAt = 5; if (head.frameCount == 0)          return fail();
        failedAt = 0;

        // The digest is in the filename too, and the filename is how this was
        // found. Checking both means a half-written or renamed file cannot be
        // mistaken for the pack that was actually asked for.
        std::string stem;
        size_t slash = path.find_last_of(L"\\/");
        size_t dot = path.find_last_of(L'.');
        if (slash != std::wstring::npos && dot != std::wstring::npos && dot > slash) {
            for (size_t i = slash + 1; i < dot; ++i) stem += (char)(path[i] & 0xFF);
        }
        failedAt = 6;
        if (stem.size() != 16 || memcmp(head.key, stem.data(), 16) != 0) return fail();

        const uint64_t indexBytes = (uint64_t)head.frameCount * sizeof(PackIndexEntry);
        failedAt = 7; if (head.indexOffset < sizeof(PackHeader)) return fail();
        failedAt = 8;
        if (head.indexOffset + indexBytes > (uint64_t)size.QuadPart) return fail();
        failedAt = 9;

        std::vector<Ref> index;
        index.reserve(head.frameCount);
        for (uint32_t i = 0; i < head.frameCount; ++i) {
            PackIndexEntry e{};
            memcpy(&e, view + head.indexOffset + (uint64_t)i * sizeof(e), sizeof(e));
            if (e.length == 0 || e.length > kMaxJpegBytes)  return fail();
            if (e.offset < sizeof(PackHeader))              return fail();
            if (e.offset + e.length > head.indexOffset)     return fail();
            index.push_back(Ref{ e.offset, e.length });
        }

        file_ = h;
        mapping_ = m;
        view_ = view;
        base_ = view;
        index_ = std::move(index);
        fps_ = (int)head.fps;
        bytes_ = (size_t)size.QuadPart;
        return true;
    }

    // -----------------------------------------------------------------------
    // GetPack
    // -----------------------------------------------------------------------

    bool GetPack(const std::wstring& path, const RenderOpts& opts, Preprocess mode,
        FramePack& pack, AbortFn abort, void* abortUser, PackStats* stats,
        bool adoptOnly, PackProgressFn onProgress, void* progressUser)
    {
        auto aborted = [&] { return abort && abort(abortUser); };

        if (mode == Preprocess::Off) return false;

        const std::wstring ffmpeg = FindFfmpeg();
        if (ffmpeg.empty()) {
            Log(LogLevel::Error, L"ffmpeg.exe not found; preprocessing needs it");
            return false;
        }

        RenderOpts resolved = opts;
        resolved.hwaccel = detail::ResolveHwaccel(ffmpeg, opts.hwaccel);

        const std::wstring filter = detail::BuildFilter(resolved.stretch, resolved.rotate);

        // The quality is part of what the frames ARE, so it has to be settled
        // before the key can name them — which for a per-frame build means the
        // ceiling and nothing else. That skips calibration entirely: a survey
        // is a second pass over the file to find a figure this path does not
        // use. An explicit -q:v turns per-frame off, because a caller naming a
        // figure is asking for that figure everywhere.
        const bool   perFrame = resolved.perFrameQuality && resolved.quality <= 0;
        const int    ceiling = perFrame ? ClampCeiling(resolved.qualityCeiling) : 0;
        const size_t frameBudget = perFrame
            ? FrameBudget(resolved.byteRate, resolved.fps) : 0;

        int quality = 0;
        if (!perFrame) {
            quality = ResolveQuality(path, resolved, abort, abortUser);
            if (quality < 0) return false;
        }
        if (aborted()) return false;

        const std::string  key = PackKey(path, filter, resolved.fps, quality, ceiling,
            frameBudget);
        const std::wstring packPath = PackPath(key);

        if (stats) *stats = PackStats{};

        // Already built on a previous run.
        if (mode == Preprocess::Disk && pack.AdoptFile(packPath)) {
            Touch(packPath);
            Log(LogLevel::Info, L"preprocessed: %zu frames from cache, %zu MB",
                pack.FrameCount(), pack.Bytes() / (1024 * 1024));
            if (stats) {
                stats->frames = pack.FrameCount();
                stats->bytes = pack.Bytes();
                stats->fromCache = true;
            }
            return true;
        }

        // Asked for what is already built and it is not there. Saying so is
        // the whole point: the caller has somewhere better to do this.
        if (adoptOnly) return false;

        const DWORD began = GetTickCount();
        if (perFrame)
            Log(LogLevel::Info,
                L"preprocessing, best quality per frame (%zu KB a frame, ceiling %d%%)...",
                frameBudget / 1024, ceiling);
        else
            Log(LogLevel::Info, L"preprocessing at -q:v %d...", quality);

        BuildResult result;

        // Which builder, decided once so the two sinks below cannot disagree.
        auto build = [&](Sink& sink, size_t budget) {
            return perFrame
                ? RunBuildPerFrame(ffmpeg, path, filter, resolved.hwaccel, resolved.fps,
                    ceiling, frameBudget, sink, budget, abort, abortUser, result,
                    onProgress, progressUser)
                : RunBuild(ffmpeg, path, filter, resolved.hwaccel, resolved.fps, quality,
                    sink, budget, abort, abortUser, result, onProgress, progressUser);
        };

        if (mode == Preprocess::Memory) {
            std::vector<uint8_t> blob;
            Sink sink;
            sink.memory = &blob;

            const uint64_t budget = MemoryBudget();

            if (!build(sink, (size_t)budget)) return false;
            if (result.cancelled || aborted()) return false;

            if (result.overBudget) {
                Log(LogLevel::Warn,
                    L"too large to hold in memory (over %llu MB) - streaming instead",
                    (unsigned long long)(budget / (1024 * 1024)));
                return false;
            }
            if (result.index.empty()) {
                Log(LogLevel::Error, L"preprocessing produced no frames");
                return false;
            }

            const size_t bytes = blob.size();
            pack.AdoptMemory(std::move(blob), std::move(result.index), resolved.fps);

            if (stats) {
                stats->frames = pack.FrameCount();
                stats->uniqueFrames = result.unique;
                stats->bytes = bytes;
                stats->buildSeconds = (GetTickCount() - began) / 1000.0;
            }
        }
        else {
            // Written straight to its final name, and made valid only by the
            // last write to it.
            //
            // The magic goes in at the end, so a build that crashes, is
            // cancelled or is killed leaves a file whose first four bytes are
            // zero — which AdoptFile rejects, exactly as it rejects anything
            // else that is not a pack. That is the same guarantee writing to a
            // temporary name and renaming it gave, without the rename.
            //
            // The rename is worth being rid of. It failed here with
            // ERROR_SHARING_VIOLATION for 32 seconds at a stretch and cost
            // every large pack that was built — around a minute of encoding
            // each, thrown away at the last step — while small ones went
            // through untouched. The temporary name carried the process id and
            // nothing else, so any two builds of one video in one process
            // shared it, and the second to open it held the first's file while
            // the first tried to move it. Nothing here needs that file to
            // exist.
            // Both flags here are load-bearing, and between them they are the
            // answer to a failure that cost every large pack this machine built.
            //
            // "N" is no-inherit, and it is the important one. This file is
            // opened before build() spawns ffmpeg, and StartFfmpegPipe creates
            // that child with handle inheritance on — so ffmpeg was being
            // handed a duplicate of the handle to the pack. Our own fclose then
            // released nothing that mattered: the child held the file until it
            // exited, and the adopt at the end of this function got
            // ERROR_SHARING_VIOLATION from what looked like nowhere. Small packs
            // came out fine because ffmpeg had already gone by then, which is
            // what made it look like a size limit or a virus scanner.
            //
            // _SH_DENYWR then lets readers in and keeps other writers out,
            // which is the sharing this actually wants — and is what
            // _wfopen_s, whose handles are not shareable at all, would not give.
            FILE* f = _wfsopen(packPath.c_str(), L"wbN", _SH_DENYWR);
            if (!f) {
                Log(LogLevel::Warn, L"could not write a pack file - streaming instead");
                return false;
            }

            PackHeader head{};
            head.version = kPackVersion;
            head.fps = (uint32_t)resolved.fps;
            head.width = kPanelWidth;
            head.height = kPanelHeight;
            head.quality = (uint32_t)quality;   // 0 marks a per-frame pack
            head.reserved = (uint32_t)ceiling;
            memcpy(head.key, key.data(), 16);

            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
                head.sourceSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                head.sourceMtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32)
                    | fad.ftLastWriteTime.dwLowDateTime;
            }

            // A placeholder for now, rewritten once the counts are known.
            fwrite(&head, 1, sizeof(head), f);

            Sink sink;
            sink.file = f;
            sink.offset = sizeof(head);

            bool built = build(sink, 0);

            bool good = built && !result.cancelled && !aborted() && !result.index.empty();

            if (good) {
                head.indexOffset = sink.offset;
                head.frameCount = (uint32_t)result.index.size();
                head.uniqueCount = (uint32_t)result.unique;

                for (const FramePack::Ref& r : result.index) {
                    PackIndexEntry e{ r.offset, r.length };
                    if (fwrite(&e, 1, sizeof(e), f) != sizeof(e)) { good = false; break; }
                }
            }

            if (good) {
                // Stamped last, and this is the moment the file becomes a pack.
                memcpy(head.magic, "JLP1", 4);

                fseek(f, 0, SEEK_SET);
                good = fwrite(&head, 1, sizeof(head), f) == sizeof(head);
            }

            fclose(f);

            if (!good) {
                DeleteFileW(packPath.c_str());
                if (result.cancelled || aborted()) return false;
                Log(LogLevel::Warn,
                    L"could not finish the pack after %zu frames - streaming instead",
                    result.index.size());
                return false;
            }

            EnforceDiskBudget(packPath);

            if (!pack.AdoptFile(packPath)) {
                Log(LogLevel::Warn, L"the pack just written would not load - streaming instead");
                return false;
            }

            if (stats) {
                stats->frames = pack.FrameCount();
                stats->uniqueFrames = result.unique;
                stats->bytes = pack.Bytes();
                stats->buildSeconds = (GetTickCount() - began) / 1000.0;
            }
        }

        if (perFrame)
            Log(LogLevel::Info,
                L"preprocessed: %zu frames (%zu distinct), %zu MB, quality %d%% "
                L"(%d-%d), %.0f KB a frame, %.1f encodes/frame, %.1fs - ffmpeg is done",
                pack.FrameCount(), result.unique, pack.Bytes() / (1024 * 1024),
                result.MeanQuality(), result.qualityMin, result.qualityMax,
                pack.FrameCount() ? (double)pack.Bytes() / pack.FrameCount() / 1024.0 : 0.0,
                result.qualityCount ? (double)result.encodes / result.qualityCount : 0.0,
                (GetTickCount() - began) / 1000.0);
        else
            Log(LogLevel::Info,
                L"preprocessed: %zu frames (%zu distinct), %zu MB, %.1fs - ffmpeg is done",
                pack.FrameCount(), result.unique, pack.Bytes() / (1024 * 1024),
                (GetTickCount() - began) / 1000.0);

        if (result.dropped)
            Log(LogLevel::Warn, L"%zu frames were over the panel's limit and left out",
                result.dropped);

        return true;
    }

    // -----------------------------------------------------------------------
    // PlayPack
    // -----------------------------------------------------------------------

    bool PlayPack(Device& device, const FramePack& pack, const RenderOpts& opts,
        AbortFn abort, void* abortUser,
        FrameFn onFrame, void* frameUser,
        PlaybackStats* stats,
        SeekFn takeSeek, void* seekUser,
        PositionFn onPosition, void* positionUser,
        const Compositor* comp,
        double startSeconds)
    {
        auto aborted = [&] { return abort && abort(abortUser); };

        if (!device.IsOpen()) {
            Log(LogLevel::Error, L"device is not open");
            return false;
        }
        if (!pack.IsOpen()) {
            Log(LogLevel::Error, L"no preprocessed frames to play");
            return false;
        }

        device.SendCommand(cmd::Live);
        Sleep(100);

        Log(LogLevel::Info, L"playing %zu preprocessed frames at %d fps%s...",
            pack.FrameCount(), pack.Fps(), opts.loop ? L", looping" : L"");

        detail::Pacer pace;
        pace.Start(pack.Fps());

        size_t sent = 0, dropped = 0, passes = 0;
        bool ok = true;
        bool running = true;

        const bool composing = comp && comp->Active();

        // One buffer for the whole run: SendImageFrame and the preview callback
        // both want a vector, and reusing this one keeps the per-frame cost to a
        // copy rather than an allocation.
        std::vector<uint8_t> scratch;
        scratch.reserve(kMaxJpegBytes);

        // Evenly spaced frames at a known rate, so the length is arithmetic
        // rather than something to go and ask ffprobe about.
        const int    fps = pack.Fps() > 0 ? pack.Fps() : 30;
        const double duration = (double)pack.FrameCount() / fps;

        // Where the first pass begins. Joining a song already in progress is
        // the music follower's normal case, and against a pack it costs an
        // index — no keyframe to land on, no ffmpeg to restart, and it is exact
        // rather than near.
        size_t startFrame = 0;
        if (startSeconds > 0.0) {
            const double clamped = startSeconds > duration ? duration : startSeconds;
            startFrame = (size_t)(clamped * fps);
            if (startFrame >= pack.FrameCount()) startFrame = pack.FrameCount() - 1;

            Log(LogLevel::Info, L"starting at %.1fs (frame %zu)", clamped, startFrame);
        }

        while (running) {
            ++passes;

            for (size_t i = startFrame; i < pack.FrameCount() && running; ++i) {
                if (aborted()) { running = false; break; }

                // The frames are already built and evenly spaced, so a seek is
                // an index and lands exactly where it was asked to.
                if (takeSeek) {
                    const double want = takeSeek(seekUser);
                    if (want >= 0.0) {
                        const size_t target = (size_t)(want * fps);

                        // Past the end is not a seek, it is the end. Clamping it
                        // to the last frame is what made a video shorter than
                        // the song it was following replay its final moment over
                        // and over: the follower kept asking for a position the
                        // video does not have, and every ask landed on the same
                        // frame. Ending here says so once, and the caller shows
                        // the card instead.
                        if (target >= pack.FrameCount()) { running = false; break; }

                        i = target;
                    }
                }

                if (onPosition) onPosition((double)i / fps, duration, positionUser);

                size_t len = 0;
                const uint8_t* bytes = pack.Frame(i, len);
                if (!bytes || len == 0) continue;

                scratch.assign(bytes, bytes + len);

                // Packed frames are already panel-ready, so this is the only
                // thing standing between the cache and the wire.
                if (composing && !comp->Apply(scratch)) { ++dropped; continue; }

                DWORD now = pace.WaitForSlot();

                if (!device.SendImageFrame(scratch)) { ok = false; running = false; break; }
                ++sent;

                if (onFrame) onFrame(scratch, frameUser);

                if (!pace.KeepAliveIfDue(device, now)) { ok = false; running = false; break; }

                if ((sent % 30) == 0) {
                    double secs = now / 1000.0;
                    Log(LogLevel::Progress, L"%zu frames, %.1f fps (preprocessed)",
                        sent, secs > 0 ? sent / secs : 0.0);
                }
            }

            // A lap that began part-way through has finished the video; the
            // next one starts at the top.
            startFrame = 0;

            if (!opts.loop || aborted()) running = false;
        }

        const double elapsed = pace.ElapsedMs() / 1000.0;

        Log(LogLevel::Info, L"%zu frames sent over %zu pass(es), no ffmpeg", sent, passes);

        if (stats) {
            stats->sent = sent;
            // A pack cannot hold a frame the panel would refuse, so this stays
            // zero unless a compositor re-encoded one past the cap.
            stats->dropped = dropped;
            stats->passes = passes;
            stats->fps = elapsed > 0 ? sent / elapsed : 0.0;
        }

        device.FlushEoi();
        return ok;
    }

    // -----------------------------------------------------------------------
    // Stills
    // -----------------------------------------------------------------------

    bool PrepareImageCached(const std::wstring& path, const RenderOpts& opts,
        Preprocess mode, std::vector<uint8_t>& jpeg)
    {
        if (mode == Preprocess::Off) return PrepareImage(path, opts, jpeg);

        const std::wstring filter = detail::BuildFilter(opts.stretch, opts.rotate);
        const std::string  key = StillKey(path, filter);

        if (StillCacheGet(key, jpeg)) return true;

        if (mode == Preprocess::Disk) {
            const std::wstring stillPath = StillPath(key);
            if (detail::ReadFileBytes(stillPath, jpeg) && jpeg.size() <= kMaxJpegBytes) {
                Touch(stillPath);
                StillCachePut(key, jpeg);
                Log(LogLevel::Info, L"prepared still from cache, %zu KB", jpeg.size() / 1024);
                return true;
            }
            jpeg.clear();
        }

        if (!PrepareImage(path, opts, jpeg)) return false;

        StillCachePut(key, jpeg);

        if (mode == Preprocess::Disk) {
            const std::wstring stillPath = StillPath(key);
            FILE* f = nullptr;
#ifdef _MSC_VER
            if (_wfopen_s(&f, stillPath.c_str(), L"wb") == 0 && f) {
#else
            f = _wfopen(stillPath.c_str(), L"wb");
            if (f) {
#endif
                fwrite(jpeg.data(), 1, jpeg.size(), f);
                fclose(f);
                EnforceDiskBudget(stillPath);
            }
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    void SetMemoryBudget(uint64_t bytes)
    {
        if (bytes == 0) bytes = kDefaultMemoryBudget;
        if (bytes < kMinMemoryBudget) bytes = kMinMemoryBudget;

        // A memory pack is addressed as one contiguous allocation, so on a
        // 32-bit build there is a hard ceiling no setting may cross.
        if (bytes > (uint64_t)SIZE_MAX / 2) bytes = (uint64_t)SIZE_MAX / 2;

        g_memoryBudget.store(bytes);
    }

    void SetDiskBudget(uint64_t bytes)
    {
        if (bytes == 0) bytes = kDefaultDiskBudget;
        if (bytes < kMinDiskBudget) bytes = kMinDiskBudget;
        g_diskBudget.store(bytes);
    }

    uint64_t MemoryBudget() { return g_memoryBudget.load(); }
    uint64_t DiskBudget() { return g_diskBudget.load(); }

    uint64_t PackCacheBytes()
    {
        uint64_t total = 0;
        for (const CacheEntry& e : ScanCache()) total += e.bytes;
        return total;
    }

    void PackCacheClear()
    {
        const std::wstring dir = PackDirectory();
        for (const CacheEntry& e : ScanCache())
            DeleteFileW((dir + L"\\" + e.name).c_str());

        EnterCriticalSection(&StillLock());
        Stills().clear();
        LeaveCriticalSection(&StillLock());
    }

}  // namespace jl
