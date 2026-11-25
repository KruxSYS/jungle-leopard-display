using System;
using System.Runtime.InteropServices;
using System.Text;

namespace JLDisplayManager.Interop;

/// <summary>
/// One-to-one binding for JLDisplayNative.dll. The struct layouts here must stay
/// in step with jl_api.h — they are blitted, not marshalled field by field, so a
/// mismatch corrupts silently rather than throwing. <see cref="VerifyLayout"/>
/// checks the sizes at startup so a drift shows up as a clear failure instead.
/// </summary>
public static class NativeMethods
{
    private const string Dll = "JLDisplayNative.dll";

    // Mirrors JlState.
    public enum JlState
    {
        Disconnected = 0,
        Idle = 1,
        Preparing = 2,
        Calibrating = 3,
        Playing = 4,
        Error = 5,
        Preprocessing = 6,
    }

    /// <summary>
    /// Mirrors JlPreprocess: how much of the transcoding happens before playback
    /// rather than during it. Session-wide, and deliberately not part of
    /// <see cref="JlRenderOpts"/> — it changes nothing about the pixels.
    /// </summary>
    public enum JlPreprocess
    {
        Off = 0,
        Memory = 1,
        Disk = 2,
    }

    public const int JL_OK = 0;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct JlRenderOpts
    {
        public int Stretch;
        public int Rotate;
        public int Fps;
        public int Quality;
        public int Loop;
        public int Recalibrate;

        /// <summary>
        /// Preprocessing only: encode every frame at the best quality it can
        /// afford rather than at the one figure the busiest frame in the file
        /// allows. Bigger packs, better picture.
        /// </summary>
        public int PerFrameQuality;

        /// <summary>0 = the default 84, else 50..98 percent.</summary>
        public int QualityCeiling;

        /// <summary>
        /// What the link and the panel behind it can swallow, in bytes per
        /// second. Divided by the frame rate this is what one frame may spend,
        /// and it is the setting that decides whether playback stutters.
        /// 0 = the default.
        /// </summary>
        public uint ByteRate;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Hwaccel;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct JlStatus
    {
        public int State;
        public int Connected;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Port;

        public long FramesSent;
        public long FramesDropped;
        public double Fps;

        /// <summary>Where the playing video has reached, in seconds.</summary>
        public double PositionSeconds;

        /// <summary>How long it runs, or 0 when that could not be determined.</summary>
        public double DurationSeconds;
        public int FinishedCount;
        public int FrameCount;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Message;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Error;

        /// <summary>Rolling mean of the per-frame decode + blend, in ms.</summary>
        public double OverlayComposeMs;

        /// <summary>Rolling mean of the per-frame JPEG re-encode, in ms.</summary>
        public double OverlayEncodeMs;

        /// <summary>
        /// Rolling mean of the composed frame, in bytes. Times the frame rate,
        /// this is the data rate actually reaching the panel — the only way to
        /// see the compositor overspending, because the panel accepts every
        /// byte and falls behind silently rather than reporting a drop.
        /// </summary>
        public double OverlayOutBytes;

        /// <summary>
        /// The working JPEG quality × 100, 5–92. Falls when the rate budget
        /// forces it down, which is the visible symptom of an overlay too
        /// expensive to encode — worth surfacing rather than hiding.
        /// </summary>
        public int OverlayQuality;

        /// <summary>Frames that would not fit under the cap at any quality.</summary>
        public int OverlayDrops;

        /// <summary>
        /// Frames sent above the per-frame rate budget. These are not drops —
        /// they went out, because a gap costs more than one expensive frame —
        /// but a number that climbs steadily means the rate is set too low for
        /// the content, or the overlay too heavy for the rate.
        /// </summary>
        public int OverlayOverBudget;

        /// <summary>Padding; mirrors overlayReserved in jl_api.h.</summary>
        public int OverlayReserved;
    }

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_find_port(StringBuilder buf, int cch);

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_open(string? port);

    [DllImport(Dll)]
    public static extern void jl_close();

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_get_info(StringBuilder buf, int cch);

    [DllImport(Dll)]
    public static extern int jl_set_brightness(int percent);

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_show_image(string path, ref JlRenderOpts opts);

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_play_video(string path, ref JlRenderOpts opts);

    /// <summary>
    /// Plays a live URL. Never packed and never calibrated — a stream has no
    /// file identity to key a pack on, and a survey pass would fetch the whole
    /// thing a second time.
    ///
    /// <paramref name="inputArgs"/> goes to ffmpeg verbatim before -i, which is
    /// how a signed URL gets the user agent it was issued to. It is a separate
    /// argument rather than a field on <see cref="JlRenderOpts"/> because that
    /// struct is key material for the caches.
    /// </summary>
    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_play_stream(string url, string inputArgs,
        double startSeconds, double durationSeconds, ref JlRenderOpts opts);

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_calibrate(string path, ref JlRenderOpts opts);

    /// <summary>
    /// Builds a file's frames into the on-disk pack cache, so playing it later
    /// runs no ffmpeg. Blocking and device-free; the options must be the ones
    /// playback will use, because they are part of what the frames are.
    /// </summary>
    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_preprocess(string path, ref JlRenderOpts opts);

    /// <summary>
    /// Whether playing this file with these options would find a pack waiting,
    /// without building one.
    /// </summary>
    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_pack_ready(string path, ref JlRenderOpts opts);

    /// <summary>
    /// Frames built so far by the <see cref="jl_preprocess"/> call in flight, or
    /// -1 when there is none.
    /// </summary>
    [DllImport(Dll)]
    public static extern int jl_preprocess_frames();

    /// <summary>
    /// The last warning or error from the most recent preprocess — the only
    /// account of why one failed.
    /// </summary>
    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_preprocess_message(StringBuilder buf, int cch);

    [DllImport(Dll)]
    public static extern void jl_stop();

    /// <summary>
    /// Moves the playing video to <paramref name="seconds"/>, taking effect at
    /// the next frame. Ignored when nothing is playing.
    /// </summary>
    [DllImport(Dll)]
    public static extern void jl_seek(double seconds);

    [DllImport(Dll)]
    public static extern void jl_get_status(out JlStatus status);

    [DllImport(Dll)]
    public static extern int jl_get_last_frame(byte[]? buf, int cap);

    [DllImport(Dll)]
    public static extern void jl_set_preprocess(int mode);

    [DllImport(Dll)]
    public static extern int jl_get_preprocess();

    [DllImport(Dll)]
    public static extern void jl_set_pack_budgets(long memoryBytes, long diskBytes);

    [DllImport(Dll)]
    public static extern long jl_memory_budget();

    [DllImport(Dll)]
    public static extern long jl_disk_budget();

    [DllImport(Dll)]
    public static extern long jl_pack_cache_bytes();

    [DllImport(Dll)]
    public static extern void jl_pack_cache_clear();

    /// <summary>
    /// Turns compositing on or off. Off costs nothing — frames reach the panel
    /// exactly as they did before the overlay existed. Takes effect on the next
    /// frame, including in the middle of a playing video.
    /// </summary>
    [DllImport(Dll)]
    public static extern void jl_overlay_set_enabled(int on);

    /// <summary>
    /// Replaces the overlay surface: 960×480 BGRA with <b>premultiplied</b>
    /// alpha, which is exactly what WPF's Pbgra32 produces. The pixels are
    /// copied, so the buffer can be reused as soon as this returns.
    /// </summary>
    [DllImport(Dll)]
    public static extern int jl_overlay_update(byte[] bgraPremultiplied, int w, int h);

    /// <summary>Drops the surface, leaving nothing drawn on top.</summary>
    [DllImport(Dll)]
    public static extern void jl_overlay_clear();

    /// <summary>
    /// Mirrors JlMediaStatus: what some other program on this machine is
    /// playing, read from the Windows media session. Nothing here plays audio —
    /// the panel cannot — it exists so the manager can follow along.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct JlMediaStatus
    {
        /// <summary>The native reader thread is up.</summary>
        public int Running;

        /// <summary>The session manager was obtained. False on a machine too old for it.</summary>
        public int Available;

        /// <summary>A session is being followed.</summary>
        public int Present;

        public int Playing;

        /// <summary>
        /// Where the track is now, in seconds — extrapolated natively from the
        /// last timeline report, because players refresh that every few seconds
        /// rather than every frame.
        /// </summary>
        public double Position;

        /// <summary>Track length, or 0 when the player does not say.</summary>
        public double Duration;

        /// <summary>
        /// Bumps on any change of title, artist or album. Watched rather than
        /// diffed, so the same track played twice still reads as a new track.
        /// </summary>
        public int TrackVersion;

        public int ThumbnailVersion;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Title;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Artist;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Album;

        /// <summary>
        /// The source app id. Often opaque rather than an executable name —
        /// Firefox reports "308046B0AF4A39CB" — so it is an identity to match
        /// on, not a label to show.
        /// </summary>
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string SourceApp;
    }

    /// <summary>
    /// Starts the media-session reader, or repoints a running one.
    /// <paramref name="preferredApp"/> is a source app id, or null to follow
    /// whichever session is playing.
    /// </summary>
    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_media_start(string? preferredApp);

    [DllImport(Dll)]
    public static extern void jl_media_stop();

    [DllImport(Dll)]
    public static extern void jl_media_get(out JlMediaStatus status);

    /// <summary>
    /// Every registered session's app id, NUL-separated with a trailing NUL.
    /// Returns the characters needed; pass null to ask for the size.
    /// </summary>
    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_media_sessions([Out] char[]? buf, int cch);

    /// <summary>
    /// The current track's album art as the player supplied it — PNG or JPEG.
    /// Returns the byte count, the size needed when <paramref name="cap"/> is
    /// too small, or 0 when there is none.
    /// </summary>
    [DllImport(Dll)]
    public static extern int jl_media_thumbnail(byte[]? buf, int cap);

    [DllImport(Dll, CharSet = CharSet.Unicode)]
    public static extern int jl_ffmpeg_path(StringBuilder buf, int cch);

    /// <summary>
    /// Throws if the managed structs no longer match the native header. Cheap,
    /// and it turns a whole class of silent memory corruption into one message.
    /// </summary>
    public static void VerifyLayout()
    {
        int opts = Marshal.SizeOf<JlRenderOpts>();
        int status = Marshal.SizeOf<JlStatus>();
        int media = Marshal.SizeOf<JlMediaStatus>();

        // JlStatus: 1128 originally; 1144 once it gained the two playback
        // position doubles; 1168 with the four overlay diagnostics; 1184 now
        // those carry the composed frame size and the over-budget count too.
        // JlRenderOpts: 88 before per-frame quality; 96 before its byte rate.
        // JlMediaStatus: 1832 as introduced.
        if (opts != 100 || status != 1184 || media != 1832)
        {
            throw new InvalidOperationException(
                $"JLDisplayNative.dll interop layout mismatch: " +
                $"JlRenderOpts={opts} (expected 100), JlStatus={status} (expected 1184), " +
                $"JlMediaStatus={media} (expected 1832). " +
                "Interop/NativeMethods.cs and jl_api.h have drifted apart.");
        }
    }
}
