using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows.Media.Imaging;

using JLDisplayManager.Models;

namespace JLDisplayManager.Services.Music;

/// <summary>
/// Turns the album art the player hands over into two files on disk: the art
/// itself, and a blurred backdrop that fills the panel behind it.
///
/// Two files rather than one composed picture because the panel is not a
/// canvas — it takes a still, and everything live on top of it is the overlay's
/// job. The backdrop is the still; the art is an <c>ImageLayer</c>.
///
/// Both are named after a hash of the art, which does the caching for free: an
/// album played twice writes nothing the second time, and the changing path is
/// what tells the renderer's image cache — keyed on path — that this is a
/// different picture.
///
/// Nothing here draws. It is all decode, scale and encode, which is safe on a
/// pool thread; a <c>DrawingVisual</c> would not be.
/// </summary>
public static class MusicArt
{
    /// <summary>
    /// Longest edge of the art file. The panel is 480 tall and the card shows it
    /// far smaller than that, so anything bigger is memory in the renderer's
    /// image cache and nothing on the glass.
    /// </summary>
    private const int ArtMax = 320;

    /// <summary>
    /// How small the art is squashed before being blown back up. This IS the
    /// blur — a box filter with a 40:1 ratio behind it — and it costs one
    /// decode instead of a render pass and a BlurEffect the codebase already
    /// found slow and unpredictable.
    /// </summary>
    private const int BlurSeed = 24;

    /// <summary>
    /// Below the panel's 80 KB cap with room to spare, and low enough that WIC
    /// keeps 4:2:0 subsampling — above roughly 90 it switches to 4:4:4, which
    /// the panel cannot decode and which would send the still through ffmpeg
    /// for no reason.
    /// </summary>
    private const int JpegQuality = 80;

    /// <summary>Files kept before the oldest are swept up.</summary>
    private const int KeepFiles = 120;

    private static readonly object Lock = new();
    private static int _lastVersion = -1;
    private static int _working;

    private static string? _artPath;
    private static string? _backdropPath;

    /// <summary>The current track's art on disk, or null when it has none.</summary>
    public static string? ArtPath => Volatile.Read(ref _artPath);

    /// <summary>A 960x480 blurred still to sit behind the card.</summary>
    public static string? BackdropPath => Volatile.Read(ref _backdropPath);

    /// <summary>
    /// Bumps when either path changes, so a caller can notice without comparing
    /// strings it would otherwise have to remember.
    /// </summary>
    public static int Version { get; private set; }

    /// <summary>
    /// Called from the sensor poll with what the reader last saw. Cheap when
    /// nothing has changed, which is nearly always: the real work happens off
    /// this thread and only when the art version moves.
    /// </summary>
    public static void Update(int thumbnailVersion, bool present)
    {
        if (!present)
        {
            Clear();
            return;
        }

        if (thumbnailVersion == _lastVersion) return;
        _lastVersion = thumbnailVersion;

        // One preparation at a time. A skipped run is not a lost one: the
        // version has already moved, so the next poll after this finishes sees
        // the difference and starts again.
        if (Interlocked.CompareExchange(ref _working, 1, 0) != 0)
        {
            _lastVersion = -1;
            return;
        }

        Task.Run(() =>
        {
            try
            {
                Prepare();
            }
            catch (Exception ex)
            {
                Storage.Log($"music art: {ex.Message}");
                Clear();
            }
            finally
            {
                Volatile.Write(ref _working, 0);
            }
        });
    }

    public static void Clear()
    {
        _lastVersion = -1;
        if (Volatile.Read(ref _artPath) == null && Volatile.Read(ref _backdropPath) == null) return;

        Volatile.Write(ref _artPath, null);
        Volatile.Write(ref _backdropPath, null);
        Version++;
    }

    private static void Prepare()
    {
        byte[]? bytes = MediaSession.Thumbnail();
        if (bytes == null || bytes.Length == 0)
        {
            Clear();
            return;
        }

        string key = Key(bytes);
        string art = Path.Combine(Storage.MusicArtDirectory, key + ".jpg");
        string backdrop = Path.Combine(Storage.MusicArtDirectory, key + "-bg.jpg");

        lock (Lock)
        {
            if (!File.Exists(art) || !File.Exists(backdrop))
            {
                Directory.CreateDirectory(Storage.MusicArtDirectory);

                BitmapSource source = Decode(bytes);
                Write(Fit(source, ArtMax), art);
                Write(Blur(source), backdrop);
                Prune();
            }
            else
            {
                // Touched so the sweep below measures use, not first sight.
                try { File.SetLastWriteTimeUtc(art, DateTime.UtcNow); } catch { }
            }
        }

        Volatile.Write(ref _artPath, art);
        Volatile.Write(ref _backdropPath, backdrop);
        Version++;
    }

    /// <summary>
    /// The still used when the track has no art at all: a flat, near-black
    /// 960x480 frame for the card to sit on. Written once and reused.
    ///
    /// It exists because the panel always needs something underneath — the
    /// overlay composites onto a frame, and with nothing playing there are no
    /// frames to composite onto.
    /// </summary>
    public static string? PlainBackdrop()
    {
        string path = Path.Combine(Storage.MusicArtDirectory, "plain.jpg");

        try
        {
            if (File.Exists(path)) return path;

            Directory.CreateDirectory(Storage.MusicArtDirectory);

            const int stride = OverlayPanelWidth * 4;
            byte[] pixels = new byte[stride * OverlayPanelHeight];
            for (int i = 0; i < pixels.Length; i += 4)
            {
                pixels[i] = 0x12;       // B
                pixels[i + 1] = 0x10;   // G
                pixels[i + 2] = 0x0E;   // R
                pixels[i + 3] = 0xFF;
            }

            var bmp = BitmapSource.Create(OverlayPanelWidth, OverlayPanelHeight, 96, 96,
                PixelFormats.Bgra32, null, pixels, stride);
            bmp.Freeze();

            Write(bmp, path);
            return path;
        }
        catch (Exception ex)
        {
            Storage.Log($"music art: could not write the plain backdrop: {ex.Message}");
            return null;
        }
    }

    private static string Key(byte[] bytes)
    {
        byte[] hash = SHA256.HashData(bytes);
        return Convert.ToHexString(hash, 0, 8).ToLowerInvariant();
    }

    private static BitmapSource Decode(byte[] bytes)
    {
        using var stream = new MemoryStream(bytes);

        // OnLoad so the stream can go: everything after this works from pixels.
        var bmp = new BitmapImage();
        bmp.BeginInit();
        bmp.CacheOption = BitmapCacheOption.OnLoad;
        bmp.StreamSource = stream;
        bmp.EndInit();
        bmp.Freeze();
        return bmp;
    }

    /// <summary>Scales down so the longest edge is at most <paramref name="max"/>.</summary>
    private static BitmapSource Fit(BitmapSource source, int max)
    {
        double scale = Math.Min(1.0, (double)max / Math.Max(source.PixelWidth, source.PixelHeight));
        if (scale >= 1.0) return source;

        var scaled = new TransformedBitmap(source, new ScaleTransform(scale, scale));
        scaled.Freeze();
        return scaled;
    }

    /// <summary>
    /// The backdrop: the art squashed to a couple of dozen pixels, blown back up
    /// to cover the panel, and cropped to it. No darkening happens here — the
    /// card's overlay draws its own scrim, which keeps the one adjustable thing
    /// adjustable in the editor rather than baked into a cache file.
    /// </summary>
    private static BitmapSource Blur(BitmapSource source)
    {
        BitmapSource small = Fit(source, BlurSeed);

        double scale = Math.Max(
            (double)OverlayPanelWidth / small.PixelWidth,
            (double)OverlayPanelHeight / small.PixelHeight);

        var big = new TransformedBitmap(small, new ScaleTransform(scale, scale));
        big.Freeze();

        int x = Math.Max(0, (big.PixelWidth - OverlayPanelWidth) / 2);
        int y = Math.Max(0, (big.PixelHeight - OverlayPanelHeight) / 2);
        int w = Math.Min(OverlayPanelWidth, big.PixelWidth);
        int h = Math.Min(OverlayPanelHeight, big.PixelHeight);

        var cropped = new CroppedBitmap(big, new System.Windows.Int32Rect(x, y, w, h));
        cropped.Freeze();
        return cropped;
    }

    private const int OverlayPanelWidth = 960;
    private const int OverlayPanelHeight = 480;

    private static void Write(BitmapSource bmp, string path)
    {
        var encoder = new JpegBitmapEncoder { QualityLevel = JpegQuality };
        encoder.Frames.Add(BitmapFrame.Create(bmp));

        // Through a temporary file: a half-written backdrop is a path the panel
        // would be asked to show, and a truncated JPEG is a failure rather than
        // a blank.
        string temp = path + ".tmp";
        using (var file = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
            encoder.Save(file);

        File.Move(temp, path, overwrite: true);
    }

    /// <summary>
    /// Keeps the cache bounded. These are files nothing references once the
    /// track has moved on, so the oldest simply go.
    /// </summary>
    private static void Prune()
    {
        try
        {
            var files = new DirectoryInfo(Storage.MusicArtDirectory).GetFiles("*.jpg");
            if (files.Length <= KeepFiles) return;

            IEnumerable<FileInfo> oldest = files
                .OrderByDescending(f => f.LastWriteTimeUtc)
                .Skip(KeepFiles);

            foreach (FileInfo f in oldest)
            {
                try { f.Delete(); } catch { }
            }
        }
        catch (Exception ex)
        {
            Storage.Log($"music art: could not prune the cache: {ex.Message}");
        }
    }
}
