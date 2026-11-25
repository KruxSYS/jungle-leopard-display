using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Forms = System.Windows.Forms;
using Drawing = System.Drawing;

namespace JLDisplayManager.Themes;

/// <summary>
/// Code-behind access to Themes/KruxSYS.xaml. Code never writes a colour down
/// either: it asks for a token by the same name the XAML uses.
/// </summary>
public static class Theme
{
    /// <summary>A brush token, e.g. "Accent", "TextMuted".</summary>
    public static Brush Brush(string key) => (Brush)Application.Current.FindResource(key);

    /// <summary>The colour behind a brush token ("Accent" → AccentColor).</summary>
    public static Color Color(string key) => (Color)Application.Current.FindResource(key + "Color");

    /// <summary>The same token for Windows Forms (the tray menu).</summary>
    public static Drawing.Color Gdi(string key)
    {
        Color c = Color(key);
        return Drawing.Color.FromArgb(c.A, c.R, c.G, c.B);
    }

    // ------------------------------------------------------------------
    // Title bars
    // ------------------------------------------------------------------

    /// <summary>
    /// Every window gets a dark caption in the page colour. Registered once,
    /// for the Window class, so a window added later is covered without
    /// anyone remembering to opt in.
    /// </summary>
    public static void HookWindows()
    {
        EventManager.RegisterClassHandler(typeof(Window), FrameworkElement.LoadedEvent,
            new RoutedEventHandler((s, _) => { if (s is Window w) DarkCaption(w); }));
    }

    private const int DwmUseImmersiveDarkMode = 20;  // Windows 10 20H1+
    private const int DwmBorderColor = 34;           // Windows 11
    private const int DwmCaptionColor = 35;
    private const int DwmTextColor = 36;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    private static int ColorRef(string key)
    {
        Color c = Color(key);
        return c.R | (c.G << 8) | (c.B << 16);
    }

    private static void DarkCaption(Window w)
    {
        IntPtr hwnd = new WindowInteropHelper(w).Handle;
        if (hwnd == IntPtr.Zero) return;

        // Each attribute fails harmlessly on a Windows that does not know it:
        // Windows 10 takes the dark mode and ignores the colours.
        int on = 1;
        DwmSetWindowAttribute(hwnd, DwmUseImmersiveDarkMode, ref on, sizeof(int));
        int caption = ColorRef("Bg"), text = ColorRef("Text"), border = ColorRef("Line");
        DwmSetWindowAttribute(hwnd, DwmCaptionColor, ref caption, sizeof(int));
        DwmSetWindowAttribute(hwnd, DwmTextColor, ref text, sizeof(int));
        DwmSetWindowAttribute(hwnd, DwmBorderColor, ref border, sizeof(int));
    }

    // ------------------------------------------------------------------
    // Tray menu
    // ------------------------------------------------------------------

    /// <summary>
    /// The tray menu is Windows Forms (NotifyIcon has no WPF equivalent), so it
    /// cannot use the XAML styles. This renderer paints it from the same tokens.
    /// </summary>
    public static Forms.ToolStripRenderer TrayRenderer() => new TrayMenuRenderer();

    private sealed class TrayMenuRenderer : Forms.ToolStripProfessionalRenderer
    {
        public TrayMenuRenderer() : base(new TrayColours()) { RoundedEdges = false; }

        protected override void OnRenderItemText(Forms.ToolStripItemTextRenderEventArgs e)
        {
            // A label's own ForeColor (the AI status turning red on an error)
            // wins; everything else is body text, muted when disabled.
            if (e.Item.ForeColor == Forms.Control.DefaultForeColor || e.Item.ForeColor == Drawing.SystemColors.ControlText)
                e.TextColor = e.Item.Enabled ? Gdi("Text") : Gdi("TextMuted");
            base.OnRenderItemText(e);
        }

        protected override void OnRenderArrow(Forms.ToolStripArrowRenderEventArgs e)
        {
            e.ArrowColor = Gdi("TextMuted");
            base.OnRenderArrow(e);
        }

        protected override void OnRenderItemCheck(Forms.ToolStripItemImageRenderEventArgs e)
        {
            // A cyan square with a dark tick, the CheckBox template's look.
            Drawing.Rectangle r = new(e.ImageRectangle.X + 1, e.ImageRectangle.Y + 1,
                                      e.ImageRectangle.Width - 2, e.ImageRectangle.Height - 2);
            using var fill = new Drawing.SolidBrush(Gdi("Accent"));
            e.Graphics.FillRectangle(fill, r);
            using var pen = new Drawing.Pen(Gdi("OnAccent"), 2);
            e.Graphics.SmoothingMode = Drawing.Drawing2D.SmoothingMode.AntiAlias;
            e.Graphics.DrawLines(pen, new[]
            {
                new Drawing.PointF(r.Left + r.Width * 0.22f, r.Top + r.Height * 0.52f),
                new Drawing.PointF(r.Left + r.Width * 0.42f, r.Top + r.Height * 0.72f),
                new Drawing.PointF(r.Left + r.Width * 0.78f, r.Top + r.Height * 0.28f),
            });
        }
    }

    private sealed class TrayColours : Forms.ProfessionalColorTable
    {
        public TrayColours() { UseSystemColors = false; }

        public override Drawing.Color ToolStripDropDownBackground => Gdi("Panel");
        public override Drawing.Color ImageMarginGradientBegin => Gdi("Panel");
        public override Drawing.Color ImageMarginGradientMiddle => Gdi("Panel");
        public override Drawing.Color ImageMarginGradientEnd => Gdi("Panel");
        public override Drawing.Color MenuBorder => Gdi("Line2");
        public override Drawing.Color MenuItemBorder => Gdi("Selected");
        public override Drawing.Color MenuItemSelected => Gdi("Selected");
        public override Drawing.Color MenuItemSelectedGradientBegin => Gdi("Selected");
        public override Drawing.Color MenuItemSelectedGradientEnd => Gdi("Selected");
        public override Drawing.Color MenuItemPressedGradientBegin => Gdi("Selected");
        public override Drawing.Color MenuItemPressedGradientEnd => Gdi("Selected");
        public override Drawing.Color SeparatorDark => Gdi("Line2");
        public override Drawing.Color SeparatorLight => Gdi("Panel");
        public override Drawing.Color CheckBackground => Gdi("Accent");
        public override Drawing.Color CheckSelectedBackground => Gdi("Accent");
        public override Drawing.Color CheckPressedBackground => Gdi("Accent");
    }
}
