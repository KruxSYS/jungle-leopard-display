// jl_media.cpp — reading the Windows media session (SMTC) behind jl_api.h.
//
// What is playing in the browser, as a struct the host can poll. The panel
// never touches audio; this exists so the manager can follow along with music
// somebody else is playing.
//
// Why this is native at all: the C# route to
// GlobalSystemMediaTransportControlsSession is the Windows SDK projection,
// which means retargeting the manager to net9.0-windows10.0.19041.0 and
// shipping Microsoft.Windows.SDK.NET.dll and WinRT.Runtime.dll — around twenty
// megabytes onto a portable drop that is under one. C++/WinRT is header-only,
// already in the SDK these projects target, and links into this DLL. The
// release stays three binaries.
//
// Everything here obeys the two rules the rest of the API does: nothing blocks
// the caller, and there is exactly one of it per process. A dedicated thread
// does all the WinRT work — it needs an apartment, and TryGetMediaPropertiesAsync
// is a cross-process call into the browser, which is not something a UI thread
// or the sensor pool may ever wait on.

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <windows.h>

#include "jl_api.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace {

    using namespace winrt::Windows::Media::Control;
    using winrt::Windows::Foundation::TimeSpan;
    using winrt::Windows::Storage::Streams::DataReader;
    using winrt::Windows::Storage::Streams::IRandomAccessStreamReference;

    using Session = GlobalSystemMediaTransportControlsSession;
    using Manager = GlobalSystemMediaTransportControlsSessionManager;
    using PlaybackStatus = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    // How often the reader looks at the session. The synchronous half of that
    // — playback status and the timeline — is a property read on an object we
    // already hold, so this costs nothing worth measuring. The expensive half
    // is rationed separately; see kPropsBackstopMs.
    constexpr DWORD kPollMs = 500;

    // TryGetMediaPropertiesAsync is a call into the browser, so it runs only
    // when something says the track may have changed: the session changed, the
    // MediaPropertiesChanged event fired, or the timeline's end time moved.
    // This is the backstop for the case where all three are missed — a dropped
    // event would otherwise leave the wrong title on the panel until the next
    // track. Only while something is playing; a paused session cannot change
    // tracks without telling us.
    constexpr long long kPropsBackstopMs = 4000;

    // A timeline update older than this is not extrapolated from. A session
    // that has been sitting still since yesterday would otherwise report a
    // position hours into a four-minute song.
    constexpr double kStaleTimelineSeconds = 3600.0;

    // How much a reported position must differ from the last one to count as
    // the player actually saying something new.
    //
    // This exists because a player may refresh its timeline's timestamp without
    // advancing the position in it. Trusting every report then re-bases the
    // clock onto the same stale number several times a second and the position
    // stands still — measured on YouTube Music in Firefox, frozen at 33.6 s
    // while the song ran on for another minute, which made the panel seek back
    // to the same spot over and over trying to "correct" it.
    //
    // Above this, the player has genuinely moved (a seek, a new track) and the
    // clock re-bases onto it. Below, our own clock carries the position forward.
    // A player reporting properly advances by a whole poll interval each time
    // and so re-bases every time, exactly as before.
    constexpr double kPositionEpsilon = 0.35;

    struct Guard {
        CRITICAL_SECTION& cs;
        explicit Guard(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
        ~Guard() { LeaveCriticalSection(&cs); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    double Seconds(TimeSpan ts)
    {
        return std::chrono::duration_cast<std::chrono::duration<double>>(ts).count();
    }

    long long NowQpcMs()
    {
        LARGE_INTEGER freq{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (freq.QuadPart == 0) return 0;
        return (now.QuadPart * 1000) / freq.QuadPart;
    }

    void CopyField(wchar_t* dst, size_t cch, const std::wstring& src)
    {
        if (!dst || cch == 0) return;
        size_t n = src.size() < cch - 1 ? src.size() : cch - 1;
        if (n) memcpy(dst, src.c_str(), n * sizeof(wchar_t));
        dst[n] = L'\0';
    }

    // -----------------------------------------------------------------------

    struct Reader {
        CRITICAL_SECTION lock{};

        HANDLE thread = nullptr;
        HANDLE stopEvent = nullptr;
        std::atomic<bool> stopping{ false };

        // Set by the MediaPropertiesChanged handler, which runs on a WinRT pool
        // thread and must do nothing more than this.
        std::atomic<bool> propsDirty{ true };

        // Which session to follow, or empty for automatic. Read by the reader
        // thread, written by whoever calls jl_media_start.
        std::wstring preferredApp;

        // --- everything below is guarded by `lock` -------------------------

        bool running = false;
        bool available = false;    // the session manager was obtained
        bool present = false;      // a session is being followed
        bool playing = false;

        std::wstring title, artist, album, sourceApp;

        // The position as reported, the moment it was true, and the rate it
        // moves at. Extrapolated at read time rather than at sample time, so a
        // caller polling every 250 ms gets a position that keeps moving between
        // the reader's own 500 ms ticks.
        double    basePosition = 0.0;
        double    duration = 0.0;
        double    rate = 1.0;
        long long baseAtMs = 0;
        bool      basePositionKnown = false;

        // The last position the player itself reported, to tell a real update
        // apart from the same figure repeated. See kPositionEpsilon.
        double    lastReported = -1.0;

        int32_t trackVersion = 0;
        int32_t thumbnailVersion = 0;
        std::vector<uint8_t> thumbnail;

        // App ids of every session currently registered, for the settings list.
        std::vector<std::wstring> sessionApps;

        Reader() { InitializeCriticalSection(&lock); }
    };

    Reader r;

    // -----------------------------------------------------------------------
    // The reader thread. Every WinRT call in this file happens on it.
    // -----------------------------------------------------------------------

    std::wstring ToWide(winrt::hstring const& h)
    {
        return std::wstring(h.c_str(), h.size());
    }

    bool SameSession(Session const& a, Session const& b)
    {
        if (!a || !b) return false;
        return winrt::get_abi(a) == winrt::get_abi(b);
    }

    // Picks the session to follow.
    //
    // An explicit choice wins outright. Otherwise a session that is actually
    // playing beats one that is not, and the one already being followed beats a
    // newcomer with equal claim — without that last rule the panel would flap
    // between two paused players every time the list is re-read.
    Session ChooseSession(Manager const& manager, Session const& current,
        std::wstring const& preferred)
    {
        Session best{ nullptr };
        bool bestPlaying = false;

        auto sessions = manager.GetSessions();
        for (uint32_t i = 0; i < sessions.Size(); ++i) {
            Session s = sessions.GetAt(i);
            if (!s) continue;

            std::wstring app;
            try { app = ToWide(s.SourceAppUserModelId()); }
            catch (...) { continue; }

            if (!preferred.empty()) {
                if (_wcsicmp(app.c_str(), preferred.c_str()) == 0) return s;
                continue;
            }

            bool isPlaying = false;
            try {
                isPlaying = s.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
            }
            catch (...) {}

            if (!best) { best = s; bestPlaying = isPlaying; continue; }
            if (isPlaying && !bestPlaying) { best = s; bestPlaying = true; continue; }
            if (isPlaying == bestPlaying && SameSession(s, current)) best = s;
        }

        // A preferred app that is not currently registered leaves nothing to
        // follow, which is the honest answer: the user named a player and that
        // player is not running.
        return best;
    }

    std::vector<uint8_t> ReadThumbnail(IRandomAccessStreamReference const& ref)
    {
        std::vector<uint8_t> bytes;
        if (!ref) return bytes;

        auto stream = ref.OpenReadAsync().get();
        if (!stream) return bytes;

        uint64_t size = stream.Size();
        // Album art is tens of kilobytes. Anything this far out is not art, and
        // reading it would only cost memory.
        if (size == 0 || size > (4u << 20)) return bytes;

        DataReader reader(stream.GetInputStreamAt(0));
        reader.LoadAsync((uint32_t)size).get();

        bytes.resize((size_t)size);
        reader.ReadBytes(bytes);
        return bytes;
    }

    // Pulls title/artist/album, and the art if the track actually changed.
    void RefreshProperties(Session const& session)
    {
        auto props = session.TryGetMediaPropertiesAsync().get();
        if (!props) return;

        std::wstring title, artist, album;
        try { title = ToWide(props.Title()); }      catch (...) {}
        try { artist = ToWide(props.Artist()); }    catch (...) {}
        try { album = ToWide(props.AlbumTitle()); } catch (...) {}

        // Some players put the artist in AlbumArtist and leave Artist empty.
        if (artist.empty()) {
            try { artist = ToWide(props.AlbumArtist()); } catch (...) {}
        }

        bool changed;
        {
            Guard g(r.lock);
            changed = title != r.title || artist != r.artist || album != r.album;
            if (changed) {
                r.title = title;
                r.artist = artist;
                r.album = album;
                r.trackVersion++;

                // A new track gets a new clock, and no inherited length: the
                // one thing worse than no end time is the previous song's.
                r.basePositionKnown = false;
                r.lastReported = -1.0;
                r.duration = 0.0;
            }
        }

        // The art is another cross-process read, so it is worth only when the
        // track itself moved — not on every backstop refresh.
        if (!changed) return;

        std::vector<uint8_t> art;
        try { art = ReadThumbnail(props.Thumbnail()); }
        catch (...) {}

        Guard g(r.lock);
        if (art != r.thumbnail) {
            r.thumbnail = std::move(art);
            r.thumbnailVersion++;
        }
    }

    void RefreshTimeline(Session const& session)
    {
        bool playing = false;
        double rate = 1.0;

        try {
            auto info = session.GetPlaybackInfo();
            playing = info.PlaybackStatus() == PlaybackStatus::Playing;
            if (auto ref = info.PlaybackRate()) {
                double v = ref.Value();
                if (v > 0.01 && v < 8.0) rate = v;
            }
        }
        catch (...) {}

        double position = 0.0, duration = 0.0;

        // The player's own figures, kept apart from `position` because that one
        // gets the age added to it below.
        double reported = 0.0, end = 0.0;

        bool known = false;

        try {
            auto tl = session.GetTimelineProperties();
            double start = Seconds(tl.StartTime());
            end = Seconds(tl.EndTime());
            position = Seconds(tl.Position());
            reported = position;
            duration = end > start ? end - start : 0.0;

            // The report is "the position was P at time T", and T can be
            // several seconds ago — browsers refresh the timeline every few
            // seconds, not every frame. Carrying it forward here is what makes
            // the figure usable for anything but a progress bar that jumps.
            auto updated = tl.LastUpdatedTime();
            if (updated.time_since_epoch().count() != 0) {
                double age = Seconds(winrt::clock::now() - updated);
                if (age >= 0.0 && age < kStaleTimelineSeconds) {
                    if (playing) position += age * rate;
                    known = true;
                }
            }
            else {
                // No timestamp: take the position at face value and let the
                // read-time extrapolation run from now.
                known = true;
            }
        }
        catch (...) {}

        if (duration > 0.0 && position > duration) position = duration;
        if (position < 0.0) position = 0.0;

        // A timeline that says nothing at all: no end, and a position of zero.
        //
        // This is not a rare shape — it is what YouTube Music in Firefox
        // reports for most tracks — and taking it at face value was wrong in a
        // way that took a while to see. The position above is then entirely
        // `age`, the time since the player last stamped its timeline, which
        // works only for as long as that stamp stands still. Firefox re-stamps
        // it mid-song, and the moment it does, this reads as the song having
        // jumped back to about three seconds.
        //
        // Downstream that is indistinguishable from the listener dragging the
        // scrubber to the start, so the follower dutifully hauled the video
        // back to the beginning, once every couple of minutes, all night. The
        // log is full of "followed a seek towards the start (3.6s)" that nobody
        // asked for.
        //
        // So a timeline in this state is treated as no information at all. It
        // is worth one guess when a track is first seen — `age` is genuinely
        // the best estimate of how far in we are joining — and after that our
        // own clock carries the position and the player is not allowed to move
        // it again.
        const bool degenerate = end <= 0.0 && reported <= 0.0;

        Guard g(r.lock);

        const bool wasPlaying = r.playing;

        // Where our own clock had got to, worked out before the playing flag
        // moves underneath it.
        double carried = r.basePosition;
        if (r.basePositionKnown && wasPlaying) {
            const double secs = (double)(NowQpcMs() - r.baseAtMs) / 1000.0;
            if (secs > 0.0) carried += secs * r.rate;
        }
        if (carried < 0.0) carried = 0.0;

        r.playing = playing;
        r.rate = rate;

        // The length is kept as well as the position, and for the same reason.
        // A timeline that stops describing itself reports no end either, and
        // letting that through set the track's length to 0 midway through the
        // song — measured on the same track ten seconds apart: 113.94, then
        // nothing. Everything drawn from it went with it, which is the end time
        // that kept being wrong. A track change clears this; a player that has
        // gone quiet does not.
        if (!degenerate || duration > 0.0) r.duration = duration;

        if (!known) {
            r.basePositionKnown = false;
            r.lastReported = -1.0;
            return;
        }

        if (degenerate) {
            // Whether we already have a clock for this track is the whole test,
            // and it took two goes to get right. Asking instead whether the
            // clock had been *started* from a timeline like this one missed the
            // case that actually happens: a track that described itself
            // perfectly well to begin with and then stopped. That still walked
            // straight into the re-base, and the position still collapsed.
            if (!r.basePositionKnown) {
                // First sight of this track. Straight after a track change
                // `age` is small, which is exactly right.
                r.basePosition = position;
                r.baseAtMs = NowQpcMs();
            }
            else if (wasPlaying != playing) {
                // Only the clock is restarted, never the position: a pause must
                // not count the time it was paused for, and a resume must not
                // lose where we were.
                r.basePosition = carried;
                r.baseAtMs = NowQpcMs();
            }

            r.basePositionKnown = true;

            // Nothing credible was reported, so there is nothing to compare
            // against next time either.
            r.lastReported = -1.0;
            return;
        }

        // A player that describes its timeline properly. Its word is better
        // than our clock, so the ordinary re-basing below applies.

        // Re-base only when the player says something new: a position that
        // actually moved, a pause or resume (the clock must not count time that
        // passed while stopped), or the first reading after nothing was known.
        const bool moved = r.lastReported < 0.0
            || std::fabs(position - r.lastReported) > kPositionEpsilon;

        if (moved || wasPlaying != playing || !r.basePositionKnown) {
            r.lastReported = position;
            r.basePosition = position;
            r.baseAtMs = NowQpcMs();
        }

        r.basePositionKnown = true;
    }

    void ClearTrackLocked()
    {
        r.present = false;
        r.playing = false;
        r.basePositionKnown = false;
        r.basePosition = 0.0;
        r.lastReported = -1.0;
        r.duration = 0.0;
        if (!r.title.empty() || !r.artist.empty() || !r.album.empty()) {
            r.title.clear();
            r.artist.clear();
            r.album.clear();
            r.trackVersion++;
        }
        r.sourceApp.clear();
    }

    DWORD WINAPI ReaderProc(LPVOID)
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        Manager manager{ nullptr };
        Session session{ nullptr };
        winrt::event_token propsToken{};
        double lastEnd = -1.0;
        long long lastPropsAt = 0;

        auto unsubscribe = [&] {
            if (session && propsToken) {
                try { session.MediaPropertiesChanged(propsToken); }
                catch (...) {}
            }
            propsToken = {};
        };

        while (!r.stopping.load()) {
            try {
                if (!manager) {
                    manager = Manager::RequestAsync().get();
                    Guard g(r.lock);
                    r.available = manager != nullptr;
                }

                if (manager) {
                    std::wstring preferred;
                    {
                        Guard g(r.lock);
                        preferred = r.preferredApp;
                    }

                    Session chosen = ChooseSession(manager, session, preferred);

                    if (!SameSession(chosen, session)) {
                        unsubscribe();
                        session = chosen;
                        lastEnd = -1.0;

                        if (session) {
                            // The handler runs on a WinRT pool thread. It sets a
                            // flag and returns; anything more would be doing the
                            // reader's work on somebody else's thread.
                            propsToken = session.MediaPropertiesChanged(
                                [](Session const&, MediaPropertiesChangedEventArgs const&) {
                                    r.propsDirty.store(true);
                                });
                            r.propsDirty.store(true);
                        }
                        else {
                            Guard g(r.lock);
                            ClearTrackLocked();
                        }
                    }

                    // The session list, for the settings picker. Refreshed here
                    // rather than on demand because a caller asking for it is on
                    // the UI thread and has no apartment to make WinRT calls in.
                    {
                        std::vector<std::wstring> apps;
                        auto all = manager.GetSessions();
                        for (uint32_t i = 0; i < all.Size(); ++i) {
                            try { apps.push_back(ToWide(all.GetAt(i).SourceAppUserModelId())); }
                            catch (...) {}
                        }
                        Guard g(r.lock);
                        r.sessionApps = std::move(apps);
                    }

                    if (session) {
                        {
                            Guard g(r.lock);
                            r.present = true;
                            try { r.sourceApp = ToWide(session.SourceAppUserModelId()); }
                            catch (...) {}
                        }

                        RefreshTimeline(session);

                        // A track change moves the end time even when the event
                        // is missed, which makes this a second, free signal.
                        double end;
                        bool playing;
                        {
                            Guard g(r.lock);
                            end = r.duration;
                            playing = r.playing;
                        }
                        if (end != lastEnd) {
                            lastEnd = end;
                            r.propsDirty.store(true);
                        }

                        long long now = NowQpcMs();
                        if (playing && now - lastPropsAt > kPropsBackstopMs)
                            r.propsDirty.store(true);

                        if (r.propsDirty.exchange(false)) {
                            lastPropsAt = now;
                            try { RefreshProperties(session); }
                            catch (...) {}
                        }
                    }
                }
            }
            catch (...) {
                // A session that vanished mid-read, a browser shutting down, or
                // a machine where the API is not there at all. None of it is
                // worth failing over: drop the manager and try again next tick.
                unsubscribe();
                session = nullptr;
                manager = nullptr;
                Guard g(r.lock);
                r.available = false;
                ClearTrackLocked();
            }

            WaitForSingleObject(r.stopEvent, kPollMs);
        }

        unsubscribe();
        session = nullptr;
        manager = nullptr;

        winrt::uninit_apartment();
        return 0;
    }

}  // namespace

// ---------------------------------------------------------------------------

extern "C" {

    int32_t jl_media_start(const wchar_t* preferredApp)
    {
        Guard g(r.lock);

        r.preferredApp = preferredApp ? preferredApp : L"";

        if (r.running) {
            // Changing the followed player is a restart of nothing: the reader
            // re-reads the preference every tick.
            r.propsDirty.store(true);
            return JL_OK;
        }

        if (!r.stopEvent) r.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!r.stopEvent) return JL_ERR_FAILED;

        ResetEvent(r.stopEvent);
        r.stopping.store(false);
        r.propsDirty.store(true);

        r.thread = CreateThread(nullptr, 0, ReaderProc, nullptr, 0, nullptr);
        if (!r.thread) return JL_ERR_FAILED;

        r.running = true;
        return JL_OK;
    }

    void jl_media_stop(void)
    {
        HANDLE thread = nullptr;
        {
            Guard g(r.lock);
            if (!r.running) return;
            thread = r.thread;
            r.thread = nullptr;
            r.running = false;
        }

        r.stopping.store(true);
        if (r.stopEvent) SetEvent(r.stopEvent);

        if (thread) {
            // The reader can be inside a call into the browser, which is why
            // this waits rather than assuming a prompt exit.
            WaitForSingleObject(thread, 10000);
            CloseHandle(thread);
        }

        Guard g(r.lock);
        ClearTrackLocked();
        r.available = false;
        r.sessionApps.clear();
        r.thumbnail.clear();
    }

    void jl_media_get(JlMediaStatus* out)
    {
        if (!out) return;
        memset(out, 0, sizeof(*out));

        Guard g(r.lock);

        out->running = r.running ? 1 : 0;
        out->available = r.available ? 1 : 0;
        out->present = r.present ? 1 : 0;
        out->playing = r.playing ? 1 : 0;
        out->duration = r.duration;
        out->trackVersion = r.trackVersion;
        out->thumbnailVersion = r.thumbnailVersion;

        double position = r.basePosition;
        if (r.basePositionKnown && r.playing) {
            double elapsed = (NowQpcMs() - r.baseAtMs) / 1000.0;
            if (elapsed > 0.0) position += elapsed * r.rate;
        }
        if (r.duration > 0.0 && position > r.duration) position = r.duration;
        if (position < 0.0) position = 0.0;
        out->position = r.basePositionKnown ? position : 0.0;

        CopyField(out->title, 256, r.title);
        CopyField(out->artist, 256, r.artist);
        CopyField(out->album, 256, r.album);
        CopyField(out->sourceApp, 128, r.sourceApp);
    }

    int32_t jl_media_sessions(wchar_t* buf, int32_t cch)
    {
        Guard g(r.lock);

        // NUL between entries, an extra NUL at the end — the same shape as the
        // multi-string Windows uses everywhere, so the caller can split on it
        // without a length prefix.
        size_t need = 1;
        for (const std::wstring& app : r.sessionApps) need += app.size() + 1;

        if (!buf || cch <= 0 || (size_t)cch < need) return (int32_t)need;

        wchar_t* p = buf;
        for (const std::wstring& app : r.sessionApps) {
            memcpy(p, app.c_str(), (app.size() + 1) * sizeof(wchar_t));
            p += app.size() + 1;
        }
        *p = L'\0';
        return (int32_t)need;
    }

    int32_t jl_media_thumbnail(uint8_t* buf, int32_t cap)
    {
        Guard g(r.lock);

        int32_t size = (int32_t)r.thumbnail.size();
        if (size == 0) return 0;
        if (!buf || cap < size) return size;

        memcpy(buf, r.thumbnail.data(), (size_t)size);
        return size;
    }

}  // extern "C"
