using System;
using System.Collections.Generic;
using System.Globalization;

using JLDisplayManager.Interop;
using JLDisplayManager.Services.Music;

namespace JLDisplayManager.Services.Sensors;

/// <summary>
/// What is playing elsewhere on this machine — a browser tab, a music player —
/// as sensors, so an overlay can put the track on the panel with no new
/// machinery: <c>{music.title}</c> works exactly like <c>{time.now}</c>.
///
/// Nothing here plays audio. The panel has no speaker; this reads the Windows
/// media session and reports it.
///
/// The reading itself is done natively on its own thread, because the calls
/// behind it are cross-process into the player and would be the wrong thing to
/// do on the sensor pool. <see cref="Poll"/> only copies out a struct.
/// </summary>
public sealed class MediaSessionProvider : ISensorProvider
{
    private readonly string? _preferredApp;
    private bool _acquired;

    public MediaSessionProvider(string? preferredApp)
    {
        _preferredApp = preferredApp;
    }

    public string Name => "Media session";

    /// <summary>
    /// True once the reader has a session manager. False on a machine too old
    /// for the API, and — briefly — before the first tick.
    /// </summary>
    public bool Available => _acquired && MediaSession.Read().Available != 0;

    public bool Start()
    {
        MediaSession.Acquire(_preferredApp);
        _acquired = true;
        return true;
    }

    /// <summary>
    /// The ids here are permanent: they end up written into saved overlay
    /// profiles, so renaming one breaks somebody's layout.
    ///
    /// Positions come in two forms deliberately. The numbers drive bars and
    /// visibility rules; the text is what a card actually shows, formatted here
    /// so that <c>{music.elapsed}</c> needs no format string to read "1:23".
    /// </summary>
    public IReadOnlyList<SensorDescriptor> Describe() => new List<SensorDescriptor>
    {
        new("music.title",     "Track title",   "Music", "", IsText: true),
        new("music.artist",    "Artist",        "Music", "", IsText: true),
        new("music.album",     "Album",         "Music", "", IsText: true),
        new("music.source",    "Player",        "Music", "", IsText: true),
        new("music.state",     "Playback state","Music", "", IsText: true),

        new("music.art",       "Album art file","Music", "", IsText: true),

        new("music.elapsed",   "Elapsed",       "Music", "", IsText: true),
        new("music.length",    "Track length",  "Music", "", IsText: true),
        new("music.remaining", "Remaining",     "Music", "", IsText: true),

        new("music.position",  "Elapsed",       "Music", "s", 0, 600),
        new("music.duration",  "Track length",  "Music", "s", 0, 600),
        new("music.progress",  "Track progress","Music", "%", 0, 100),
        new("music.playing",   "Something playing", "Music", "", 0, 1),

        // The one reading that is always available, and the inverse of the
        // rest: while it is 1 every other music sensor is unavailable, so a
        // layer bound to "not idle" is how a card hides itself rather than
        // drawing a row of "--".
        new("music.idle",      "Nothing playing",   "Music", "", 0, 1),

        // The video being fetched for the panel. Always available, so a layer
        // can be bound to "only while downloading" and stay hidden the rest of
        // the time rather than drawing an empty bar.
        new("music.fetching",  "Downloading a video", "Music", "", 0, 1),
        new("music.fetch",     "Download progress",   "Music", "%", 0, 100),
    };

    public void Poll(ISensorSink sink)
    {
        NativeMethods.JlMediaStatus m = MediaSession.Read();

        // Album art is fetched, scaled and cached off this thread; all that
        // happens here is noticing that the version moved.
        MusicArt.Update(m.ThumbnailVersion, m.Present != 0);

        // Before the nothing-playing branch below: a download can still be
        // running when the music has been paused, and the bar should stay
        // truthful about it.
        bool fetching = MusicVideoCache.Fetching;
        sink.Publish("music.fetching", fetching ? 1 : 0);
        sink.Publish("music.fetch", fetching ? MusicVideoCache.Progress : 0);

        // Nothing playing publishes exactly one thing: the flag a layer's
        // visibility rule tests. Everything else stays unavailable, which is
        // what renders "--" rather than a stale title from an hour ago.
        if (m.Present == 0)
        {
            sink.Publish("music.idle", 1);
            sink.PublishText("music.state", "Nothing playing");
            return;
        }

        sink.Publish("music.idle", 0);
        sink.Publish("music.playing", m.Playing != 0 ? 1 : 0);
        sink.PublishText("music.state", m.Playing != 0 ? "Playing" : "Paused");

        if (!string.IsNullOrEmpty(m.Title)) sink.PublishText("music.title", m.Title);
        if (!string.IsNullOrEmpty(m.Artist)) sink.PublishText("music.artist", m.Artist);
        if (!string.IsNullOrEmpty(m.Album)) sink.PublishText("music.album", m.Album);

        string source = MediaSession.FriendlyName(m.SourceApp);
        if (!string.IsNullOrEmpty(source)) sink.PublishText("music.source", source);

        // A path rather than a picture: an ImageLayer's file is a template, so
        // "{music.art}" in one is how the current track's cover reaches the
        // panel. Unavailable when the track has no art, which draws nothing
        // rather than the previous track's cover.
        string? art = MusicArt.ArtPath;
        if (!string.IsNullOrEmpty(art)) sink.PublishText("music.art", art);

        sink.Publish("music.position", m.Position);
        sink.PublishText("music.elapsed", Clock(m.Position));

        // A player that does not report a length leaves the length, the
        // remaining time and the progress bar unavailable rather than showing
        // a bar that is always empty or a countdown from zero.
        if (m.Duration > 0)
        {
            sink.Publish("music.duration", m.Duration);
            sink.PublishText("music.length", Clock(m.Duration));
            sink.PublishText("music.remaining", "-" + Clock(Math.Max(0, m.Duration - m.Position)));
            sink.Publish("music.progress", Math.Clamp(m.Position / m.Duration * 100.0, 0, 100));
        }
    }

    /// <summary>
    /// m:ss, or h:mm:ss for anything over an hour — a track never is, but a
    /// podcast or a set in the same session might be.
    /// </summary>
    private static string Clock(double seconds)
    {
        if (double.IsNaN(seconds) || seconds < 0) seconds = 0;

        TimeSpan t = TimeSpan.FromSeconds(seconds);
        return t.TotalHours >= 1
            ? string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00}",
                (int)t.TotalHours, t.Minutes, t.Seconds)
            : string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}", t.Minutes, t.Seconds);
    }

    public void Dispose()
    {
        if (!_acquired) return;
        _acquired = false;
        MediaSession.Release();
    }
}
