using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;

using JLDisplayManager.Models;

namespace JLDisplayManager.Services.Music;

/// <summary>
/// Works out which YouTube video is the thing the browser is playing.
///
/// It answers with an id and stops there. Turning that into something the panel
/// can show is <see cref="MusicVideoCache"/>'s job, and it does it by fetching
/// the file rather than resolving a stream URL — see the note there for why a
/// stream turned out to be unplayable.
///
/// This is the part of the feature that cannot be made exact. The Windows media
/// session reports a title, an artist and a length — never a URL — so the video
/// has to be found by searching for it, and a search can be wrong.
///
/// The length is what makes it usable. Searching "artist title" on YouTube
/// returns the song, a lyric video, three live cuts and an hour-long loop, and
/// only one of them runs for the same number of seconds as the thing playing.
/// Without that check this would pick a plausible wrong video often enough to
/// be worse than showing nothing.
/// </summary>
/// <summary>A matched video: its id, and how long YouTube says it runs.</summary>
public sealed record VideoMatch(string Id, double Seconds);

public static class TrackResolver
{
    /// <summary>
    /// How far a candidate's length may be from the session's and still count.
    /// YouTube reports whole seconds and the session reports fractions, so this
    /// cannot be zero; much wider and a radio edit matches an album version.
    /// </summary>
    private const double DurationToleranceSeconds = 3.0;

    private static readonly object Lock = new();
    private static Dictionary<string, CacheEntry>? _cache;

    private sealed class CacheEntry
    {
        /// <summary>The video, or null when the search found nothing plausible.</summary>
        public string? VideoId { get; set; }

        /// <summary>
        /// Its length, kept because the check that matters most often cannot be
        /// made at search time: this player frequently reports no track length
        /// until a few seconds in, and by then the search has already run on a
        /// title alone. Holding the candidate's length lets the comparison
        /// happen later, when the track's own length has arrived.
        /// </summary>
        public double Seconds { get; set; }

        public DateTime WhenUtc { get; set; }
    }

    private static string CachePath => Path.Combine(Storage.Directory, "music-cache.json");

    /// <summary>
    /// How long a "there is no video for this" answer is trusted before the
    /// search is tried again. A miss is usually permanent — the track simply has
    /// no video — and re-searching every play would mean a yt-dlp launch per
    /// track forever.
    /// </summary>
    private static readonly TimeSpan MissLifetime = TimeSpan.FromDays(30);

    /// <summary>
    /// The YouTube id of the video for a track, or null when there is no
    /// confident match — which is a normal outcome and means "show the card".
    ///
    /// Blocking: it launches yt-dlp. Call it off the UI thread.
    /// </summary>
    public static VideoMatch? Resolve(string title, string artist, double duration,
                                      CancellationToken token = default)
    {
        if (string.IsNullOrWhiteSpace(title)) return null;

        YtDlp? ytdlp = YtDlp.Find();
        if (ytdlp is null) return null;

        string key = Key(title, artist, duration);
        if (Lookup(key) is { } hit) return hit;

        if (IsKnownMiss(key)) return null;

        string? id = Search(ytdlp, title, artist, duration, out double seconds,
            out bool searched, token);

        // Only a search that actually ran is worth remembering. A blocked
        // firewall, a dead link or a yt-dlp that fell over is not evidence that
        // the track has no video, and caching it as one would hide the video
        // for a month after a moment's network trouble.
        if (searched) Remember(key, id, seconds);

        return id == null ? null : new VideoMatch(id, seconds);
    }

    // -----------------------------------------------------------------------

    /// <summary>
    /// Searches YouTube and picks the candidate whose length matches.
    ///
    /// <c>--flat-playlist</c> is what makes this affordable: the search page
    /// carries an id, a title and a length for every hit, so one request answers
    /// the question. Without it yt-dlp extracts all five videos in full, which
    /// is five more round trips for information already on the page.
    /// </summary>
    /// <param name="searched">
    /// True when yt-dlp ran and gave an answer — so a null result means "no such
    /// video" rather than "could not look". Only the former is worth caching.
    /// </param>
    private static string? Search(YtDlp ytdlp, string title, string artist, double duration,
                                  out double chosenSeconds, out bool searched,
                                  CancellationToken token)
    {
        searched = false;
        chosenSeconds = 0;
        string query = string.IsNullOrWhiteSpace(artist) ? title : $"{artist} {title}";

        var lines = new List<string>();
        string[] args =
        {
            "ytsearch5:" + query,
            "--flat-playlist",
            "--print", "%(id)s|%(duration)s|%(title)s|%(channel)s",
            "--no-warnings",
            "--skip-download",
        };

        try
        {
            int rc = ytdlp.Run(args, line => { lock (lines) lines.Add(line); }, token);
            if (rc != 0 && lines.Count == 0)
            {
                Storage.Log($"music: search for \"{query}\" failed (yt-dlp exit {rc})");
                return null;
            }
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            Storage.Log($"music: search failed for \"{query}\": {ex.Message}");
            return null;
        }

        searched = true;

        string? best = null;
        double bestSeconds = 0;
        double bestGap = double.MaxValue;
        bool bestNamed = false;

        foreach (string line in lines)
        {
            string[] parts = line.Split('|');
            if (parts.Length < 3) continue;               // a log line, not a result

            string id = parts[0].Trim();
            if (id.Length != 11) continue;                // ditto; ids are fixed width

            if (!double.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture,
                    out double seconds))
                continue;                                 // "NA": no length, no confidence

            string candidate = parts[2];
            string channel = parts.Length > 3 ? parts[3] : "";

            // Named by the artist somewhere. Weaker than the length, but it is
            // what separates the official upload from somebody's re-upload of
            // exactly the same track.
            bool named = !string.IsNullOrWhiteSpace(artist)
                && (Contains(candidate, artist) || Contains(channel, artist));

            if (duration > 0)
            {
                double gap = Math.Abs(seconds - duration);
                if (gap > DurationToleranceSeconds) continue;

                // A named match beats an unnamed one even when its length is
                // slightly further off; between two of the same kind, closer wins.
                if (best == null || (named && !bestNamed) || (named == bestNamed && gap < bestGap))
                {
                    best = id;
                    bestSeconds = seconds;
                    bestGap = gap;
                    bestNamed = named;
                }
                continue;
            }

            // No length to check against. YouTube Music in a browser frequently
            // reports no end time at all, so this is not a rare path: only a
            // title match will do, and among those the artist's own upload
            // beats a lyric video or somebody's re-upload.
            if (!Contains(candidate, title)) continue;
            if (best == null || (named && !bestNamed))
            {
                best = id;
                bestSeconds = seconds;
                bestNamed = named;
            }
        }

        if (best == null)
            Storage.Log($"music: no video matched \"{query}\" at {duration:0.#}s");

        chosenSeconds = bestSeconds;
        return best;
    }

    // -----------------------------------------------------------------------
    // The cache: video ids, never URLs
    // -----------------------------------------------------------------------

    /// <summary>
    /// Keyed on what the session reports, rounded — the position moves but the
    /// length does not, and a track re-reported a second later must hit the same
    /// entry.
    /// </summary>
    private static string Key(string title, string artist, double duration) =>
        $"{Normalise(title)}|{Normalise(artist)}|{Math.Round(duration)}";

    private static VideoMatch? Lookup(string key)
    {
        lock (Lock)
        {
            Load();
            if (!_cache!.TryGetValue(key, out CacheEntry? e)) return null;
            return e.VideoId == null ? null : new VideoMatch(e.VideoId, e.Seconds);
        }
    }

    private static bool IsKnownMiss(string key)
    {
        lock (Lock)
        {
            Load();
            if (!_cache!.TryGetValue(key, out CacheEntry? e)) return false;
            if (e.VideoId != null) return false;

            return DateTime.UtcNow - e.WhenUtc < MissLifetime;
        }
    }

    private static void Remember(string key, string? id, double seconds)
    {
        lock (Lock)
        {
            Load();
            _cache![key] = new CacheEntry
            {
                VideoId = id,
                Seconds = seconds,
                WhenUtc = DateTime.UtcNow,
            };
            Save();
        }
    }

    /// <summary>Forgets one track, so the next play searches again.</summary>
    public static void Forget(string title, string artist, double duration)
    {
        lock (Lock)
        {
            Load();
            if (_cache!.Remove(Key(title, artist, duration))) Save();
        }
    }

    private static void Load()
    {
        if (_cache != null) return;

        try
        {
            if (File.Exists(CachePath))
            {
                _cache = JsonSerializer.Deserialize<Dictionary<string, CacheEntry>>(
                    File.ReadAllText(CachePath));
            }
        }
        catch (Exception ex)
        {
            Storage.Log($"music: could not read the resolve cache: {ex.Message}");
        }

        _cache ??= new Dictionary<string, CacheEntry>();
    }

    private static void Save()
    {
        try
        {
            Directory.CreateDirectory(Storage.Directory);
            File.WriteAllText(CachePath,
                JsonSerializer.Serialize(_cache, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception ex)
        {
            Storage.Log($"music: could not write the resolve cache: {ex.Message}");
        }
    }

    // -----------------------------------------------------------------------

    /// <summary>
    /// Loose containment: case, spacing and punctuation all differ between what
    /// a player reports and what an uploader typed, and none of those
    /// differences mean the track is a different one.
    /// </summary>
    private static bool Contains(string haystack, string needle)
    {
        string h = Normalise(haystack), n = Normalise(needle);
        return n.Length > 0 && h.Contains(n, StringComparison.Ordinal);
    }

    private static string Normalise(string? value)
    {
        if (string.IsNullOrEmpty(value)) return "";

        var sb = new StringBuilder(value.Length);
        foreach (char c in value.ToLowerInvariant())
            if (char.IsLetterOrDigit(c)) sb.Append(c);

        return sb.ToString();
    }
}
