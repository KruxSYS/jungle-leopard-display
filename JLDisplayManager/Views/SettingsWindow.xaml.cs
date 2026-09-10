using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

using JLDisplayManager.Models;
using JLDisplayManager.Services;
using JLDisplayManager.Services.Music;

using MessageBox = System.Windows.MessageBox;

namespace JLDisplayManager.Views;

public partial class SettingsWindow : Window
{
    private static readonly string[] HwaccelValues =
        { "auto", "none", "cuda", "qsv", "d3d11va", "dxva2" };

    // In the order the two music cache combos list them. 0 days means "no age
    // limit", which is why it sits last rather than first.
    private static readonly int[] MusicDaysValues = { 3, 7, 14, 30, 90, 0 };
    private static readonly int[] MusicCacheValues = { 512, 1024, 2048, 4096, 8192, 16384 };

    /// <summary>
    /// The dark ink the other combo boxes set inline in XAML. Items built in
    /// code have to say it too, or they inherit the window's light foreground
    /// and vanish against the dropdown's background.
    /// </summary>
    private static readonly Brush ComboText = FrozenInk();

    private static Brush FrozenInk()
    {
        var brush = new SolidColorBrush(Color.FromRgb(0x1A, 0x12, 0x07));
        brush.Freeze();   // shared across every item, so it must not stay mutable
        return brush;
    }

    private readonly App _app = App.Current;
    private bool _loaded;

    public SettingsWindow()
    {
        InitializeComponent();
        Load();
        _loaded = true;
    }

    private void Load()
    {
        var s = _app.Settings;

        RotateBox.SelectedIndex = s.Rotate / 90;
        StretchBox.IsChecked = s.Stretch;
        FpsSlider.Value = s.Fps;
        FpsValue.Text = s.Fps.ToString();

        int hw = Array.IndexOf(HwaccelValues, s.Hwaccel);
        HwaccelBox.SelectedIndex = hw >= 0 ? hw : 0;

        PreprocessBox.SelectedIndex = (int)s.Preprocess;
        PerFrameBox.IsChecked = s.PerFrameQuality;
        SelectByteRate(s.ByteRateKBs);
        ShowPreprocessHint();

        // Read the real registered state rather than the stored flag: the task
        // can be removed from outside the app, and a checkbox that disagrees
        // with Task Scheduler is worse than no checkbox.
        StartWithWindowsBox.IsChecked = StartupTask.IsRegistered();

        StartMinimisedBox.IsChecked = s.StartMinimised;
        ResumeBox.IsChecked = s.ResumeOnStart;

        // The same setting the AI window offers; it lives here as well because
        // this is where someone looks for what happens at startup.
        StartAiBox.IsChecked = _app.Ai.StartWithApp;
        StartAiHint.Text = _app.Ai.Prompts.Count == 0
            ? "Nothing to generate from yet — add prompts under AI first."
            : "Takes precedence over putting back what was showing.";

        AutoReconnectBox.IsChecked = s.AutoReconnect;
        BlankOnExitBox.IsChecked = s.BlankOnExit;
        PortBox.Text = s.Port;

        LhmBox.IsChecked = s.UseLibreHardwareMonitor;
        HwInfoBox.IsChecked = s.UseHwInfo;

        MediaSessionBox.IsChecked = s.UseMediaSession;
        MusicVideoBox.IsChecked = s.MusicVideo;
        FillMediaApps(s.MediaSessionApp);

        MusicDaysCombo.SelectedIndex = NearestIndex(MusicDaysValues, s.MusicCacheDays);
        MusicCacheCombo.SelectedIndex = NearestIndex(MusicCacheValues, s.MusicCacheMB);
        ShowMusicCacheSize();

        ShowSensorState();
        ShowDeviceInfo();
        ShowFfmpegState();
    }

    private void ShowDeviceInfo()
    {
        string? json = _app.Display.DeviceInfo();
        if (json is null)
        {
            DeviceInfoText.Text = "Not connected.";
            return;
        }

        try
        {
            // The panel answers with {"cmd":"info","data":{…}}; only the inner
            // object is worth showing, and only a few fields of it.
            using var document = JsonDocument.Parse(json);
            var data = document.RootElement.GetProperty("data");

            string model = data.TryGetProperty("model", out var m) ? m.GetString() ?? "?" : "?";
            string version = data.TryGetProperty("version", out var v) ? v.GetString() ?? "?" : "?";
            string uid = data.TryGetProperty("uid", out var u) ? u.GetString() ?? "?" : "?";
            int width = data.TryGetProperty("width", out var w) ? w.GetInt32() : 0;
            int height = data.TryGetProperty("height", out var h) ? h.GetInt32() : 0;

            DeviceInfoText.Text =
                $"{model}  ·  {width}×{height}  ·  firmware {version}\n" +
                $"serial {uid}  on  {_app.Display.Port}";
        }
        catch
        {
            DeviceInfoText.Text = $"Connected on {_app.Display.Port}.";
        }
    }

    private void ShowFfmpegState()
    {
        string? path = DisplayService.FindFfmpeg();
        FfmpegText.Text = path is null
            ? "ffmpeg was not found. Install it with:\n" +
              "    winget install \"FFmpeg (Essentials Build)\"\n" +
              "or put ffmpeg.exe beside this program. Without it only images that " +
              "are already 960×480 JPEG under 80 KB can be shown."
            : $"ffmpeg: {path}";
    }

    /// <summary>
    /// Roughly what a megabyte of preprocessed frames is worth in playing time.
    /// At one quality for the whole file a 960x480 frame lands around 40 KB, so
    /// 30 fps costs about 70 MB a minute. A per-frame build spends exactly the
    /// data rate it was given, which makes the same figure arithmetic rather
    /// than an estimate.
    /// </summary>
    private double MegabytesPerVideoMinute =>
        _app.Settings.PerFrameQuality
            ? _app.Settings.ByteRateKBs * 60.0 / 1024.0
            : 70.0;

    /// <summary>
    /// The data rates offered, in KB/s. 1150 is what one -q:v 3 frame a 30th of
    /// a second comes to, which is what the app has always sent.
    /// <para>
    /// The two below it exist because 1150 is not a floor, only a default: it
    /// was measured on one panel with one kind of content, and dense footage —
    /// foliage, gravel, anything without a flat region — is what makes a panel's
    /// decoder run out. There is nowhere to go but down when that happens, so
    /// there has to be a down.
    /// </para>
    /// </summary>
    private static readonly int[] ByteRateValues = { 700, 900, 1150, 1400, 1900 };

    private static readonly int[] MemoryPresetsMB = { 128, 256, 512, 1024, 2048, 4096 };

    private static readonly int[] DiskPresetsMB =
        { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

    /// <summary>Suppresses the change handler while the list is being rebuilt.</summary>
    private bool _populatingBudgets;

    private static string FormatSize(int megabytes) =>
        megabytes >= 1024 ? $"{megabytes / 1024} GB" : $"{megabytes} MB";

    private string PlayingTime(int megabytes)
    {
        double minutes = megabytes / MegabytesPerVideoMinute;
        if (minutes < 1) return "under a minute";
        if (minutes < 90) return $"about {Math.Round(minutes)} minutes";
        return $"about {minutes / 60:0.#} hours";
    }

    /// <summary>
    /// Rebuilds the limit list for the selected mode. Only one limit is ever
    /// relevant — Off has none and the other two never apply at once — so this
    /// is one control that changes meaning rather than two that sit half-unused.
    /// </summary>
    private void PopulateBudgets(PreprocessMode mode)
    {
        if (mode == PreprocessMode.Off)
        {
            BudgetRow.Visibility = Visibility.Collapsed;
            return;
        }

        bool disk = mode == PreprocessMode.Disk;

        BudgetLabel.Text = disk ? "Disk limit" : "Memory limit";
        BudgetRow.Visibility = Visibility.Visible;

        int current = disk ? _app.Settings.DiskBudgetMB : _app.Settings.MemoryBudgetMB;

        // A value hand-edited into settings.json is kept as an option of its
        // own rather than silently snapped to the nearest preset.
        var choices = new List<int>(disk ? DiskPresetsMB : MemoryPresetsMB);
        if (!choices.Contains(current))
        {
            choices.Add(current);
            choices.Sort();
        }

        _populatingBudgets = true;
        BudgetBox.Items.Clear();

        foreach (int mb in choices)
        {
            BudgetBox.Items.Add(new ComboBoxItem
            {
                Content = new TextBlock
                {
                    Text = $"{FormatSize(mb)} — {PlayingTime(mb)} of video",
                    Foreground = ComboText,
                },
            });
        }

        BudgetBox.SelectedIndex = choices.IndexOf(current);
        _budgetChoices = choices;
        _populatingBudgets = false;
    }

    private List<int> _budgetChoices = new();

    /// <summary>
    /// Says what the selected mode actually costs. The frame-rate caveat is
    /// worth stating out loud: the rate is part of what a preprocessed frame is,
    /// so changing it throws every stored one away.
    /// </summary>
    private void ShowPreprocessHint()
    {
        if (PreprocessHint is null) return;

        var mode = (PreprocessMode)Math.Max(0, PreprocessBox.SelectedIndex);

        PopulateBudgets(mode);

        bool preparing = mode != PreprocessMode.Off;
        bool perFrame = preparing && _app.Settings.PerFrameQuality;

        // Per-frame quality is a property of prepared frames, so it has nothing
        // to say while nothing is being prepared.
        PerFrameBox.Visibility = preparing ? Visibility.Visible : Visibility.Collapsed;
        ByteRateRow.Visibility = perFrame ? Visibility.Visible : Visibility.Collapsed;

        int mbPerMinute = (int)MegabytesPerVideoMinute;

        PreprocessHint.Text = mode switch
        {
            PreprocessMode.Memory =>
                "ffmpeg runs once per item and then stops. Frames are held in memory and " +
                "rebuilt each time the app starts. Anything that would not fit the limit " +
                "falls back to streaming on its own, so raising it only widens what " +
                "benefits — it never decides what will play.",
            PreprocessMode.Disk =>
                $"ffmpeg runs once per item, ever. Roughly {mbPerMinute} MB per minute of " +
                "video, evicted least-recently-used once the limit is reached. Changing " +
                "rotation, stretch or frame rate rebuilds them.",
            _ =>
                "ffmpeg runs for as long as something is on the panel.",
        };

        if (perFrame)
        {
            PreprocessHint.Text +=
                " Each frame is encoded at the best quality that fits the data rate " +
                "below, rather than at the one setting the busiest frame in the whole " +
                "file allows. At the default that is the same rate the panel has " +
                "always been sent, for a visibly better picture; raising it is what " +
                "makes playback stutter. Items already prepared keep what they were " +
                "built with until they are built again.";
        }

        PackCachePanel.Visibility = mode == PreprocessMode.Disk
            ? Visibility.Visible
            : Visibility.Collapsed;

        if (mode == PreprocessMode.Disk) ShowPackCacheSize();
    }

    private void ShowPackCacheSize()
    {
        long bytes = DisplayService.PackCacheBytes();
        string used = bytes < 1024L * 1024
            ? "empty"
            : $"{bytes / (1024.0 * 1024 * 1024):0.00} GB";

        PackCacheText.Text = $"Cache: {used} of {FormatSize(_app.Settings.DiskBudgetMB)}";
    }

    private void OnPreprocessChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded) return;

        // Applied as soon as it is picked rather than on close: the next item to
        // start should already use it, and the hint below is about to describe
        // behaviour the session is not yet in otherwise.
        _app.Settings.Preprocess = (PreprocessMode)Math.Max(0, PreprocessBox.SelectedIndex);
        _app.Display.ApplyPreprocess();
        ShowPreprocessHint();
    }

    private void SelectByteRate(int kbs)
    {
        int index = Array.IndexOf(ByteRateValues, kbs);
        if (index < 0) index = 0;
        ByteRateBox.SelectedIndex = index;
    }

    private void OnPerFrameChanged(object sender, RoutedEventArgs e)
    {
        if (!_loaded) return;

        _app.Settings.PerFrameQuality = PerFrameBox.IsChecked == true;

        // Nothing is pushed down here: it rides on JlRenderOpts with the next
        // item rather than on the session. What does need saying is that the
        // packs already on disk were built the other way and stay that way
        // until they are rebuilt — which ShowPreprocessHint covers.
        ShowPreprocessHint();
    }

    private void OnByteRateChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded) return;

        int index = ByteRateBox.SelectedIndex;
        if (index < 0 || index >= ByteRateValues.Length) return;

        _app.Settings.ByteRateKBs = ByteRateValues[index];
        ShowPreprocessHint();
    }

    private void OnBudgetChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded || _populatingBudgets) return;

        int index = BudgetBox.SelectedIndex;
        if (index < 0 || index >= _budgetChoices.Count) return;

        if (_app.Settings.Preprocess == PreprocessMode.Disk)
        {
            _app.Settings.DiskBudgetMB = _budgetChoices[index];
            ShowPackCacheSize();

            // Lowering the limit does not reclaim anything until the next pack
            // is written, so say so rather than leaving a figure that looks
            // like it is over the limit and being ignored.
            if (DisplayService.PackCacheBytes() > _app.Settings.DiskBudgetMB * 1024L * 1024L)
                PackCacheText.Text += " — trimmed as new items are added";
        }
        else
        {
            _app.Settings.MemoryBudgetMB = _budgetChoices[index];
        }

        _app.Display.ApplyPreprocess();
    }

    private void OnClearPackCache(object sender, RoutedEventArgs e)
    {
        DisplayService.ClearPackCache();
        ShowPackCacheSize();
    }

    private void OnFpsChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        // The slider's Minimum of 1 coerces its default value of 0 during XAML
        // parsing, which raises this before the rest of the tree exists. The
        // named fields are still null at that point.
        if (FpsValue is null) return;

        int value = (int)Math.Round(e.NewValue);
        FpsValue.Text = value.ToString();
        if (_loaded) _app.Settings.Fps = value;
    }

    private void OnStartWithWindowsChanged(object sender, RoutedEventArgs e)
    {
        bool wanted = StartWithWindowsBox.IsChecked == true;

        if (!StartupTask.Apply(wanted, out string error))
        {
            MessageBox.Show(this,
                $"Could not {(wanted ? "create" : "remove")} the startup task.\n\n{error}",
                "Startup", MessageBoxButton.OK, MessageBoxImage.Warning);

            // Snap back to whatever is actually registered, so the box never
            // claims something that is not true.
            StartWithWindowsBox.IsChecked = StartupTask.IsRegistered();
        }

        _app.Settings.StartWithWindows = StartWithWindowsBox.IsChecked == true;
    }

    // -----------------------------------------------------------------------
    // Sensors
    // -----------------------------------------------------------------------

    private void OnSensorSourceChanged(object sender, RoutedEventArgs e)
    {
        if (!_loaded) return;

        _app.Settings.UseLibreHardwareMonitor = LhmBox.IsChecked == true;
        _app.Settings.UseHwInfo = HwInfoBox.IsChecked == true;
        ShowSensorState();
    }

    // -----------------------------------------------------------------------
    // Music
    // -----------------------------------------------------------------------

    private void OnMusicChanged(object sender, RoutedEventArgs e)
    {
        if (!_loaded) return;

        _app.Settings.UseMediaSession = MediaSessionBox.IsChecked == true;
        _app.Settings.MusicVideo = MusicVideoBox.IsChecked == true;
    }

    private void OnMusicCacheSettingChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded) return;

        if (MusicDaysCombo.SelectedIndex >= 0)
            _app.Settings.MusicCacheDays = MusicDaysValues[MusicDaysCombo.SelectedIndex];

        if (MusicCacheCombo.SelectedIndex >= 0)
            _app.Settings.MusicCacheMB = MusicCacheValues[MusicCacheCombo.SelectedIndex];

        // Pushed down straight away so "Sweep now" uses what is on screen
        // rather than what was set when the follower last started.
        MusicVideoCache.Configure(_app.Settings.MusicCacheMB, _app.Settings.MusicCacheDays);
        ShowMusicCacheSize();
    }

    private void OnClearMusicCache(object sender, RoutedEventArgs e)
    {
        MusicVideoCache.Clear();
        ShowMusicCacheSize();
    }

    private void OnSweepMusicCache(object sender, RoutedEventArgs e)
    {
        MusicVideoCache.Configure(_app.Settings.MusicCacheMB, _app.Settings.MusicCacheDays);
        MusicVideoCache.Sweep();
        ShowMusicCacheSize();
    }

    private void ShowMusicCacheSize()
    {
        long bytes = MusicVideoCache.Bytes();
        string used = bytes < 1024L * 1024
            ? "empty"
            : $"{bytes / (1024.0 * 1024):0} MB";

        int days = _app.Settings.MusicCacheDays;
        string kept = days == 0 ? "no age limit" : days == 1 ? "1 day" : $"{days} days";

        MusicCacheText.Text =
            $"Downloaded videos: {used} of {FormatSize(_app.Settings.MusicCacheMB)}, {kept}";
    }

    /// <summary>
    /// The listed value closest to a saved one, so a figure hand-edited into
    /// settings.json still selects something sensible instead of nothing.
    /// </summary>
    private static int NearestIndex(int[] values, int value)
    {
        int best = 0;
        for (int i = 1; i < values.Length; i++)
        {
            // 0 means "no limit" and is never nearest to a real figure; it is
            // only chosen when the saved value is 0 too.
            if (values[i] == 0) continue;
            if (value == 0) return Array.IndexOf(values, 0);

            if (Math.Abs(values[i] - value) < Math.Abs(values[best] - value)) best = i;
        }

        return value == 0 ? Array.IndexOf(values, 0) : best;
    }

    private void OnMediaAppChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded) return;
        if (MediaAppCombo.SelectedItem is not ComboBoxItem item) return;

        _app.Settings.MediaSessionApp = item.Tag as string ?? "";
        _app.Music?.Repoint();
    }

    /// <summary>
    /// Lists the players Windows currently knows about.
    ///
    /// By app id, because that is the identity the follower matches on, and it
    /// is frequently not a name anybody would recognise — Firefox reports
    /// itself as "308046B0AF4A39CB". The friendly name is what gets shown; the
    /// id rides along as the tag. A player that is saved but not running right
    /// now is listed too, or picking it would silently reset to automatic.
    /// </summary>
    private void FillMediaApps(string saved)
    {
        MediaAppCombo.Items.Clear();
        MediaAppCombo.Items.Add(Entry("Automatic", ""));

        var seen = new List<string>();
        foreach (string app in MediaSession.Sessions())
        {
            if (string.IsNullOrWhiteSpace(app) || seen.Contains(app)) continue;
            seen.Add(app);
            MediaAppCombo.Items.Add(Entry(MediaSession.FriendlyName(app), app));
        }

        if (saved.Length > 0 && !seen.Contains(saved))
            MediaAppCombo.Items.Add(Entry(MediaSession.FriendlyName(saved) + " (not running)", saved));

        foreach (object o in MediaAppCombo.Items)
        {
            if (o is ComboBoxItem c && (c.Tag as string ?? "") == saved)
            {
                MediaAppCombo.SelectedItem = c;
                break;
            }
        }

        MediaAppCombo.SelectedItem ??= MediaAppCombo.Items[0];

        // The same trick the rest of the combos use: an item's generated
        // TextBlock picks up the app's implicit style and renders invisible, so
        // the content is an explicit TextBlock with its own foreground.
        static ComboBoxItem Entry(string text, string tag) => new()
        {
            Content = new TextBlock
            {
                Text = text,
                Foreground = new SolidColorBrush(Color.FromRgb(0x1A, 0x12, 0x07)),
            },
            Tag = tag,
        };
    }

    /// <summary>
    /// Says which source is actually answering, and — the part that matters —
    /// says so plainly when none is. A CPU temperature layer that reads "--"
    /// looks like a broken app rather than a missing helper, and this is the one
    /// place that can explain the difference.
    /// </summary>
    private void ShowSensorState()
    {
        var registry = _app.Overlay?.Sensors;

        if (registry == null)
        {
            SensorStatusText.Text = "";
            return;
        }

        var live = new List<string>();
        foreach (Services.Sensors.ISensorProvider p in registry.Providers)
            if (p.Available && (p.Name.StartsWith("Libre") || p.Name.StartsWith("HWiNFO")))
                live.Add(p.Name);

        bool haveTemp = registry.Snapshot()["cpu.temp"].Available;

        if (haveTemp)
        {
            SensorStatusText.Foreground = (Brush)FindResource("TextDim");
            SensorStatusText.Text = $"Reading from {string.Join(" and ", live)}. "
                                    + "CPU temperature is available.";
            return;
        }

        SensorStatusText.Foreground = (Brush)FindResource("Accent");
        SensorStatusText.Text = live.Count > 0
            ? $"{string.Join(" and ", live)} is running but is not reporting a CPU "
              + "temperature yet."
            : "Neither is running, so CPU temperature, fan speeds and coolant "
              + "temperature are unavailable. Changes here take effect next launch.";
    }

    private void OnReleaseDevice(object sender, RoutedEventArgs e)
    {
        _app.Player.Stop();
        _app.Display.ReleaseDevice();
        AutoReconnectBox.IsChecked = false;
        ShowDeviceInfo();
    }

    private void OnOpenFolder(object sender, RoutedEventArgs e)
    {
        Storage.EnsureDirectories();
        Process.Start(new ProcessStartInfo(Storage.Directory) { UseShellExecute = true });
    }

    private void OnClose(object sender, RoutedEventArgs e)
    {
        Save();
        Close();
    }

    protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
    {
        Save();
        base.OnClosing(e);
    }

    private void Save()
    {
        var s = _app.Settings;

        s.Rotate = Math.Max(0, RotateBox.SelectedIndex) * 90;
        s.Stretch = StretchBox.IsChecked == true;
        s.Fps = (int)Math.Round(FpsSlider.Value);
        s.Hwaccel = HwaccelValues[Math.Max(0, HwaccelBox.SelectedIndex)];
        s.Preprocess = (PreprocessMode)Math.Max(0, PreprocessBox.SelectedIndex);
        s.StartMinimised = StartMinimisedBox.IsChecked == true;
        s.ResumeOnStart = ResumeBox.IsChecked == true;

        if (_app.Ai.StartWithApp != (StartAiBox.IsChecked == true))
        {
            _app.Ai.StartWithApp = StartAiBox.IsChecked == true;
            Storage.SaveAi(_app.Ai);
        }

        s.AutoReconnect = AutoReconnectBox.IsChecked == true;
        s.BlankOnExit = BlankOnExitBox.IsChecked == true;
        s.Port = PortBox.Text.Trim();

        s.UseLibreHardwareMonitor = LhmBox.IsChecked == true;
        s.UseHwInfo = HwInfoBox.IsChecked == true;

        s.UseMediaSession = MediaSessionBox.IsChecked == true;
        s.MusicVideo = MusicVideoBox.IsChecked == true;
        if (MusicDaysCombo.SelectedIndex >= 0)
            s.MusicCacheDays = MusicDaysValues[MusicDaysCombo.SelectedIndex];
        if (MusicCacheCombo.SelectedIndex >= 0)
            s.MusicCacheMB = MusicCacheValues[MusicCacheCombo.SelectedIndex];
        if (MediaAppCombo.SelectedItem is ComboBoxItem app) s.MediaSessionApp = app.Tag as string ?? "";

        Storage.SaveSettings(s);
    }
}
