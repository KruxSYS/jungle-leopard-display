using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;

using JLDisplayManager.Models;

namespace JLDisplayManager.Services.Music;

/// <summary>
/// Fetches a music video to disk once and keeps it, so the panel plays a file
/// rather than a stream.
///
/// This is not an optimisation, it is what makes the feature work. YouTube
/// serves fragmented MP4 at roughly playback rate, and ffmpeg can only seek one
/// by reading the whole thing to index it: measured on a 3½-minute track, every
/// <c>-ss</c> cost <b>106 seconds</b> to the first frame — the same whether the
/// target was 30 seconds in or 120 — against 0.4 seconds with no seek. A stream
/// therefore could only ever be played from its beginning, which meant a video
/// could not be joined late and could never be nudged back into step.
///
/// A downloaded file has a <c>moov</c> index, so seeking is instant and both of
/// those come back. It also fixes the quality question: the 720p rung is
/// fetched whole, at whatever rate the link allows, instead of being decoded
/// live from a throttled connection.
///
/// yt-dlp does the fetching, which matters on a locked-down machine — it is the
/// one component here that is already allowed through firewalls, because the
/// library's Download… button has always needed it.
/// </summary>
public static class MusicVideoCache
{
    /// <summary>
    /// Video only, 720p or under, H.264 preferred.
    ///
    /// No audio track at all: something else is already making the sound, and
    /// muxing one in would only cost bytes and a merge step. 720p because the
    /// panel is 960x480 — anything larger is downloaded to be thrown away in the
    /// downscale. H.264 first because every GPU decoder handles it, where the
    /// AV1 rungs YouTube now prefers would land on a software decoder.
    /// </summary>
    private const string FormatSelector =
        "bv*[height<=?720][vcodec^=avc1]/bv*[height<=?720]/b[height<=?720]";

    // How much the cache may hold and how long a video is kept, set from
    // AppSettings before the first sweep. Defaults match the settings' own, so
    // a sweep that somehow runs first still behaves sensibly.
    private static long _budgetBytes = 4L * 1024 * 1024 * 1024;
    private static int _maxAgeDays = 14;

    /// <summary>
    /// Applies the user's cache limits. Cheap; call it whenever they change.
    /// </summary>
    public static void Configure(int budgetMB, int maxAgeDays)
    {
        _budgetBytes = Math.Max(1, budgetMB) * 1024L * 1024L;
        _maxAgeDays = Math.Max(0, maxAgeDays);
    }

    private static readonly object Lock = new();

    /// <summary>
    /// Ids currently being fetched, so two ticks cannot start the same download
    /// twice. Keyed by id rather than guarded by one lock, because two different
    /// videos may legitimately download at once.
    /// </summary>
    private static readonly HashSet<string> InFlight = new(StringComparer.Ordinal);

    // Whether a download is running and how far along it is, as plain ints so
    // they can be read from the sensor poll without a lock. Published as
    // sensors, which is what lets the card draw a progress bar for something
    // happening on a completely different thread.
    private static int _fetching;
    private static int _fetchPercent;

    /// <summary>True while a video is being downloaded.</summary>
    public static bool Fetching => Volatile.Read(ref _fetching) != 0;

    /// <summary>How far the current download has got, 0-100.</summary>
    public static double Progress => Volatile.Read(ref _fetchPercent);

    /// <summary>The file for a video id, whether or not it exists yet.</summary>
    /// <summary>Whether this is the shape of a YouTube video id: 11 of [A-Za-z0-9_-].</summary>
    public static bool IsVideoId(string? id) =>
        id is { Length: 11 } && id.All(c => char.IsAsciiLetterOrDigit(c) || c == '_' || c == '-');

    public static string PathFor(string videoId) =>
        Path.Combine(Storage.MusicVideoDirectory, videoId + ".mp4");

    /// <summary>The cached file, or null when it has not been fetched.</summary>
    public static string? Existing(string videoId)
    {
        string path = PathFor(videoId);
        if (!File.Exists(path)) return null;

        // Touched so the sweep measures when it was last wanted, not when it
        // was first fetched.
        try { File.SetLastAccessTimeUtc(path, DateTime.UtcNow); } catch { }
        return path;
    }

    /// <summary>
    /// Returns the cached file, downloading it first if need be.
    ///
    /// Blocking — it runs yt-dlp — so call it off the UI thread. Returns null
    /// when the download fails, which is an ordinary outcome: the card is what
    /// happens then.
    /// </summary>
    public static string? Ensure(string videoId, Action<double>? onProgress = null,
                                 CancellationToken token = default)
    {
        // The id names a file (PathFor) and is handed to yt-dlp, and it comes
        // from parsing yt-dlp's own search output. Anything that is not the
        // shape of a YouTube id is refused rather than trusted with either.
        if (!IsVideoId(videoId)) return null;

        if (Existing(videoId) is { } cached) return cached;

        YtDlp? ytdlp = YtDlp.Find();
        if (ytdlp is null) return null;

        lock (Lock)
        {
            if (!InFlight.Add(videoId)) return null;   // another tick has it
        }

        try
        {
            return Download(ytdlp, videoId, onProgress, token);
        }
        finally
        {
            lock (Lock) InFlight.Remove(videoId);
        }
    }

    private static string? Download(YtDlp ytdlp, string videoId, Action<double>? onProgress,
                                    CancellationToken token)
    {
        Storage.EnsureDirectories();

        string path = PathFor(videoId);

        var args = new List<string>
        {
            "-f", FormatSelector,

            // A fixed name rather than a title template: the id is the cache
            // key, and a title would make the same video land under two names
            // when YouTube renames it.
            "-o", path,

            "--no-playlist",
            "--no-mtime",
            "--no-warnings",
            "--newline",
            "--no-color",

            // The id last, after "--". YouTube ids may begin with '-' (about
            // one in 64 do), and as a leading argument yt-dlp parsed those as
            // an option and failed, so that song's video could never be
            // fetched. "--" ends option parsing; everything after it is a URL.
            "--",
            videoId,
        };

        var progress = new System.Text.RegularExpressions.Regex(@"\[download\]\s+([\d.]+)%");
        string lastError = "";

        Volatile.Write(ref _fetchPercent, 0);
        Volatile.Write(ref _fetching, 1);

        try
        {
            int rc = ytdlp.Run(args.ToArray(), line =>
            {
                if (line.StartsWith("ERROR", StringComparison.OrdinalIgnoreCase)) lastError = line;

                if (onProgress == null) return;
                var m = progress.Match(line);
                if (m.Success && double.TryParse(m.Groups[1].Value,
                        System.Globalization.NumberStyles.Float,
                        System.Globalization.CultureInfo.InvariantCulture, out double pct))
                {
                    Volatile.Write(ref _fetchPercent, (int)pct);
                    onProgress(pct);
                }
            }, token);

            if (rc != 0 || !File.Exists(path))
            {
                Storage.Log($"music: could not fetch video {videoId}" +
                            (lastError.Length > 0 ? $": {lastError}" : $" (yt-dlp exit {rc})"));
                Discard(path);
                return null;
            }
        }
        catch (OperationCanceledException)
        {
            // A track that changed mid-download. The partial file is no use to
            // anybody and would be mistaken for a complete one next time.
            Discard(path);
            throw;
        }
        catch (Exception ex)
        {
            Storage.Log($"music: could not fetch video {videoId}: {ex.Message}");
            Discard(path);
            return null;
        }
        finally
        {
            // Cleared on every exit, including the cancelled and failed ones —
            // a bar left sitting at 47% would be worse than no bar at all.
            Volatile.Write(ref _fetching, 0);
        }

        Sweep();
        return path;
    }

    private static void Discard(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }

        // yt-dlp writes to a .part file and renames on success, so a cancelled
        // fetch leaves one behind.
        try
        {
            string part = path + ".part";
            if (File.Exists(part)) File.Delete(part);
        }
        catch { }
    }

    /// <summary>Bytes the cache currently holds.</summary>
    public static long Bytes()
    {
        try
        {
            return new DirectoryInfo(Storage.MusicVideoDirectory)
                .GetFiles("*.mp4").Sum(f => f.Length);
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>Empties it. Safe at any time — a file already playing keeps playing.</summary>
    public static void Clear()
    {
        try
        {
            foreach (FileInfo f in new DirectoryInfo(Storage.MusicVideoDirectory).GetFiles("*.mp4"))
            {
                try { f.Delete(); } catch { }
            }
        }
        catch (Exception ex)
        {
            Storage.Log($"music: could not clear the video cache: {ex.Message}");
        }
    }

    /// <summary>
    /// Clears out videos that have aged out or no longer fit, and any wreckage
    /// from downloads that never finished.
    ///
    /// Age is applied before size and matters more: a listening habit moves on,
    /// so a cache trimmed only when it hits its budget ends up full of tracks
    /// nobody has played in months. Size is the backstop for the opposite case,
    /// a lot of listening in a short time.
    ///
    /// "Last played" is <see cref="Existing"/> stamping the access time each
    /// time a file is handed out, so it means what it says rather than being
    /// whatever the filesystem happened to record.
    ///
    /// A file currently on the panel simply refuses to delete, which is the
    /// behaviour wanted rather than a case to detect.
    /// </summary>
    public static void Sweep()
    {
        try
        {
            var dir = new DirectoryInfo(Storage.MusicVideoDirectory);
            if (!dir.Exists) return;

            int aged = 0, trimmed = 0, orphans = 0;

            // Downloads that were cancelled or died leave a .part behind. They
            // are never reused — a fetch always starts afresh — so any that
            // survived a restart is dead weight.
            foreach (FileInfo part in dir.GetFiles("*.part"))
            {
                if (DateTime.UtcNow - part.LastWriteTimeUtc < TimeSpan.FromHours(1)) continue;
                try { part.Delete(); orphans++; } catch { }
            }

            var files = new List<FileInfo>(dir.GetFiles("*.mp4"));

            if (_maxAgeDays > 0)
            {
                DateTime cutoff = DateTime.UtcNow - TimeSpan.FromDays(_maxAgeDays);
                for (int i = files.Count - 1; i >= 0; i--)
                {
                    if (files[i].LastAccessTimeUtc >= cutoff) continue;
                    try { files[i].Delete(); files.RemoveAt(i); aged++; } catch { }
                }
            }

            long total = files.Sum(f => f.Length);
            if (total > _budgetBytes)
            {
                foreach (FileInfo f in files.OrderBy(f => f.LastAccessTimeUtc))
                {
                    long size = f.Length;
                    try { f.Delete(); } catch { continue; }

                    trimmed++;
                    total -= size;
                    if (total <= _budgetBytes) break;
                }
            }

            if (aged + trimmed + orphans > 0)
            {
                Storage.Log($"music: swept the video cache - {aged} aged out, " +
                            $"{trimmed} over budget, {orphans} unfinished; " +
                            $"{total / (1024 * 1024)} MB left");
            }
        }
        catch (Exception ex)
        {
            Storage.Log($"music: could not sweep the video cache: {ex.Message}");
        }
    }
}
