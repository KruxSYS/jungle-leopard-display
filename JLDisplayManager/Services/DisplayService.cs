using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

using JLDisplayManager.Interop;
using JLDisplayManager.Models;

namespace JLDisplayManager.Services;

/// <summary>
/// The app's single connection to the panel.
///
/// The native side does all the blocking work on its own threads and publishes a
/// status struct; this class polls that struct on the dispatcher and turns it
/// into properties the UI can bind to. Nothing here ever blocks — the one rule
/// that makes the whole design hang together.
/// </summary>
public sealed class DisplayService : INotifyPropertyChanged, IDisposable
{
    private const int PollMs = 250;
    private const int ReconnectMs = 2000;

    /// <summary>
    /// How long after a wake to leave the device alone before the first
    /// reconnect attempt. USB re-enumeration is not instant and an attempt made
    /// before it finishes is certain to fail; the reconnect loop would retry
    /// anyway, so this only saves a pointless first go.
    /// </summary>
    private const int ResumeSettleMs = 2000;

    private readonly AppSettings _settings;
    private readonly DispatcherTimer _poll;

    private int _lastFrameCount = -1;
    private int _lastFinishedCount;
    private DateTime _nextReconnectAttempt = DateTime.MinValue;
    private bool _everConnected;
    private volatile bool _suspended;
    private DateTime _lastResume = DateTime.MinValue;

    private NativeMethods.JlState _state = NativeMethods.JlState.Disconnected;
    private bool _connected;
    private string _port = "";
    private string _message = "";
    private string _error = "";
    private double _fps;
    private double _positionSeconds;
    private double _durationSeconds;
    private long _framesSent;
    private long _framesDropped;
    private BitmapSource? _preview;
    private MediaItem? _current;

    public DisplayService(AppSettings settings)
    {
        _settings = settings;

        _poll = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(PollMs),
        };
        _poll.Tick += (_, _) => Poll();

        RegisterPowerNotifications();
    }

    /// <summary>Raised when an item reached its own end — a video running out.</summary>
    public event EventHandler? ItemFinished;

    // -----------------------------------------------------------------------
    // Bindable state
    // -----------------------------------------------------------------------

    public NativeMethods.JlState State
    {
        get => _state;
        private set
        {
            if (Set(ref _state, value)) Raise(nameof(StateText));
        }
    }

    public bool Connected
    {
        get => _connected;
        private set
        {
            if (Set(ref _connected, value)) Raise(nameof(StateText));
        }
    }

    public string Port
    {
        get => _port;
        private set => Set(ref _port, value);
    }

    public string Message
    {
        get => _message;
        private set => Set(ref _message, value);
    }

    public string Error
    {
        get => _error;
        private set
        {
            if (Set(ref _error, value)) Raise(nameof(HasError));
        }
    }

    public bool HasError => !string.IsNullOrEmpty(_error);

    public double Fps
    {
        get => _fps;
        private set => Set(ref _fps, value);
    }

    /// <summary>Where the playing video has reached, in seconds.</summary>
    public double PositionSeconds
    {
        get => _positionSeconds;
        private set => Set(ref _positionSeconds, value);
    }

    /// <summary>
    /// How long the playing video runs, or 0 when that is not known — ffprobe
    /// is optional, and a still has no length.
    /// </summary>
    public double DurationSeconds
    {
        get => _durationSeconds;
        private set => Set(ref _durationSeconds, value);
    }

    /// <summary>True when there is a video playing that can be seeked in.</summary>
    public bool CanSeek =>
        State == NativeMethods.JlState.Playing && DurationSeconds > 0;

    /// <summary>
    /// Moves the playing video. Takes effect at the next frame rather than
    /// immediately, so calling it repeatedly while dragging is cheap.
    /// </summary>
    public void Seek(double seconds)
    {
        if (seconds < 0) seconds = 0;
        NativeMethods.jl_seek(seconds);
    }

    public long FramesSent
    {
        get => _framesSent;
        private set => Set(ref _framesSent, value);
    }

    public long FramesDropped
    {
        get => _framesDropped;
        private set => Set(ref _framesDropped, value);
    }

    /// <summary>The last frame actually sent to the panel — a true preview.</summary>
    public BitmapSource? Preview
    {
        get => _preview;
        private set => Set(ref _preview, value);
    }

    public MediaItem? Current
    {
        get => _current;
        private set
        {
            if (Set(ref _current, value)) Raise(nameof(StateText));
        }
    }

    public string StateText => State switch
    {
        NativeMethods.JlState.Disconnected => "Not connected",
        NativeMethods.JlState.Idle => Connected ? $"Connected on {Port} — idle" : "Not connected",
        NativeMethods.JlState.Preparing => $"Preparing {Current?.Name}…",
        NativeMethods.JlState.Calibrating => $"Calibrating {Current?.Name}…",
        NativeMethods.JlState.Preprocessing => $"Preprocessing {Current?.Name}…",
        NativeMethods.JlState.Playing => Current is null ? "Playing" : $"Showing {Current.Name}",
        NativeMethods.JlState.Error => "Error",
        _ => "",
    };

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    public void Start()
    {
        NativeMethods.VerifyLayout();
        ApplyPreprocess();
        _poll.Start();
        Connect();
    }

    /// <summary>
    /// Pushes the preprocessing mode and both budgets down to the native
    /// session. Takes effect on the next item started, so changing any of them
    /// never disturbs what is playing.
    /// </summary>
    public void ApplyPreprocess()
    {
        NativeMethods.jl_set_preprocess((int)_settings.Preprocess);
        NativeMethods.jl_set_pack_budgets(
            _settings.MemoryBudgetMB * 1024L * 1024L,
            _settings.DiskBudgetMB * 1024L * 1024L);
    }

    /// <summary>Bytes held by the on-disk pack cache.</summary>
    public static long PackCacheBytes() => NativeMethods.jl_pack_cache_bytes();

    /// <summary>
    /// Empties the pack cache. Safe while something is playing — a pack already
    /// mapped runs to the end of its item.
    /// </summary>
    public static void ClearPackCache() => NativeMethods.jl_pack_cache_clear();

    /// <summary>
    /// Opens the port. Failure is not exceptional — the panel is a USB device
    /// that may simply be unplugged — so it reports through <see cref="Error"/>
    /// and lets the reconnect loop keep trying.
    /// </summary>
    public bool Connect()
    {
        string? port = string.IsNullOrWhiteSpace(_settings.Port) ? null : _settings.Port;

        if (NativeMethods.jl_open(port) == NativeMethods.JL_OK)
        {
            _everConnected = true;
            Poll();
            ApplyBrightness();
            Storage.Log($"connected on {Port}");
            return true;
        }

        Poll();
        return false;
    }

    public void Disconnect()
    {
        Current = null;
        NativeMethods.jl_close();
        Preview = null;
        Poll();
    }

    /// <summary>
    /// Releases the device so another program — the CLI, or the vendor app —
    /// can take it. Suppresses reconnection until the user asks for it back.
    /// </summary>
    public void ReleaseDevice()
    {
        _settings.AutoReconnect = false;
        Disconnect();
        Storage.Log("device released at the user's request");
    }

    // -----------------------------------------------------------------------
    // Content
    // -----------------------------------------------------------------------

    /// <summary>
    /// Puts an item on the panel. Returns immediately: the native side spins up
    /// a worker and the poll loop reports what happens next.
    /// </summary>
    /// <param name="forceNoLoop">
    /// Set by the playlist for videos, which must hand over rather than repeat.
    /// </param>
    public bool Play(MediaItem item, bool forceNoLoop = false)
    {
        if (!Connected) return false;

        if (!File.Exists(item.Path))
        {
            Error = $"{item.Name} is missing from disk";
            return false;
        }

        var opts = BuildOpts(item, forceNoLoop);
        Current = item;

        int rc = item.IsVideo
            ? NativeMethods.jl_play_video(item.Path, ref opts)
            : NativeMethods.jl_show_image(item.Path, ref opts);

        if (rc != NativeMethods.JL_OK)
        {
            Current = null;
            Poll();
            return false;
        }

        Poll();
        return true;
    }

    /// <summary>
    /// Holds a still that is already in the panel's own format, bypassing the
    /// library entirely — used by the music follower for the card's backdrop.
    ///
    /// Rotation is deliberately 0. A 960x480 JPEG under the cap is sent as-is
    /// with no rotation asked for, which is what lets the card work on a machine
    /// with no ffmpeg at all; and the only thing drawn here is a blur, which has
    /// no orientation worth preserving. Everything legible on the card is the
    /// overlay's, and the overlay rotates itself.
    /// </summary>
    public bool ShowStill(string path)
    {
        if (!Connected) return false;
        if (!File.Exists(path)) return false;

        var opts = new NativeMethods.JlRenderOpts
        {
            Stretch = 1,
            Rotate = 0,
            Fps = _settings.Fps,
            Quality = 0,
            Loop = 0,
            Recalibrate = 0,
            PerFrameQuality = _settings.PerFrameQuality ? 1 : 0,
            QualityCeiling = _settings.QualityCeiling,
            ByteRate = (uint)(_settings.ByteRateKBs * 1024),
            Hwaccel = _settings.Hwaccel,
        };

        // Nothing in the library is showing, and saying so matters: the app's
        // reconnect handler puts Current back, and doing that would fight the
        // follower for the panel.
        Current = null;

        int rc = NativeMethods.jl_show_image(path, ref opts);
        Poll();
        return rc == NativeMethods.JL_OK;
    }

    /// <summary>
    /// Streams a live URL onto the panel — the music follower's video path.
    ///
    /// Rotation and stretch come from the settings as they do for a video,
    /// because unlike the card's backdrop this is a picture with a right way up.
    /// </summary>
    public bool PlayStream(string url, string inputArgs, double startSeconds, double duration)
    {
        if (!Connected) return false;
        if (string.IsNullOrWhiteSpace(url)) return false;

        var opts = new NativeMethods.JlRenderOpts
        {
            Stretch = _settings.Stretch ? 1 : 0,
            Rotate = _settings.Rotate,
            Fps = _settings.Fps,
            Quality = 0,
            Loop = 0,
            Recalibrate = 0,
            PerFrameQuality = 0,
            QualityCeiling = _settings.QualityCeiling,
            ByteRate = (uint)(_settings.ByteRateKBs * 1024),
            Hwaccel = _settings.Hwaccel,
        };

        Current = null;

        int rc = NativeMethods.jl_play_stream(url, inputArgs ?? "", startSeconds, duration,
            ref opts);
        Poll();
        return rc == NativeMethods.JL_OK;
    }

    public void Stop()
    {
        Current = null;
        NativeMethods.jl_stop();
        Poll();
    }

    public void ApplyBrightness()
    {
        if (Connected) NativeMethods.jl_set_brightness(_settings.Brightness);
    }

    public NativeMethods.JlRenderOpts BuildOpts(MediaItem item, bool forceNoLoop = false)
    {
        return new NativeMethods.JlRenderOpts
        {
            Stretch = (item.Stretch ?? _settings.Stretch) ? 1 : 0,
            Rotate = item.Rotate ?? _settings.Rotate,
            Fps = _settings.Fps,
            Quality = 0,                       // always let the core decide
            Loop = (item.Loop && !forceNoLoop) ? 1 : 0,
            Recalibrate = 0,
            PerFrameQuality = _settings.PerFrameQuality ? 1 : 0,
            QualityCeiling = _settings.QualityCeiling,
            ByteRate = (uint)(_settings.ByteRateKBs * 1024),
            Hwaccel = _settings.Hwaccel,
        };
    }

    public string? DeviceInfo()
    {
        if (!Connected) return null;
        var sb = new StringBuilder(2048);
        return NativeMethods.jl_get_info(sb, sb.Capacity) == NativeMethods.JL_OK
            ? sb.ToString()
            : null;
    }

    public static string? FindFfmpeg()
    {
        var sb = new StringBuilder(512);
        return NativeMethods.jl_ffmpeg_path(sb, sb.Capacity) == NativeMethods.JL_OK
            ? sb.ToString()
            : null;
    }

    // -----------------------------------------------------------------------
    // Polling
    // -----------------------------------------------------------------------

    private void Poll()
    {
        NativeMethods.jl_get_status(out var s);

        State = (NativeMethods.JlState)s.State;
        Connected = s.Connected != 0;
        Port = s.Port;
        Message = s.Message;
        Error = s.Error;
        Fps = s.Fps;
        FramesSent = s.FramesSent;
        FramesDropped = s.FramesDropped;
        PositionSeconds = s.PositionSeconds;
        DurationSeconds = s.DurationSeconds;
        Raise(nameof(CanSeek));

        if (s.FrameCount != _lastFrameCount)
        {
            _lastFrameCount = s.FrameCount;
            RefreshPreview();
        }

        // A counter rather than a flag, so an item that starts and finishes
        // between two polls still registers exactly once.
        if (s.FinishedCount != _lastFinishedCount)
        {
            _lastFinishedCount = s.FinishedCount;
            ItemFinished?.Invoke(this, EventArgs.Empty);
        }

        if (!Connected) MaybeReconnect();
    }

    // -----------------------------------------------------------------------
    // Sleep and wake
    //
    // The panel is a USB device on the far side of a power transition, and what
    // goes wrong there is specifically the thing the reconnect loop cannot see.
    //
    // That loop only runs when Connected goes false, and Connected only goes
    // false when a write fails. After a wake there are two ways for no write to
    // ever fail. If nothing was playing there are no writes at all, so nothing
    // notices the handle is stale. And if something was playing, the panel may
    // come back with its COM port still present and the handle still valid,
    // having reset itself while the host slept: the writes keep succeeding, the
    // host reports a clean connection at 30 fps, and the picture never moves.
    // That second one is the same blindness that makes the data rate impossible
    // to measure from here — the panel accepts everything and says nothing.
    //
    // So this does not try to detect anything. It closes the port on the way
    // down and again on the way up, which forces the next open to redo
    // Device::Clear(), live mode and brightness, and lets the ordinary
    // reconnect loop do the retrying it is already good at.
    // -----------------------------------------------------------------------

    // PowerRegisterSuspendResumeNotification rather than
    // SystemEvents.PowerModeChanged, because .NET does not map
    // PBT_APMRESUMEAUTOMATIC onto PowerModes.Resume. Windows sends that one for
    // every wake; PBT_APMRESUMESUSPEND only follows when a person woke the
    // machine. Listening to SystemEvents alone would fix the case where someone
    // presses a key and miss the machine waking itself for maintenance — which
    // looks identical when you come back to it hours later.
    private const int PBT_APMSUSPEND = 0x0004;
    private const int PBT_APMRESUMESUSPEND = 0x0007;
    private const int PBT_APMRESUMEAUTOMATIC = 0x0012;
    private const int DEVICE_NOTIFY_CALLBACK = 2;

    private delegate int DeviceNotifyCallback(IntPtr context, int type, IntPtr setting);

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceNotifySubscribeParameters
    {
        public IntPtr Callback;
        public IntPtr Context;
    }

    [DllImport("powrprof.dll")]
    private static extern uint PowerRegisterSuspendResumeNotification(
        int flags, ref DeviceNotifySubscribeParameters parameters, out IntPtr handle);

    [DllImport("powrprof.dll")]
    private static extern uint PowerUnregisterSuspendResumeNotification(IntPtr handle);

    // Held for the life of the registration: the OS keeps the raw pointer, and
    // a collected delegate is a crash the moment the machine sleeps.
    private DeviceNotifyCallback? _powerCallback;
    private IntPtr _powerNotify;

    private void RegisterPowerNotifications()
    {
        _powerCallback = OnPowerEvent;

        var p = new DeviceNotifySubscribeParameters
        {
            Callback = Marshal.GetFunctionPointerForDelegate(_powerCallback),
            Context = IntPtr.Zero,
        };

        if (PowerRegisterSuspendResumeNotification(
                DEVICE_NOTIFY_CALLBACK, ref p, out _powerNotify) != 0)
        {
            _powerNotify = IntPtr.Zero;
            _powerCallback = null;
            Storage.Log("could not subscribe to sleep and wake notifications");
        }
    }

    private int OnPowerEvent(IntPtr context, int type, IntPtr setting)
    {
        switch (type)
        {
            case PBT_APMSUSPEND:
                // Done inline rather than through the dispatcher: the native
                // session takes its own lock, and Windows gives a suspend
                // handler only a couple of seconds — enough to close the port,
                // not enough to also wait for the UI thread to be free.
                _suspended = true;
                NativeMethods.jl_close();
                Storage.Log("suspending: released the panel");
                _poll.Dispatcher.BeginInvoke(Poll);
                break;

            case PBT_APMRESUMESUSPEND:
            case PBT_APMRESUMEAUTOMATIC:
                // Both usually arrive for a single wake. WakeUp is idempotent,
                // but a second pass would still restart the settle timer and
                // log again, so collapse them on time rather than on having
                // seen a suspend — a resume without one has to still work.
                DateTime now = DateTime.UtcNow;
                if (now - _lastResume < TimeSpan.FromSeconds(5)) break;
                _lastResume = now;

                _suspended = false;
                _poll.Dispatcher.BeginInvoke(WakeUp);
                break;
        }

        return 0;   // ERROR_SUCCESS
    }

    /// <summary>
    /// Puts the connection back after a wake. Closes first even though the
    /// suspend handler usually did: hibernate and fast startup do not reliably
    /// deliver Suspend, and reopening is the only way to be sure the panel gets
    /// its initialisation sequence again.
    /// </summary>
    private void WakeUp()
    {
        _suspended = false;

        NativeMethods.jl_close();
        _nextReconnectAttempt = DateTime.UtcNow.AddMilliseconds(ResumeSettleMs);

        Storage.Log(_settings.AutoReconnect
            ? "resumed: reconnecting to the panel"
            : "resumed: not reconnecting, the device is released");

        Poll();
    }

    private void MaybeReconnect()
    {
        // Between the suspend handler and the machine actually going down the
        // poll timer can still tick, and reopening the port there would undo
        // the close moments before sleep.
        if (_suspended) return;

        if (!_settings.AutoReconnect) return;
        if (DateTime.UtcNow < _nextReconnectAttempt) return;

        _nextReconnectAttempt = DateTime.UtcNow.AddMilliseconds(ReconnectMs);

        // Checking for the port first keeps the common unplugged case quiet:
        // opening a port that is not there would set an error every two seconds.
        var sb = new StringBuilder(64);
        if (NativeMethods.jl_find_port(sb, sb.Capacity) != NativeMethods.JL_OK) return;

        if (NativeMethods.jl_open(null) == NativeMethods.JL_OK)
        {
            _everConnected = true;
            ApplyBrightness();
            Storage.Log("reconnected");
            Reconnected?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <summary>Raised after the reconnect loop gets the device back.</summary>
    public event EventHandler? Reconnected;

    public bool EverConnected => _everConnected;

    private void RefreshPreview()
    {
        int size = NativeMethods.jl_get_last_frame(null, 0);
        if (size <= 0)
        {
            Preview = null;
            return;
        }

        var buffer = new byte[size];
        int got = NativeMethods.jl_get_last_frame(buffer, size);
        if (got <= 0 || got > size) return;

        try
        {
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.StreamSource = new MemoryStream(buffer, 0, got, writable: false);
            bmp.CacheOption = BitmapCacheOption.OnLoad;   // decode now, release the stream
            bmp.EndInit();
            bmp.Freeze();                                  // usable from any thread
            Preview = bmp;
        }
        catch
        {
            // A torn frame is not worth reporting; the next one is 200 ms away.
        }
    }

    public void Dispose()
    {
        // The OS holds a raw pointer to the callback until this returns, so
        // unregistering has to happen before the delegate can be collected.
        if (_powerNotify != IntPtr.Zero) {
            PowerUnregisterSuspendResumeNotification(_powerNotify);
            _powerNotify = IntPtr.Zero;
        }
        _powerCallback = null;

        _poll.Stop();
        NativeMethods.jl_close();
    }

    // -----------------------------------------------------------------------

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }

    private void Raise(string name) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
