using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace JLDisplayManager.Models;

/// <summary>
/// How much of the transcoding happens before an item starts rather than while
/// it plays. Mirrors JlPreprocess on the native side.
/// </summary>
public enum PreprocessMode
{
    /// <summary>ffmpeg streams for as long as something is on the panel.</summary>
    Off = 0,

    /// <summary>Frames are built into RAM once, then ffmpeg stops.</summary>
    Memory = 1,

    /// <summary>Frames are built into a file once and reused across runs.</summary>
    Disk = 2,
}

/// <summary>
/// Everything the app remembers between runs, split in two files: settings the
/// user tweaks, and the library they build up. Both live under LOCALAPPDATA
/// next to the calibration cache the CLI already writes.
/// </summary>
public sealed class AppSettings : INotifyPropertyChanged
{
    private int _rotate;
    private bool _stretch;
    private int _fps = 30;
    private int _brightness = 100;
    private string _hwaccel = "auto";
    private string _port = "";
    private bool _startWithWindows;
    private bool _startMinimised = true;
    private bool _resumeOnStart = true;
    private bool _autoReconnect = true;
    private bool _blankOnExit = true;
    private PreprocessMode _preprocess = PreprocessMode.Memory;
    private int _memoryBudgetMB = 512;
    private int _diskBudgetMB = 8192;
    private bool _perFrameQuality = true;
    private int _qualityCeiling = 84;
    private int _byteRateKBs = 1150;

    /// <summary>Global default; a MediaItem may override it.</summary>
    public int Rotate
    {
        get => _rotate;
        set => Set(ref _rotate, Normalise(value));
    }

    public bool Stretch
    {
        get => _stretch;
        set => Set(ref _stretch, value);
    }

    public int Fps
    {
        get => _fps;
        set => Set(ref _fps, Math.Clamp(value, 1, 60));
    }

    public int Brightness
    {
        get => _brightness;
        set => Set(ref _brightness, Math.Clamp(value, 0, 100));
    }

    /// <summary>"auto", "none", or an explicit ffmpeg method such as "cuda".</summary>
    public string Hwaccel
    {
        get => _hwaccel;
        set => Set(ref _hwaccel, value ?? "auto");
    }

    /// <summary>
    /// Whether frames are prepared up front. Memory is the default: it writes
    /// nothing, is bounded, and quietly falls back to streaming for a source too
    /// long to hold — so the worst case is exactly the old behaviour.
    /// </summary>
    public PreprocessMode Preprocess
    {
        get => _preprocess;
        set => Set(ref _preprocess, value);
    }

    /// <summary>
    /// Ceiling for Memory mode, in megabytes. A source whose frames would not
    /// fit streams instead, so this trades RAM for how much of the library gets
    /// the benefit rather than deciding what will play.
    /// </summary>
    public int MemoryBudgetMB
    {
        get => _memoryBudgetMB;
        set => Set(ref _memoryBudgetMB, Math.Clamp(value, 32, 16384));
    }

    /// <summary>
    /// Ceiling for the Disk pack cache, in megabytes. Reaching it evicts the
    /// least recently used packs rather than refusing to build new ones.
    /// </summary>
    public int DiskBudgetMB
    {
        get => _diskBudgetMB;
        set => Set(ref _diskBudgetMB, Math.Clamp(value, 128, 262144));
    }

    /// <summary>
    /// Whether a preprocessed frame gets the best quality it can afford rather
    /// than the one figure the busiest frame in its file allows. On by default:
    /// the whole reason to prepare frames up front is that there is no longer
    /// any need to commit to a figure before seeing them. It costs pack size,
    /// which <see cref="QualityCeiling"/> is what bounds.
    /// </summary>
    public bool PerFrameQuality
    {
        get => _perFrameQuality;
        set => Set(ref _perFrameQuality, value);
    }

    /// <summary>
    /// The best a single frame is allowed to be, as a percentage. Not surfaced:
    /// <see cref="ByteRateKBs"/> binds first on nearly all content, and this
    /// only decides how good a frame simple enough to fit anyway is allowed to
    /// get. Here so it can be changed in settings.json if that ever matters.
    /// </summary>
    public int QualityCeiling
    {
        get => _qualityCeiling;
        set => Set(ref _qualityCeiling, Math.Clamp(value, 50, 98));
    }

    /// <summary>
    /// How many kilobytes a second of preprocessed frames the panel is asked to
    /// swallow, divided by the frame rate to get what one frame may spend. This
    /// is the setting that decides whether playback stutters, and the limit is
    /// the panel's own decoding rather than the link — the link measures at
    /// 3 MB/s, and the panel reports nothing when it falls behind.
    ///
    /// The default is the rate the app has always run at — one -q:v 3 frame is
    /// about 37 KB, which at 30 fps is close to this exactly.
    /// </summary>
    public int ByteRateKBs
    {
        get => _byteRateKBs;
        set => Set(ref _byteRateKBs, Math.Clamp(value, 400, 4096));
    }

    /// <summary>Empty means autodetect by hardware ID, which is nearly always right.</summary>
    public string Port
    {
        get => _port;
        set => Set(ref _port, value ?? "");
    }

    public bool StartWithWindows
    {
        get => _startWithWindows;
        set => Set(ref _startWithWindows, value);
    }

    public bool StartMinimised
    {
        get => _startMinimised;
        set => Set(ref _startMinimised, value);
    }

    /// <summary>Put back whatever was showing when the app last closed.</summary>
    public bool ResumeOnStart
    {
        get => _resumeOnStart;
        set => Set(ref _resumeOnStart, value);
    }

    public bool AutoReconnect
    {
        get => _autoReconnect;
        set => Set(ref _autoReconnect, value);
    }

    /// <summary>
    /// Whether to leave the panel dark on exit. Off means the last frame stays
    /// frozen on the glass, which the firmware is happy to do indefinitely.
    /// </summary>
    public bool BlankOnExit
    {
        get => _blankOnExit;
        set => Set(ref _blankOnExit, value);
    }

    // -----------------------------------------------------------------------
    // Optional sensor sources
    //
    // Windows cannot report CPU temperature on its own — on AMD it lives behind
    // the SMU and needs ring-0 access, which is why LibreHardwareMonitor ships a
    // kernel driver. Rather than shipping one too, this reads what a monitor the
    // user already runs has already measured.
    // -----------------------------------------------------------------------

    private bool _useLibreHardwareMonitor = true;
    private int _libreHardwareMonitorPort = 8085;
    private bool _useHwInfo = true;

    /// <summary>
    /// Read from LibreHardwareMonitor's web server, if it is running. On by
    /// default and harmless when it is not: the provider reports nothing and the
    /// sensors stay unavailable.
    /// </summary>
    public bool UseLibreHardwareMonitor
    {
        get => _useLibreHardwareMonitor;
        set => Set(ref _useLibreHardwareMonitor, value);
    }

    public int LibreHardwareMonitorPort
    {
        get => _libreHardwareMonitorPort;
        set => Set(ref _libreHardwareMonitorPort, Math.Clamp(value, 1, 65535));
    }

    /// <summary>Read HWiNFO's shared memory, if it is enabled there.</summary>
    public bool UseHwInfo
    {
        get => _useHwInfo;
        set => Set(ref _useHwInfo, value);
    }

    // -----------------------------------------------------------------------
    // What is playing elsewhere on the machine
    // -----------------------------------------------------------------------

    private bool _useMediaSession = true;
    private string _mediaSessionApp = "";
    private bool _followMusic;
    private bool _musicVideo = true;
    private int _musicCacheMB = 4096;
    private int _musicCacheDays = 14;
    private int _musicMaxVideoMinutes = 12;

    /// <summary>
    /// Publish <c>music.*</c> sensors from the Windows media session — whatever
    /// is playing in a browser tab or a music player. On by default and cheap:
    /// the reader is one thread that polls a property, and a machine with
    /// nothing playing simply reports nothing.
    /// </summary>
    public bool UseMediaSession
    {
        get => _useMediaSession;
        set => Set(ref _useMediaSession, value);
    }

    /// <summary>
    /// Which player to follow, by source app id, or empty to follow whichever
    /// one is playing. An id rather than a name because that is what Windows
    /// reports, and it is frequently opaque — Firefox calls itself
    /// <c>308046B0AF4A39CB</c>.
    /// </summary>
    public string MediaSessionApp
    {
        get => _mediaSessionApp;
        set => Set(ref _mediaSessionApp, value ?? "");
    }

    /// <summary>
    /// Whether the panel follows the media session. Off by default, and
    /// remembered: a pump display is set once and left, so a machine that was
    /// following when it shut down should be following when it comes back.
    /// </summary>
    public bool FollowMusic
    {
        get => _followMusic;
        set => Set(ref _followMusic, value);
    }

    /// <summary>
    /// Whether following also streams the music video, falling back to the card
    /// when no video can be found — which on a music service is most tracks.
    /// Off means the card, always, and no yt-dlp searching at all.
    /// </summary>
    public bool MusicVideo
    {
        get => _musicVideo;
        set => Set(ref _musicVideo, value);
    }

    /// <summary>
    /// The longest video worth downloading, in minutes. 0 accepts any length.
    ///
    /// A search for a song returns the track, a lyric video, and — reliably —
    /// an hour of it on loop. Length is checked before the download rather than
    /// after, because the download is the part that costs: a match this far off
    /// is not the recording being played and was never going to be shown.
    /// </summary>
    public int MusicMaxVideoMinutes
    {
        get => _musicMaxVideoMinutes;
        set => Set(ref _musicMaxVideoMinutes, Math.Clamp(value, 0, 600));
    }

    /// <summary>
    /// How much disk the downloaded music videos may take. Reaching it drops
    /// the least recently played rather than refusing to fetch new ones.
    /// </summary>
    public int MusicCacheMB
    {
        get => _musicCacheMB;
        set => Set(ref _musicCacheMB, Math.Clamp(value, 128, 262144));
    }

    /// <summary>
    /// How long a downloaded video is kept after it was last played, in days.
    /// 0 keeps them until the size budget alone evicts them.
    ///
    /// Age matters more than size here: a listening habit moves on, and a
    /// library that only ever grew to its budget would be full of tracks
    /// nobody has played in months.
    /// </summary>
    public int MusicCacheDays
    {
        get => _musicCacheDays;
        set => Set(ref _musicCacheDays, Math.Clamp(value, 0, 3650));
    }

    private static int Normalise(int degrees)
    {
        int d = ((degrees % 360) + 360) % 360;
        return d - (d % 90);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}

/// <summary>The library and the playlist built from it.</summary>
public sealed class AppLibrary
{
    public List<MediaItem> Items { get; set; } = new();

    /// <summary>Ids into <see cref="Items"/>, in play order. Duplicates allowed.</summary>
    public List<Guid> Playlist { get; set; } = new();

    public bool Shuffle { get; set; }

    /// <summary>What to restore on launch when ResumeOnStart is set.</summary>
    public Guid? LastPlayed { get; set; }

    public bool LastWasPlaylist { get; set; }
}

public static class Storage
{
    public static string Directory { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "JungleLeopardDisplay");

    public static string ThumbnailDirectory { get; } = Path.Combine(Directory, "thumbnails");

    /// <summary>Where a YouTube download lands before it is added to the library.</summary>
    public static string DownloadDirectory { get; } = Path.Combine(Directory, "downloads");

    /// <summary>Where the AI pipeline writes what it generates.</summary>
    public static string GeneratedDirectory { get; } = Path.Combine(Directory, "generated");

    /// <summary>
    /// Images an overlay draws. Files are copied in when added, so a profile
    /// keeps working when the original is moved or deleted.
    /// </summary>
    public static string OverlayAssetDirectory { get; } = Path.Combine(Directory, "overlay-assets");

    /// <summary>
    /// Album art from the media session, and the blurred backdrops built from
    /// it. Kept apart from overlay-assets because these are cache, not
    /// documents: nothing here is referenced by a saved profile, and deleting
    /// the lot costs one redraw.
    /// </summary>
    public static string MusicArtDirectory { get; } = Path.Combine(Directory, "music-art");

    /// <summary>
    /// Music videos fetched for the follower, one file per YouTube id.
    ///
    /// Cache, not library: nothing references these by name and deleting the
    /// folder costs only the next download. They are kept because a track
    /// played twice should start instantly the second time — and because a file
    /// on disk can be seeked, which a YouTube stream cannot.
    /// </summary>
    public static string MusicVideoDirectory { get; } = Path.Combine(Directory, "music-videos");

    private static string SettingsPath => Path.Combine(Directory, "settings.json");
    private static string LibraryPath => Path.Combine(Directory, "library.json");
    private static string AiPath => Path.Combine(Directory, "ai.json");

    /// <summary>
    /// Overlay profiles live apart from settings.json so one can be exported,
    /// shared or imported without dragging everything else along.
    /// </summary>
    private static string OverlayPath => Path.Combine(Directory, "overlays.json");

    public static string LogPath => Path.Combine(Directory, "manager.log");

    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    public static void EnsureDirectories()
    {
        System.IO.Directory.CreateDirectory(Directory);
        System.IO.Directory.CreateDirectory(ThumbnailDirectory);
        System.IO.Directory.CreateDirectory(DownloadDirectory);
        System.IO.Directory.CreateDirectory(GeneratedDirectory);
        System.IO.Directory.CreateDirectory(OverlayAssetDirectory);
        System.IO.Directory.CreateDirectory(MusicArtDirectory);
        System.IO.Directory.CreateDirectory(MusicVideoDirectory);
    }

    /// <summary>
    /// Whether settings have ever been saved here. False on the first run of a
    /// fresh install, which is the one start where hiding in the tray is wrong:
    /// nobody knows to look there yet.
    /// </summary>
    public static bool HasSettings => File.Exists(SettingsPath);

    public static AppSettings LoadSettings() => Load<AppSettings>(SettingsPath) ?? new AppSettings();

    public static AppLibrary LoadLibrary() => Load<AppLibrary>(LibraryPath) ?? new AppLibrary();

    public static AiSettings LoadAi()
    {
        var ai = Load<AiSettings>(AiPath) ?? new AiSettings();

        // An untouched system prompt from an older version is moved on to the
        // current default. Anything the user has edited is left as they wrote it.
        if (AiSettings.IsSupersededSystemPrompt(ai.SystemPrompt))
            ai.SystemPrompt = AiSettings.DefaultSystemPrompt;

        return ai;
    }

    /// <summary>
    /// Overlay profiles. A first run — or a file so damaged it will not parse —
    /// comes back with one usable profile rather than an empty panel and no way
    /// to discover the feature.
    /// </summary>
    public static Overlay.OverlaySettings LoadOverlays()
    {
        var o = Load<Overlay.OverlaySettings>(OverlayPath) ?? new Overlay.OverlaySettings();

        if (o.Profiles.Count == 0)
        {
            o.Profiles.Add(Overlay.OverlaySettings.CreateDefault());
            o.ActiveProfileId = o.Profiles[0].Id;
        }

        // Anything generated before themes existed has its colours baked in and
        // would ignore a theme entirely. Converting the known old values to
        // roles is invisible under the default theme and is what makes the
        // picker do something on a profile made yesterday.
        int migrated = Overlay.OverlaySettings.MigrateToRoles(o);
        if (migrated > 0) Log($"overlay: moved {migrated} colour(s) onto the theme");

        return o;
    }

    public static void SaveOverlays(Overlay.OverlaySettings o) => Save(OverlayPath, o);

    public static void SaveSettings(AppSettings s) => Save(SettingsPath, s);

    public static void SaveLibrary(AppLibrary l) => Save(LibraryPath, l);

    public static void SaveAi(AiSettings a) => Save(AiPath, a);

    internal static T? Load<T>(string path) where T : class
    {
        try
        {
            if (!File.Exists(path)) return null;
            return JsonSerializer.Deserialize<T>(File.ReadAllText(path), Json);
        }
        catch (Exception ex)
        {
            // A corrupt settings file must not stop the app from starting, so
            // it comes up with defaults - but the file is moved aside first.
            // Left where it was, the next save (any edit, or quitting) would
            // write those defaults over it, and a library that failed to parse
            // once - a crash mid-write on an old version, a disk hiccup, a file
            // from a newer release - would be gone for good instead of being
            // one rename away from recovery.
            string kept = Quarantine(path);
            Log($"could not read {Path.GetFileName(path)}: {ex.Message}" +
                (kept.Length > 0 ? $" - kept it as {Path.GetFileName(kept)}" : ""));
            return null;
        }
    }

    /// <summary>
    /// Renames an unreadable file to <c>name.corrupt-yyyyMMdd-HHmmss</c> beside
    /// it and returns the new path, or "" if it could not be moved.
    /// </summary>
    internal static string Quarantine(string path)
    {
        try
        {
            string target = $"{path}.corrupt-{DateTime.Now:yyyyMMdd-HHmmss}";
            for (int n = 2; File.Exists(target); n++)
                target = $"{path}.corrupt-{DateTime.Now:yyyyMMdd-HHmmss}-{n}";
            File.Move(path, target);
            return target;
        }
        catch
        {
            return "";
        }
    }

    internal static void Save<T>(string path, T value)
    {
        try
        {
            EnsureDirectories();
            // Write beside and move, so a crash mid-write cannot leave a
            // half-written file where the real one was.
            string temp = path + ".tmp";
            File.WriteAllText(temp, JsonSerializer.Serialize(value, Json));
            File.Move(temp, path, overwrite: true);
        }
        catch (Exception ex)
        {
            Log($"could not write {Path.GetFileName(path)}: {ex.Message}");
        }
    }

    /// <summary>
    /// Past this the log is rolled to manager.log.1 (one generation kept), so
    /// it stays readable and a tray app that runs for years cannot fill a disk
    /// with it. The follower alone writes a few lines per track.
    /// </summary>
    internal const long LogRollBytes = 2 * 1024 * 1024;

    private static readonly object LogLock = new();

    public static void Log(string message)
    {
        // Serialised: this is called from the UI thread, pool threads and the
        // pack build at once, and two appends racing for the file used to lose
        // one of the lines to a sharing violation, silently.
        lock (LogLock)
        {
            try
            {
                EnsureDirectories();
                RollIfLarge(LogPath, LogRollBytes);
                File.AppendAllText(LogPath, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss}  {message}{Environment.NewLine}");
            }
            catch
            {
                // Logging must never be the thing that breaks the app.
            }
        }
    }

    internal static void RollIfLarge(string path, long limit)
    {
        var info = new FileInfo(path);
        if (!info.Exists || info.Length < limit) return;
        File.Move(path, path + ".1", overwrite: true);
    }
}
