using System;
using System.Collections.Generic;

namespace JLDisplayManager.Models.Overlay;

/// <summary>
/// A named arrangement of layers. Profiles are the unit people switch between —
/// "Minimal" while gaming, "Full telemetry" while rendering — and the unit they
/// export and share, which is why they live in their own file rather than
/// inside settings.json.
/// </summary>
public sealed class OverlayProfile
{
    public Guid Id { get; set; } = Guid.NewGuid();

    public string Name { get; set; } = "Untitled";

    /// <summary>
    /// Which <see cref="OverlayTheme"/> this profile draws with. Stored by name
    /// rather than by value, so improving a shipped theme improves every profile
    /// using it — and so a profile stays readable when hand-edited.
    ///
    /// Empty or unrecognised means <c>minimal</c>, which holds the values every
    /// profile written before themes existed already used.
    /// </summary>
    public string Theme { get; set; } = "minimal";

    /// <summary>Bottom first: list order is z-order, exactly as the editor shows it.</summary>
    public List<OverlayLayer> Layers { get; set; } = new();

    /// <summary>
    /// A copy with its own layer list but the same layer objects.
    ///
    /// The render thread works from one of these because the editor mutates the
    /// live list from the UI thread — see <c>OverlayService.Refresh</c>. Layer
    /// objects stay shared on purpose, so a drag in progress shows through.
    ///
    /// It lives here, beside the fields, rather than in the service: it is a
    /// copy that has to list every property, and doing that anywhere else is how
    /// a newly added field gets silently left behind. <see cref="Theme"/> was,
    /// exactly once, and the symptom was a theme picker that did nothing.
    /// </summary>
    public OverlayProfile ShallowCopy() => new()
    {
        Id = Id,
        Name = Name,
        Theme = Theme,
        Layers = new List<OverlayLayer>(Layers),
    };
}

/// <summary>Everything the overlay feature remembers, saved to overlays.json.</summary>
public sealed class OverlaySettings
{
    /// <summary>Master switch. Off costs nothing at all on the native side.</summary>
    public bool Enabled { get; set; }

    public Guid? ActiveProfileId { get; set; }

    /// <summary>
    /// How often the overlay is redrawn, at most. 10 is plenty: sensors move at
    /// 1 Hz and the render is the most expensive step in the whole pipeline, so
    /// pushing this towards the panel's 30 fps buys nothing but CPU.
    /// </summary>
    public int RenderHz { get; set; } = 10;

    /// <summary>How often sensors are read, in milliseconds.</summary>
    public int SensorPollMs { get; set; } = 1000;

    public List<OverlayProfile> Profiles { get; set; } = new();

    public OverlayProfile? Active()
    {
        if (Profiles.Count == 0) return null;

        foreach (OverlayProfile p in Profiles)
            if (p.Id == ActiveProfileId) return p;

        return Profiles[0];
    }

    /// <summary>
    /// Converts colours and fonts that were written before themes existed into
    /// the roles that follow one.
    ///
    /// Needed because "a literal colour always wins" — the rule that protects a
    /// shade somebody deliberately picked — also protects every colour the
    /// generator baked in before roles existed. Without this, switching theme on
    /// a profile made yesterday changes nothing, which reads as broken rather
    /// than as a rule.
    ///
    /// Deliberately narrow: only the exact values the old palette emitted are
    /// converted. Anything else was chosen by hand and stays untouched. And
    /// every one of these roles resolves under <c>minimal</c> to the very hex it
    /// replaces, so a user who never switches theme sees no change at all.
    ///
    /// Returns how many values were rewritten, for the log.
    /// </summary>
    public static int MigrateToRoles(OverlaySettings settings)
    {
        // The values AccentPalette used to hand out, and the model defaults of
        // the same era.
        var colours = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["#FFFFFFFF"] = "text",
            ["#FFD0D0D0"] = "dim",
            ["#FF4AD995"] = "good",
            ["#FF5AB6FF"] = "cool",
            ["#FFFFB43A"] = "warm",
            ["#FFFF5A52"] = "hot",
            ["#73000000"] = "panel",
            ["#80000000"] = "panel",
            ["#60FFFFFF"] = "track",
            ["#50FFFFFF"] = "track",
            ["#80FFFFFF"] = "line",
        };

        int changed = 0;

        string? Map(string? value)
        {
            if (value == null) return null;
            if (!colours.TryGetValue(value.Trim(), out string? role)) return value;
            changed++;
            return role;
        }

        // The old default. Empty means "follow the theme", and resolves back to
        // Segoe UI under minimal, so this is invisible until a theme is chosen.
        string Font(string f)
        {
            if (!string.Equals(f.Trim(), "Segoe UI", StringComparison.OrdinalIgnoreCase)) return f;
            changed++;
            return "";
        }

        foreach (OverlayProfile profile in settings.Profiles)
        {
            foreach (OverlayLayer layer in profile.Layers)
            {
                switch (layer)
                {
                    case TextLayer t:
                        t.Colour = Map(t.Colour) ?? t.Colour;
                        t.BackgroundColour = Map(t.BackgroundColour);
                        t.FontFamily = Font(t.FontFamily);
                        foreach (ColourStop s in t.Thresholds) s.Colour = Map(s.Colour) ?? s.Colour;
                        break;

                    case BarLayer b:
                        b.TrackColour = Map(b.TrackColour) ?? b.TrackColour;
                        b.FillColour = Map(b.FillColour) ?? b.FillColour;
                        b.FillColourTo = Map(b.FillColourTo);
                        b.BorderColour = Map(b.BorderColour);
                        foreach (ColourStop s in b.Thresholds) s.Colour = Map(s.Colour) ?? s.Colour;
                        break;

                    case GaugeLayer g:
                        g.TrackColour = Map(g.TrackColour) ?? g.TrackColour;
                        g.FillColour = Map(g.FillColour) ?? g.FillColour;
                        g.TickColour = Map(g.TickColour) ?? g.TickColour;
                        g.CentreColour = Map(g.CentreColour) ?? g.CentreColour;
                        g.CaptionColour = Map(g.CaptionColour) ?? g.CaptionColour;
                        g.FontFamily = Font(g.FontFamily);
                        foreach (ColourStop s in g.Thresholds) s.Colour = Map(s.Colour) ?? s.Colour;
                        break;

                    case ShapeLayer sh:
                        sh.FillColour = Map(sh.FillColour);
                        sh.StrokeColour = Map(sh.StrokeColour);
                        break;
                }
            }
        }

        return changed;
    }

    /// <summary>
    /// A first-run profile, so turning the overlay on shows something rather
    /// than a blank panel and an instruction to go and edit some JSON.
    /// Deliberately modest: two corner readouts that stay out of the picture.
    /// </summary>
    /// <summary>The well-known id of the built-in "Now playing" card.</summary>
    public static readonly Guid NowPlayingId = new("6d757369-6300-4000-8000-6e6f77706c79");

    // The download indicator's layers also have fixed ids, so a card saved
    // before they existed can be given them without duplicating anything.
    private static readonly Guid FetchTextId = new("6d757369-6301-4000-8000-6e6f77706c79");
    private static readonly Guid FetchBarId = new("6d757369-6302-4000-8000-6e6f77706c79");

    /// <summary>
    /// The line that says a video is being fetched, and the bar under it.
    ///
    /// Both are bound to <c>music.fetching</c> and so are invisible unless a
    /// download is actually running — which is most of the time, since a track
    /// heard before plays from the cache with no fetch at all.
    /// </summary>
    private static TextLayer FetchText() => new()
    {
        Id = FetchTextId,
        Name = "Fetching label",
        Anchor = LayerAnchor.MiddleLeft,
        X = 356, Y = 136, Width = 552, Height = 22,
        Template = "Fetching video  {music.fetch:0}%",
        FontSize = 15,
        Bold = false,
        Colour = "dim",
        VisibleWhen = VisibilityRule.SensorAbove,
        VisibleSource = "music.fetching",
        VisibleThreshold = 0.5,
    };

    private static BarLayer FetchBar() => new()
    {
        Id = FetchBarId,
        Name = "Fetching bar",
        Anchor = LayerAnchor.MiddleLeft,
        X = 356, Y = 160, Width = 552, Height = 6,
        Source = "music.fetch",
        Min = 0,
        Max = 100,

        // Deliberately not the track bar's colour: two green bars one above the
        // other would read as one thing at arm's length.
        FillColour = "cool",
        VisibleWhen = VisibilityRule.SensorAbove,
        VisibleSource = "music.fetching",
        VisibleThreshold = 0.5,
    };

    /// <summary>
    /// Adds anything the shipped card has gained since a saved copy of it was
    /// written, and returns how many layers that was.
    ///
    /// The card lives in the user's profile list and is theirs to edit, so it
    /// cannot simply be replaced on upgrade. Matching on the fixed layer ids
    /// adds only what is genuinely missing and leaves every edit alone.
    /// </summary>
    public static int UpgradeNowPlaying(OverlayProfile card)
    {
        int added = 0;

        if (!card.Layers.Exists(l => l.Id == FetchTextId)) { card.Layers.Add(FetchText()); added++; }
        if (!card.Layers.Exists(l => l.Id == FetchBarId)) { card.Layers.Add(FetchBar()); added++; }

        return added;
    }

    public static OverlayProfile CreateDefault()
    {
        var profile = new OverlayProfile { Name = "Essentials" };

        profile.Layers.Add(new ShapeLayer
        {
            Name = "CPU panel",
            Anchor = LayerAnchor.TopLeft,
            X = 14, Y = 14, Width = 250, Height = 86,
            Kind = ShapeKind.Rectangle,
            CornerRadius = -1,   // follow the theme
            FillColour = "panel",
        });

        // Load and clock rather than temperature: nothing supplies cpu.temp
        // without LibreHardwareMonitor or HWiNFO, and a default profile that
        // ships reading "--°C" looks broken rather than looking like a feature
        // waiting to be enabled.
        profile.Layers.Add(new TextLayer
        {
            Name = "CPU readout",
            Anchor = LayerAnchor.TopLeft,
            X = 28, Y = 24, Width = 224, Height = 30,
            Template = "CPU  {cpu.load:0}%   {cpu.clock:0.0} GHz",
            FontSize = 22,
            ThresholdSource = "cpu.load",
            Thresholds =
            {
                new ColourStop { AtOrAbove = 0,  Colour = "text" },
                new ColourStop { AtOrAbove = 75, Colour = "warm" },
                new ColourStop { AtOrAbove = 92, Colour = "hot" },
            },
        });

        profile.Layers.Add(new BarLayer
        {
            Name = "CPU load bar",
            Anchor = LayerAnchor.TopLeft,
            X = 28, Y = 60, Width = 222, Height = 12,
            Source = "cpu.load",
            Thresholds =
            {
                new ColourStop { AtOrAbove = 0,  Colour = "good" },
                new ColourStop { AtOrAbove = 70, Colour = "warm" },
                new ColourStop { AtOrAbove = 90, Colour = "hot" },
            },
        });

        profile.Layers.Add(new ShapeLayer
        {
            Name = "GPU panel",
            Anchor = LayerAnchor.TopRight,
            X = 14, Y = 14, Width = 250, Height = 86,
            Kind = ShapeKind.Rectangle,
            CornerRadius = -1,   // follow the theme
            FillColour = "panel",
        });

        profile.Layers.Add(new TextLayer
        {
            Name = "GPU readout",
            Anchor = LayerAnchor.TopRight,
            X = 28, Y = 24, Width = 224, Height = 30,
            Template = "GPU  {gpu.load:0}%   {gpu.temp:0}°C",
            FontSize = 22,
            ThresholdSource = "gpu.temp",
            Thresholds =
            {
                new ColourStop { AtOrAbove = 0,  Colour = "text" },
                new ColourStop { AtOrAbove = 75, Colour = "warm" },
                new ColourStop { AtOrAbove = 87, Colour = "hot" },
            },
        });

        profile.Layers.Add(new BarLayer
        {
            Name = "GPU load bar",
            Anchor = LayerAnchor.TopRight,
            X = 28, Y = 60, Width = 222, Height = 12,
            Source = "gpu.load",
            Thresholds =
            {
                new ColourStop { AtOrAbove = 0,  Colour = "good" },
                new ColourStop { AtOrAbove = 70, Colour = "warm" },
                new ColourStop { AtOrAbove = 90, Colour = "hot" },
            },
        });

        profile.Layers.Add(new TextLayer
        {
            Name = "Clock",
            Anchor = LayerAnchor.BottomRight,
            X = 20, Y = 16, Width = 200, Height = 44,
            Template = "{time.short}",
            FontSize = 34,
            Align = TextAlign.Right,
            ShadowOffsetX = 2,
            ShadowOffsetY = 2,
        });

        return profile;
    }

    /// <summary>
    /// The card shown while following whatever is playing elsewhere on the
    /// machine — the fallback for a track with no video, which on YouTube Music
    /// is most of them.
    ///
    /// It is an ordinary profile on purpose. The alternative was a private
    /// renderer that draws a card, and this way the whole thing is editable in
    /// the editor that already exists: move the art, restyle the title, change
    /// the theme. Nothing about it is special except that the follower switches
    /// to it.
    ///
    /// It draws over the blurred backdrop <see cref="Services.Music.MusicArt"/>
    /// puts on the panel, which is why the first layer is a scrim: the backdrop
    /// is deliberately left bright so the amount of darkening is a property of
    /// this profile rather than baked into a cache file.
    /// </summary>
    public static OverlayProfile CreateNowPlaying()
    {
        // A fixed id rather than a fresh one: the follower has to find this
        // profile again on every launch, and the user has to be able to edit it
        // and keep their edits.
        var profile = new OverlayProfile
        {
            Id = NowPlayingId,
            Name = "Now playing",
        };

        profile.Layers.Add(new ShapeLayer
        {
            Name = "Scrim",
            Anchor = LayerAnchor.TopLeft,
            X = 0, Y = 0, Width = 960, Height = 480,
            Kind = ShapeKind.Rectangle,
            CornerRadius = 0,
            FillColour = "#B0000000",
        });

        // "{music.art}" is a path, resolved per draw. A track with no art
        // leaves it unavailable and this draws nothing at all, which is why
        // there is no placeholder square behind it.
        profile.Layers.Add(new ImageLayer
        {
            Name = "Album art",
            Anchor = LayerAnchor.MiddleLeft,
            X = 56, Y = -8, Width = 260, Height = 260,
            File = "{music.art}",
            PreserveAspect = true,
            VisibleWhen = VisibilityRule.SensorBelow,
            VisibleSource = "music.idle",
            VisibleThreshold = 0.5,
        });

        profile.Layers.Add(new TextLayer
        {
            Name = "Title",
            Anchor = LayerAnchor.MiddleLeft,
            X = 356, Y = -96, Width = 552, Height = 96,
            Template = "{music.title}",
            FontSize = 44,
            Wrap = true,
            ShadowOffsetX = 2,
            ShadowOffsetY = 2,
            VisibleWhen = VisibilityRule.SensorBelow,
            VisibleSource = "music.idle",
            VisibleThreshold = 0.5,
        });

        profile.Layers.Add(new TextLayer
        {
            Name = "Artist",
            Anchor = LayerAnchor.MiddleLeft,
            X = 356, Y = 8, Width = 552, Height = 40,
            Template = "{music.artist}",
            FontSize = 30,
            Bold = false,
            Colour = "dim",
            ShadowOffsetX = 2,
            ShadowOffsetY = 2,
            VisibleWhen = VisibilityRule.SensorBelow,
            VisibleSource = "music.idle",
            VisibleThreshold = 0.5,
        });

        // The bar and the track length are hidden rather than empty when the
        // player does not report a duration — which happens for the first few
        // seconds of a track, and permanently for a live stream. An empty bar
        // and a "--" read as broken; nothing reads as "no length here".
        profile.Layers.Add(new BarLayer
        {
            Name = "Progress",
            Anchor = LayerAnchor.MiddleLeft,
            X = 356, Y = 76, Width = 552, Height = 8,
            Source = "music.progress",
            Min = 0,
            Max = 100,
            FillColour = "good",
            VisibleWhen = VisibilityRule.SensorAbove,
            VisibleSource = "music.duration",
            VisibleThreshold = 0,
        });

        profile.Layers.Add(new TextLayer
        {
            Name = "Elapsed",
            Anchor = LayerAnchor.MiddleLeft,
            X = 356, Y = 104, Width = 200, Height = 28,
            Template = "{music.elapsed}",
            FontSize = 20,
            Bold = false,
            Colour = "dim",
            VisibleWhen = VisibilityRule.SensorBelow,
            VisibleSource = "music.idle",
            VisibleThreshold = 0.5,
        });

        profile.Layers.Add(new TextLayer
        {
            Name = "Length",
            Anchor = LayerAnchor.MiddleLeft,
            X = 708, Y = 104, Width = 200, Height = 28,
            Template = "{music.length}",
            FontSize = 20,
            Bold = false,
            Colour = "dim",
            Align = TextAlign.Right,
            VisibleWhen = VisibilityRule.SensorAbove,
            VisibleSource = "music.duration",
            VisibleThreshold = 0,
        });

        profile.Layers.Add(FetchText());
        profile.Layers.Add(FetchBar());

        // The only thing on the panel when no player is running at all. Every
        // other music sensor is unavailable then, which is what hides the rest
        // of the card rather than leaving it full of dashes.
        profile.Layers.Add(new TextLayer
        {
            Name = "Nothing playing",
            Anchor = LayerAnchor.Centre,
            X = 0, Y = 0, Width = 600, Height = 44,
            Template = "Nothing playing",
            FontSize = 30,
            Bold = false,
            Colour = "dim",
            Align = TextAlign.Centre,
            VisibleWhen = VisibilityRule.SensorAbove,
            VisibleSource = "music.idle",
            VisibleThreshold = 0.5,
        });

        // Only while the track is actually stopped, so it does not sit over a
        // playing card saying nothing useful.
        profile.Layers.Add(new TextLayer
        {
            Name = "Paused",
            Anchor = LayerAnchor.TopRight,
            X = 24, Y = 20, Width = 200, Height = 30,
            Template = "{music.state}",
            FontSize = 20,
            Colour = "warm",
            Align = TextAlign.Right,
            VisibleWhen = VisibilityRule.SensorBelow,
            VisibleSource = "music.playing",
            VisibleThreshold = 0.5,
        });

        return profile;
    }
}
