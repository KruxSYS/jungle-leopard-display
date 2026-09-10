using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Threading;

using JLDisplayManager.Interop;
using JLDisplayManager.Models;
using JLDisplayManager.Models.Overlay;
using JLDisplayManager.Services.Overlay;

namespace JLDisplayManager.Services.Music;

/// <summary>
/// Puts whatever is playing elsewhere on this machine onto the panel.
///
/// The third thing that can own the panel, beside the playlist and the AI
/// pipeline — and like them it must put itself back after a reconnect, because
/// nothing else knows what it was showing.
///
/// It plays nothing and controls nothing. The browser is the remote control;
/// this follows it. Two things can be on the glass: the music video, streamed
/// live, or a card — the album art blurred across the panel as a still, with
/// the title, artist and progress drawn on top by the ordinary overlay
/// renderer. The card is not a consolation prize: most tracks on a music
/// service have no video, and it is what they get.
/// </summary>
public sealed class MusicFollower : INotifyPropertyChanged, IDisposable
{
    /// <summary>
    /// How often the session is looked at. The reader is already sampling at
    /// 500 ms, so anything finer would only re-read the same struct.
    /// </summary>
    private static readonly TimeSpan TickInterval = TimeSpan.FromMilliseconds(500);

    /// <summary>
    /// How far ahead of the reported position a stream is started.
    ///
    /// ffmpeg has to connect, fetch and decode before it hands over a first
    /// frame, and by then the song has moved on. Starting at exactly the
    /// reported position therefore lands permanently behind by however long
    /// that took. This is the allowance for it — deliberately modest, because
    /// being slightly late is less noticeable than being early.
    /// </summary>
    private const double StartLeadSeconds = 0.3;

    /// <summary>
    /// How far out the panel may drift before it is worth correcting.
    ///
    /// A correction is not free: a seek restarts ffmpeg at the mark and lands on
    /// the nearest keyframe before it, which is a visible hitch. Below this the
    /// hitch costs more than the drift does.
    /// </summary>
    private const double DriftToleranceSeconds = 1.5;

    /// <summary>
    /// The least time between two corrections. Without it a stream that cannot
    /// hold sync — a slow link, a machine that cannot decode fast enough —
    /// would seek every few seconds and never show anything but keyframe jumps.
    /// </summary>
    private static readonly TimeSpan CorrectionCooldown = TimeSpan.FromSeconds(20);

    /// <summary>
    /// Consecutive ticks a gap must persist before it is believed.
    ///
    /// A track turning over looks exactly like a huge backwards jump for a
    /// moment: the browser resets the timeline the instant the next song
    /// starts, but the title follows a beat later, so until it arrives the
    /// follower sees the *old* track apparently seeking to zero. Acting on that
    /// yanks the outgoing video back to its start, right before the new track
    /// replaces it anyway.
    ///
    /// Measured on Firefox, the title can lag the timeline reset by more than
    /// two seconds, so this is deliberately generous. It costs nothing when the
    /// gap is real: a genuine scrub in the browser still gets followed, just a
    /// moment later.
    /// </summary>
    private const int DriftTicksBeforeActing = 8;

    /// <summary>
    /// A position this close to the start is treated as a track that has just
    /// begun rather than as a seek to the beginning.
    ///
    /// This is the other half of the same problem. Between the browser resetting
    /// the timeline and the new title arriving, the follower sees the old track
    /// apparently jumping to zero — and "jumped to zero" is exactly what a huge
    /// backwards gap plus a position at the very start means. Nobody scrubs a
    /// song back to 0:02 and leaves it there; a song that changed does that
    /// every single time.
    /// </summary>
    private const double TrackStartSeconds = 30.0;

    /// <summary>
    /// How far the song may land from where our own clock expected it before it
    /// counts as somebody having dragged the browser's scrubber.
    ///
    /// A seek is not drift and must not be treated as one: drift is small and
    /// accumulates, so it is worth confirming over several ticks and rationing;
    /// a seek is large, instant and deliberate, and waiting on it just leaves
    /// the panel showing the wrong part of the song. Detecting it is what makes
    /// scrubbing in the browser move the panel at all.
    /// </summary>
    private const double SeekJumpSeconds = 2.0;

    /// <summary>
    /// A jump landing this close to the start cannot be judged on the spot: a
    /// new track and a scrub backwards look identical for a moment, because the
    /// timeline resets a beat before the title arrives.
    ///
    /// It is not guessed at. The jump is held for <see cref="ZeroJumpGrace"/>
    /// and then decided by whether the track identity actually changed, which
    /// is the thing that genuinely tells them apart.
    ///
    /// This matters far more than it looks: measured on Firefox, dragging the
    /// scrubber makes the player report a position near zero *first*, and only
    /// then the place you dragged to. Treating that as a track change threw
    /// away almost every real seek.
    /// </summary>
    private const double TrackTurnoverSeconds = 3.0;

    /// <summary>
    /// How long a jump towards the start is held before it is taken at face
    /// value. Longer than the title lag that causes the ambiguity, short enough
    /// that following a scrub still feels immediate.
    /// </summary>
    private static readonly TimeSpan ZeroJumpGrace = TimeSpan.FromSeconds(2.5);

    /// <summary>
    /// How often the downloaded videos are swept for ones that have aged out.
    ///
    /// Hourly rather than only after a download, which is what the sweep used
    /// to be tied to: somebody who stops adding new music would otherwise keep
    /// every video they ever fetched, because nothing would ever run the check
    /// again.
    /// </summary>
    private static readonly TimeSpan SweepInterval = TimeSpan.FromHours(1);

    /// <summary>
    /// How far a video's length may be from the track's before the match is
    /// rejected.
    ///
    /// This is the check the search itself often cannot make. The browser
    /// frequently reports no track length for the first few seconds, so the
    /// search runs on a title alone and can land on a thirty-second clip for a
    /// four-minute song — which then "ends" moments after it starts, because
    /// the seek to the track's position lands past its end. By the time the
    /// video has been fetched the real length has usually arrived, so the
    /// comparison is made here instead, where it can still be acted on.
    /// </summary>
    private const double LengthMatchToleranceSeconds = 5.0;

    /// <summary>
    /// The shortest video that could plausibly be a song, used only when the
    /// track's own length is unknown.
    ///
    /// Without a length to compare against, a title match alone will happily
    /// accept a thirty-second promo clip or a Short — and those are exactly
    /// what a search returns alongside the real thing. The cache filled with
    /// 0.8 MB files before this existed, each of which "ended" a moment after
    /// it started.
    /// </summary>
    private const double MinPlausibleVideoSeconds = 75.0;

    /// <summary>
    /// How long to wait for the track's length to arrive before resolving
    /// without it.
    ///
    /// It is worth waiting for: the length is what makes a match trustworthy,
    /// and this player usually reports it a second or two after the track
    /// starts. Some tracks never report one at all, which is why this gives up
    /// rather than waiting forever.
    /// </summary>
    private static readonly TimeSpan DurationGrace = TimeSpan.FromSeconds(6);

    /// <summary>
    /// Consecutive ticks the session must report "not playing" before a video
    /// is torn down for it.
    ///
    /// Everything this player reports is a little late and a little jittery,
    /// and the playback flag is no exception: it dips for a moment as one track
    /// hands over to the next. Acting on a single tick of that pulled down a
    /// perfectly good video and put the card up, over and over — a card for six
    /// seconds, a video for two, repeatedly, for as long as the music played.
    ///
    /// A genuine pause still reaches the card in about a second and a half,
    /// which nobody will notice.
    /// </summary>
    private const int PauseTicksBeforeCard = 6;

    /// <summary>
    /// How long a stream may produce nothing before it is written off.
    ///
    /// The native side reports "playing" from the moment the worker starts,
    /// which is before ffmpeg has connected — so a stream that never connects
    /// looks exactly like one that is about to. Without this the follower waits
    /// on it forever, holding a stale frame on the panel. Longer than ffmpeg's
    /// own read timeout, so ffmpeg normally gives up first and this is only the
    /// backstop.
    /// </summary>
    private static readonly TimeSpan StreamStartTimeout = TimeSpan.FromSeconds(12);

    private enum Showing { Nothing, Card, Video }

    private readonly DisplayService _display;
    private readonly OverlayService _overlay;
    private readonly OverlaySettings _overlays;
    private readonly AppSettings _settings;
    private readonly DispatcherTimer _tick;
    private readonly Dispatcher _dispatcher;

    private bool _running;
    private string _summary = "";
    private string _detail = "";

    private Showing _showing = Showing.Nothing;

    // What the panel is currently holding, so an unchanged card is left alone:
    // re-showing a still restarts the native worker and blinks the panel.
    private string? _shownPath;
    private int _lastArtVersion = -1;

    // Track versions, which is how a track change is noticed: the same song
    // played twice is a new track, and comparing titles would miss that.
    private int _streamingTrack = -1;
    private int _resolvingTrack = -1;

    private string? _streamingFile;
    private CancellationTokenSource? _resolve;
    private DateTime _lastCorrection = DateTime.MinValue;
    private int _driftStreak;
    private DateTime _streamStartedAt = DateTime.MinValue;
    private DateTime _trackSeenAt = DateTime.MinValue;
    private int _seenTrack = -1;

    // Where the song was, and when — the reference a jump is measured against.
    private double _lastTrackPos = -1;
    private DateTime _lastTrackPosAt = DateTime.MinValue;

    // A jump towards the start that is waiting to be explained, and the track
    // it happened on. If that track is still current when the grace runs out,
    // nothing changed songs and it was a seek.
    private DateTime _zeroJumpAt = DateTime.MinValue;
    private int _zeroJumpVersion = -1;
    private DateTime _lastSweep = DateTime.MinValue;
    private int _pauseStreak;
    private long _lastFramesSent = -1;

    // The user's overlay, put back when following stops.
    private Guid? _previousProfile;
    private bool _previousEnabled;
    private bool _remembered;
    private bool _cardOverlay;

    public MusicFollower(DisplayService display, OverlayService overlay,
                         OverlaySettings overlays, AppSettings settings)
    {
        _display = display;
        _overlay = overlay;
        _overlays = overlays;
        _settings = settings;
        _dispatcher = Dispatcher.CurrentDispatcher;

        _tick = new DispatcherTimer { Interval = TickInterval };
        _tick.Tick += (_, _) => Tick();

        // A panel that came back has nothing on it. The playlist and the
        // pipeline each do this for themselves for the same reason.
        _display.Reconnected += (_, _) =>
        {
            if (!_running) return;
            _shownPath = null;
            _streamingTrack = -1;
            _showing = Showing.Nothing;
            Tick();
        };
    }

    public bool Running
    {
        get => _running;
        private set => Set(ref _running, value);
    }

    /// <summary>What is playing, for the tray and the window.</summary>
    public string Summary
    {
        get => _summary;
        private set => Set(ref _summary, value);
    }

    /// <summary>
    /// What the follower is doing about it — searching, streaming, or showing
    /// the card. "It picked the wrong video" is the complaint this feature will
    /// get, and this is where somebody can see what it picked.
    /// </summary>
    public string Detail
    {
        get => _detail;
        private set => Set(ref _detail, value);
    }

    public void Start()
    {
        if (_running) return;

        MediaSession.Acquire(PreferredApp());
        RememberOverlay();

        // Once at the start, so a machine left off for a fortnight clears out
        // what it stopped listening to as soon as it comes back.
        SweepCacheIfDue(force: true);

        Running = true;
        _shownPath = null;
        _showing = Showing.Nothing;
        _lastArtVersion = -1;
        _streamingTrack = -1;
        _resolvingTrack = -1;

        Tick();
        _tick.Start();
    }

    public void Stop()
    {
        if (!_running) return;

        _tick.Stop();
        CancelResolve();

        Running = false;
        Summary = "";
        Detail = "";
        _shownPath = null;
        _showing = Showing.Nothing;
        _streamingTrack = -1;

        RestoreOverlay();
        MediaSession.Release();
    }

    /// <summary>
    /// Points the reader at a different player. Called when the setting changes,
    /// so it takes effect without stopping.
    /// </summary>
    public void Repoint()
    {
        if (!_running) return;
        MediaSession.Repoint(PreferredApp());
    }

    /// <summary>
    /// Clears out aged and over-budget downloads, on a long interval and off
    /// the UI thread — it touches the filesystem, and the tick it runs from is
    /// the dispatcher's.
    /// </summary>
    private void SweepCacheIfDue(bool force)
    {
        if (!force && DateTime.UtcNow - _lastSweep < SweepInterval) return;
        _lastSweep = DateTime.UtcNow;

        MusicVideoCache.Configure(_settings.MusicCacheMB, _settings.MusicCacheDays);
        Task.Run(MusicVideoCache.Sweep);
    }

    private string? PreferredApp() =>
        string.IsNullOrWhiteSpace(_settings.MediaSessionApp) ? null : _settings.MediaSessionApp;

    // -----------------------------------------------------------------------

    private void Tick()
    {
        NativeMethods.JlMediaStatus m = MediaSession.Read();
        Summary = Describe(m);

        SweepCacheIfDue(force: false);

        int art = MusicArt.Version;
        if (art != _lastArtVersion)
        {
            _lastArtVersion = art;

            // The renderer caches decoded images by path forever. Album art is a
            // new path per track, so without this every track played would stay
            // in memory for the life of the process.
            OverlayRenderer.ClearImageCache();

            // A new backdrop is a different file, so the card has to be re-shown.
            if (_showing == Showing.Card) _shownPath = null;
        }

        // A pause has to hold before it is believed — see PauseTicksBeforeCard.
        // Nothing else here is debounced because nothing else flaps: a track
        // change is a version bump, which is unambiguous.
        bool idle = m.Present == 0 || m.Playing == 0;
        _pauseStreak = idle ? _pauseStreak + 1 : 0;

        bool settledIdle = idle && (_pauseStreak >= PauseTicksBeforeCard
            || _showing != Showing.Video);

        // Nothing playing, paused, or video turned off: the card, and no
        // searching. A paused song with its video still running would be worse
        // than no video at all.
        if (!_settings.MusicVideo || settledIdle)
        {
            CancelResolve();
            if (_showing == Showing.Video) LeaveVideo();
            ShowCard();

            if (_settings.MusicVideo && m.Present != 0 && m.Playing == 0) Detail = "Paused";
            else if (!_settings.MusicVideo) Detail = "";
            return;
        }

        // Reported not-playing, but not for long enough to act on. Leave the
        // video exactly as it is and wait for the next tick to settle it.
        if (idle) return;

        // A track we are already streaming: leave it alone but keep it in step.
        if (m.TrackVersion == _streamingTrack)
        {
            if (_display.State == NativeMethods.JlState.Playing)
            {
                long frames = _display.FramesSent;

                // Still connecting — or never going to. "Playing" is set when
                // the worker starts, not when a picture appears, so the frame
                // count is the only thing that tells the two apart.
                if (frames == 0)
                {
                    if (DateTime.UtcNow - _streamStartedAt <= StreamStartTimeout) return;

                    string? dead = _streamingFile;
                    LeaveVideo();
                    ReportDeadVideo(dead);
                    ShowCard();
                    return;
                }

                CorrectDrift(m, frames);
                return;
            }

            // The stream stopped. A video shorter than the song is one reason;
            // a stream that never started is the other, and telling them apart
            // matters — "ended" on something that never played a frame sends
            // anyone debugging this to entirely the wrong place.
            //
            // The usual cause of the second is ffmpeg having no route to the
            // network: it fails to connect and exits, and nothing else in the
            // app would ever say so.
            bool played = _display.FramesSent > 0;

            LeaveVideo();

            if (played) Detail = "Video ended";
            else ReportDeadVideo(_streamingFile);

            ShowCard();
            return;
        }

        // A new track. The card goes up immediately and the video replaces it if
        // and when one is found, so a slow or failing search degrades to the
        // card rather than to a blank panel.
        ShowCard();
        BeginPrepare(m);
    }

    // -----------------------------------------------------------------------
    // The card
    // -----------------------------------------------------------------------

    private void ShowCard()
    {
        string? path = MusicArt.BackdropPath ?? MusicArt.PlainBackdrop();
        if (path == null) return;
        if (path == _shownPath && _showing == Showing.Card) return;

        // Leave _shownPath alone on failure so the next tick tries again rather
        // than believing this worked.
        if (!_display.ShowStill(path))
        {
            Storage.Log($"music: could not put the card on the panel ({path})");
            return;
        }

        _shownPath = path;
        _showing = Showing.Card;
        UseCardOverlay(true);
    }

    private void LeaveVideo()
    {
        _streamingTrack = -1;
        _streamingFile = null;
        _showing = Showing.Nothing;
        _shownPath = null;
    }

    // -----------------------------------------------------------------------
    // The video
    // -----------------------------------------------------------------------

    /// <summary>
    /// Finds the video for a track and gets it onto disk, off the UI thread.
    ///
    /// Two steps, both cached: which video it is, then the video itself. A track
    /// heard before skips both and is playing within a tick; a new one costs a
    /// search and a download, during which the card is already up.
    /// </summary>
    private void BeginPrepare(NativeMethods.JlMediaStatus m)
    {
        if (_resolvingTrack == m.TrackVersion) return;   // already working on it
        if (string.IsNullOrWhiteSpace(m.Title)) return;  // nothing to search for

        // Give the length a moment to arrive before searching without it. It
        // decides whether a match can be trusted at all, and a search run one
        // second too early is the difference between the song and a promo clip.
        if (_seenTrack != m.TrackVersion)
        {
            _seenTrack = m.TrackVersion;
            _trackSeenAt = DateTime.UtcNow;
        }

        if (m.Duration <= 0 && DateTime.UtcNow - _trackSeenAt < DurationGrace) return;

        CancelResolve();

        _resolvingTrack = m.TrackVersion;
        Detail = "Looking for a video…";

        var cts = new CancellationTokenSource();
        _resolve = cts;

        string title = m.Title;
        string artist = m.Artist;
        double duration = m.Duration;
        int version = m.TrackVersion;
        CancellationToken token = cts.Token;

        Task.Run<(string? File, double Seconds)>(() =>
        {
            VideoMatch? match = TrackResolver.Resolve(title, artist, duration, token);
            if (match == null) return (null, 0);

            token.ThrowIfCancellationRequested();

            // Already downloaded: nothing to report, it is about to play.
            if (MusicVideoCache.Existing(match.Id) is { } have) return (have, match.Seconds);

            _dispatcher.BeginInvoke(() =>
            {
                if (_resolvingTrack == version) Detail = "Fetching the video…";
            });

            string? file = MusicVideoCache.Ensure(match.Id, pct =>
            {
                _dispatcher.BeginInvoke(() =>
                {
                    if (_resolvingTrack == version) Detail = $"Fetching the video… {pct:0}%";
                });
            }, token);

            return (file, match.Seconds);
        }, token)
            .ContinueWith(t =>
            {
                if (t.IsCanceled || token.IsCancellationRequested) return;

                (string? file, double seconds) = t.IsFaulted ? (null, 0d) : t.Result;
                if (t.IsFaulted)
                    Storage.Log($"music: preparing \"{title}\" failed: {t.Exception?.GetBaseException().Message}");

                _dispatcher.BeginInvoke(() => OnPrepared(version, file, seconds, title, artist));
            }, TaskScheduler.Default);
    }

    private void OnPrepared(int version, string? file, double videoSeconds,
                            string title, string artist)
    {
        if (!_running) return;

        // The song moved on while we were working. Its answer is not wanted,
        // and the tick that noticed the change has already started again.
        NativeMethods.JlMediaStatus m = MediaSession.Read();
        if (m.Present == 0 || m.TrackVersion != version) return;

        if (file == null)
        {
            // Normal, not a failure: most tracks on a music service have no
            // video. The card is already up and stays up.
            Detail = "No video — showing the card";
            return;
        }

        if (m.Playing == 0)
        {
            // Paused while we worked. The next tick shows the card; both the
            // search and the file are cached, so resuming costs nothing.
            Detail = "Paused";
            return;
        }

        // The length check the search could not make. A video that runs for a
        // different time than the track is not that recording, and playing it
        // would seek past its end and "finish" seconds later.
        if (videoSeconds > 0)
        {
            // With a track length, the video has to match it. Without one, it
            // only has to be long enough to be a song at all — which is what
            // keeps clips and Shorts off the panel.
            bool wrong = m.Duration > 0
                ? Math.Abs(videoSeconds - m.Duration) > LengthMatchToleranceSeconds
                : videoSeconds < MinPlausibleVideoSeconds;

            if (wrong)
            {
                string against = m.Duration > 0
                    ? $"the track's {m.Duration:0}s"
                    : "a plausible song";

                Detail = m.Duration > 0
                    ? $"Video is {videoSeconds:0}s, track is {m.Duration:0}s — showing the card"
                    : $"Video is only {videoSeconds:0}s — showing the card";

                Storage.Log($"music: rejected the video for \"{title}\" - it runs " +
                            $"{videoSeconds:0}s against {against}");

                // Forgotten so a later play, when the length is known up front,
                // can search again and do better.
                TrackResolver.Forget(title, artist, 0);

                // And dropped, so a bad match does not sit in the cache taking
                // up room for a video that will never be played.
                try { if (File.Exists(file)) File.Delete(file); } catch { }
                return;
            }
        }

        // Straight to where the song has reached. This is only affordable
        // because the video is a local file with an index — the same seek
        // against a YouTube stream cost 106 seconds, which is what the download
        // exists to avoid.
        double start = m.Position + StartLeadSeconds;

        // Never past the end, whatever the numbers say.
        if (videoSeconds > 0 && start > videoSeconds - 5) start = 0;

        if (!_display.PlayStream(file, "", start, m.Duration > 0 ? m.Duration : videoSeconds))
        {
            Detail = "Could not start the video";
            return;
        }

        _streamingTrack = version;
        _streamingFile = file;
        _showing = Showing.Video;
        _shownPath = null;
        _lastCorrection = DateTime.UtcNow;
        _streamStartedAt = DateTime.UtcNow;
        _lastFramesSent = -1;
        _driftStreak = 0;

        // The video now starts where the song is, so the next tick measures
        // from here rather than from whatever the previous track was doing.
        _lastTrackPos = -1;
        _lastTrackPosAt = DateTime.UtcNow;
        _zeroJumpVersion = -1;
        _zeroJumpAt = DateTime.MinValue;

        // The card's scrim covers the whole panel, so leaving it up would draw
        // a video through a dark sheet. Video gets whatever overlay the user
        // had — their statistics, or nothing.
        UseCardOverlay(false);
        Detail = $"Video {Path.GetFileNameWithoutExtension(file)}";
    }

    /// <summary>
    /// Nudges the panel back into step with the browser.
    ///
    /// This is open loop and always will be: two independent streams of the same
    /// song, one of which cannot be observed. The browser's own buffering is a
    /// standing offset of some hundreds of milliseconds that nothing here can
    /// measure, so the aim is "close", not "in sync".
    /// </summary>
    private void CorrectDrift(NativeMethods.JlMediaStatus m, long frames)
    {
        // Where our own clock said the song should be by now, against where the
        // player says it is. Tracked before anything below can return, so the
        // reference never goes stale and invents a jump later.
        DateTime now = DateTime.UtcNow;
        double expected = _lastTrackPos + (now - _lastTrackPosAt).TotalSeconds;
        double jump = _lastTrackPos < 0 ? 0 : Math.Abs(m.Position - expected);

        _lastTrackPos = m.Position;
        _lastTrackPosAt = now;

        // Somebody moved the song. Followed immediately: none of the caution
        // below applies here, because there is nothing to confirm — the player
        // has said outright that it is somewhere else. Waiting on a seek is
        // what made dragging the browser's scrubber do nothing to the panel.
        if (jump > SeekJumpSeconds)
        {
            if (m.Position <= TrackTurnoverSeconds)
            {
                // Held, not judged. If the song really did change, Tick stops
                // coming here at all — the track-change path takes over the
                // moment the title lands — so a hold that survives its grace
                // is a seek by elimination.
                if (_zeroJumpVersion != m.TrackVersion)
                {
                    _zeroJumpVersion = m.TrackVersion;
                    _zeroJumpAt = now;
                }
            }
            else
            {
                _zeroJumpVersion = -1;
                _driftStreak = 0;
                _lastCorrection = now;
                _display.Seek(m.Position + StartLeadSeconds);
                Storage.Log($"music: followed a seek to {m.Position:0.0}s " +
                            $"(video was at {_display.PositionSeconds:0.0}s)");
                return;
            }
        }

        // A held jump that no track change ever arrived to explain. It was a
        // scrub towards the beginning, and the song has been playing on from
        // there while we waited, so follow it to where it is now.
        if (_zeroJumpVersion == m.TrackVersion && _zeroJumpAt != DateTime.MinValue
            && now - _zeroJumpAt > ZeroJumpGrace)
        {
            _zeroJumpVersion = -1;
            _zeroJumpAt = DateTime.MinValue;
            _driftStreak = 0;
            _lastCorrection = now;
            _display.Seek(m.Position + StartLeadSeconds);
            Storage.Log($"music: followed a seek towards the start ({m.Position:0.0}s, " +
                        $"video was at {_display.PositionSeconds:0.0}s)");
            return;
        }

        // Only while frames are actually flowing. A stalled stream's position
        // stands still while the song runs on, so the apparent drift grows
        // without bound — and seeking to "fix" it restarts the very decoder
        // that is already struggling. Corrections of twenty seconds, then
        // forty, twenty seconds apart, are what that looks like in the log.
        bool moving = _lastFramesSent >= 0 && frames > _lastFramesSent;
        _lastFramesSent = frames;
        if (!moving) return;

        // Everything below compares the song against the video's own position,
        // which is only meaningful once the item has a known length. Following
        // a seek does not need one, which is why that happens above this line:
        // gating the whole method on it turned seeking off completely for any
        // track whose length the browser never reported.
        if (_display.DurationSeconds <= 0) return;

        double drift = m.Position - _display.PositionSeconds;

        // A gap has to hold still before it is acted on — see
        // DriftTicksBeforeActing. A track change resolves itself inside this
        // window, so the correction never happens.
        if (Math.Abs(drift) < DriftToleranceSeconds)
        {
            _driftStreak = 0;
            return;
        }

        // A video ahead of the song can only have got there by the song going
        // backwards, and the song going backwards while the title stays put is
        // a track change whose title has not landed yet. Correcting that seeks
        // the outgoing video to the top, moments before it is replaced anyway.
        //
        // A deliberate scrub backwards is the one thing this gives up on, and
        // only while the track is still near its start. It costs a video that
        // runs ahead until the next track, which is the cheaper mistake.
        if (drift < 0 && m.Position <= TrackStartSeconds)
        {
            _driftStreak = 0;
            return;
        }

        if (++_driftStreak < DriftTicksBeforeActing) return;
        if (DateTime.UtcNow - _lastCorrection < CorrectionCooldown) return;

        _driftStreak = 0;
        _lastCorrection = DateTime.UtcNow;
        _display.Seek(m.Position + StartLeadSeconds);

        // Logged because it is the only evidence available when somebody says it
        // drifts: nothing else records that a correction happened at all.
        // Both positions, not just the difference: a gap can mean the panel is
        // lagging, or that the track jumped, and the raw numbers say which.
        Storage.Log($"music: seeking video from {_display.PositionSeconds:0.0}s " +
                    $"to {m.Position:0.0}s ({drift:+0.0;-0.0}s out)");
    }

    /// <summary>
    /// Says a video produced nothing, in the one place anybody will look.
    ///
    /// Now that the file is fetched before it is played, this means ffmpeg
    /// could not decode what was downloaded — a truncated fetch, or a codec it
    /// has no decoder for. The file is dropped so the next play fetches it
    /// again rather than failing the same way forever.
    /// </summary>
    private void ReportDeadVideo(string? file)
    {
        Detail = "Video would not play";

        // Plain ASCII, like every other line in this log: the file is written
        // as UTF-8 and read by whatever the user opens it with.
        Storage.Log($"music: the video produced no frames ({file ?? "unknown file"}); " +
                    "discarding it so the next play fetches it again");

        if (file == null) return;
        try { if (File.Exists(file)) File.Delete(file); } catch { }
    }

    private void CancelResolve()
    {
        _resolvingTrack = -1;

        CancellationTokenSource? cts = _resolve;
        _resolve = null;
        if (cts == null) return;

        try { cts.Cancel(); } catch { }
        cts.Dispose();
    }

    private static string Describe(NativeMethods.JlMediaStatus m)
    {
        if (m.Present == 0) return "Nothing playing";

        string title = string.IsNullOrWhiteSpace(m.Title) ? "Unknown track" : m.Title;
        string line = string.IsNullOrWhiteSpace(m.Artist) ? title : $"{title} — {m.Artist}";

        return m.Playing != 0 ? line : $"{line} (paused)";
    }

    // -----------------------------------------------------------------------
    // The card is an overlay profile, so following swaps the active one
    // -----------------------------------------------------------------------

    private void RememberOverlay()
    {
        if (_remembered) return;

        _previousProfile = _overlays.ActiveProfileId;
        _previousEnabled = _overlay.Enabled;
        _remembered = true;
    }

    /// <summary>
    /// Swaps between the card's profile and the user's own.
    ///
    /// The card is mostly overlay — a blurred still with everything legible
    /// drawn on top — so showing it means forcing compositing on and making
    /// "Now playing" active. A video is the opposite: the card's scrim covers
    /// the whole panel, so it has to go, and what the user had comes back.
    /// </summary>
    private void UseCardOverlay(bool on)
    {
        if (_cardOverlay == on) return;
        _cardOverlay = on;

        // Logged because the card is mostly overlay: if this does not happen,
        // the panel shows a blurred backdrop and nothing else, and there is
        // otherwise no way to tell that from the card working correctly.
        Storage.Log($"music: overlay -> {(on ? "Now playing card" : "the user's own")}");

        if (on)
        {
            OverlayProfile? card =
                _overlays.Profiles.Find(p => p.Id == OverlaySettings.NowPlayingId);

            if (card == null)
            {
                // Added to the list rather than kept private, so it shows up in
                // the tray menu and the editor: the card is meant to be
                // restyled, and a profile nobody can find cannot be.
                card = OverlaySettings.CreateNowPlaying();
                _overlays.Profiles.Add(card);
                Storage.SaveOverlays(_overlays);
            }
            else if (OverlaySettings.UpgradeNowPlaying(card) > 0)
            {
                // A card saved by an earlier version, given whatever the
                // shipped one has gained since. Only additions, so anything
                // moved or restyled stays as the user left it.
                Storage.SaveOverlays(_overlays);
            }

            _overlays.ActiveProfileId = card.Id;
            _overlay.Refresh(card);
            if (!_overlay.Enabled) _overlay.SetEnabled(true);
            return;
        }

        _overlays.ActiveProfileId = _previousProfile;
        _overlay.Refresh(_overlays.Active());
        if (_overlay.Enabled != _previousEnabled) _overlay.SetEnabled(_previousEnabled);
    }

    private void RestoreOverlay()
    {
        if (!_remembered) return;
        _remembered = false;

        UseCardOverlay(false);
        _cardOverlay = false;
    }

    public void Dispose() => Stop();

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name!));
    }
}
