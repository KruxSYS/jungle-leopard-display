using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Markup;
using System.Windows.Media;
using Xunit;

namespace JLDisplayManager.Tests;

/// <summary>
/// The KruxSYS theme (Themes/KruxSYS.xaml, docs/brand.md): the tokens have the
/// brand's values, nothing outside the theme writes a colour, and every key a
/// view asks for exists - a mistyped StaticResource is otherwise a crash the
/// first time that window opens.
/// </summary>
public class ThemeTests
{
    private static string Manager => Path.Combine(TestSupport.RepoRoot, "JLDisplayManager");
    private static string ThemePath => Path.Combine(Manager, "Themes", "KruxSYS.xaml");

    private static ResourceDictionary LoadTheme()
    {
        using var stream = File.OpenRead(ThemePath);
        return (ResourceDictionary)XamlReader.Load(stream);
    }

    public static IEnumerable<object[]> BrandTokens() => new[]
    {
        new object[] { "Bg", "#07090C" },
        new object[] { "Panel", "#0C1016" },
        new object[] { "Panel2", "#111620" },
        new object[] { "Line", "#1A212C" },
        new object[] { "Line2", "#2A3442" },
        new object[] { "Text", "#E8ECF2" },
        new object[] { "TextMuted", "#8E9BB0" },
        new object[] { "TextSoft", "#C9D1DD" },
        new object[] { "Accent", "#22D3EE" },
        new object[] { "AccentHover", "#67E8F9" },
        new object[] { "OnAccent", "#031014" },
        new object[] { "Selected", "#0E242C" },
        new object[] { "AccentDim", "#0F2B34" },
        new object[] { "Error", "#FF8A80" },
        new object[] { "Ok", "#6FDC95" },
        new object[] { "CloseHover", "#C42B1C" },
    };

    [Theory]
    [MemberData(nameof(BrandTokens))]
    public void Token_has_the_brand_value(string key, string hex)
    {
        TestSupport.Sta(() =>
        {
            var theme = LoadTheme();
            var brush = Assert.IsType<SolidColorBrush>(theme[key]);
            Assert.Equal((Color)ColorConverter.ConvertFromString(hex), brush.Color);
        });
    }

    [Fact]
    public void Corners_are_square()
    {
        TestSupport.Sta(() =>
        {
            var theme = LoadTheme();
            Assert.Equal(new CornerRadius(2), theme["RadiusControl"]);
            Assert.Equal(new CornerRadius(0), theme["RadiusPanel"]);
            Assert.Equal(new Thickness(1), theme["BorderThin"]);
        });
    }

    [Fact]
    public void Amber_is_not_in_the_ui()
    {
        // #ffb547 is the brand's release-status colour (README badges only);
        // #F0A030 was the old accent. Neither belongs anywhere in the app.
        string theme = File.ReadAllText(ThemePath);
        Assert.DoesNotContain("FFB547", theme, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("F0A030", theme, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Files allowed to write a colour: the theme, and overlay content - what
    /// is drawn on the user's panel, which the rebrand deliberately left alone.
    /// </summary>
    private static readonly string[] ColourAllowed =
    {
        @"Themes\KruxSYS.xaml",
        @"Models\Overlay\",
        @"Services\Overlay\Palette.cs",
    };

    /// <summary>PropertyPanel lines that are overlay content (documented in docs/brand.md).</summary>
    private static readonly string[] ColourAllowedLines =
    {
        "set(v ?? \"#FFFFFFFF\")",            // a new layer's default colour
        "Threshold stops, as one line of text", // doc comment showing the syntax
    };

    [Fact]
    public void No_hard_coded_colours_outside_the_theme()
    {
        var hex = new Regex("#[0-9A-Fa-f]{6}");
        var offenders = new List<string>();

        foreach (string file in SourceFiles())
        {
            string rel = Path.GetRelativePath(Manager, file);
            if (ColourAllowed.Any(a => rel.StartsWith(a, StringComparison.OrdinalIgnoreCase))) continue;

            string[] lines = File.ReadAllLines(file);
            for (int i = 0; i < lines.Length; i++)
                if (hex.IsMatch(lines[i]) && !ColourAllowedLines.Any(lines[i].Contains))
                    offenders.Add($"{rel}:{i + 1}: {lines[i].Trim()}");
        }

        Assert.True(offenders.Count == 0, "colours outside the theme:\n" + string.Join("\n", offenders));
    }

    [Fact]
    public void No_rgb_literals_in_views()
    {
        // The same rule for colours built in code.
        var built = new Regex(@"Color\.FromRgb\(|Color\.FromArgb\(\s*(0x|\d)");
        var offenders = Directory.EnumerateFiles(Path.Combine(Manager, "Views"), "*.cs", SearchOption.AllDirectories)
            .Concat(new[] { Path.Combine(Manager, "App.xaml.cs") })
            .SelectMany(f => File.ReadAllLines(f).Select((l, i) => (f, l, i)))
            .Where(x => built.IsMatch(x.l))
            .Select(x => $"{Path.GetRelativePath(Manager, x.f)}:{x.i + 1}: {x.l.Trim()}")
            .ToList();

        Assert.True(offenders.Count == 0, "colours built in code:\n" + string.Join("\n", offenders));
    }

    [Fact]
    public void Every_resource_key_used_exists()
    {
        var defined = new HashSet<string>(StringComparer.Ordinal);
        TestSupport.Sta(() =>
        {
            foreach (object k in LoadTheme().Keys) defined.Add(k.ToString()!);
        });

        var xKey = new Regex("x:Key=\"([^\"]+)\"");
        foreach (string f in Directory.EnumerateFiles(Manager, "*.xaml", SearchOption.AllDirectories)
                     .Where(NotBuildOutput))
            foreach (Match m in xKey.Matches(File.ReadAllText(f)))
                defined.Add(m.Groups[1].Value);

        var used = new Regex(@"\{(?:Static|Dynamic)Resource\s+([A-Za-z0-9_]+)\}" +
                             @"|(?:Theme\.Brush|Theme\.Color|Theme\.Gdi|FindResource|TryFindResource)\(""([A-Za-z0-9_]+)""\)");

        var missing = new List<string>();
        foreach (string f in SourceFiles())
        {
            string text = File.ReadAllText(f);
            foreach (Match m in used.Matches(text))
            {
                string key = m.Groups[1].Success ? m.Groups[1].Value : m.Groups[2].Value;
                // Theme.Color("X") reads XColor.
                if (m.Value.StartsWith("Theme.Color") || m.Value.StartsWith("Theme.Gdi")) key += "Color";
                if (!defined.Contains(key))
                    missing.Add($"{Path.GetRelativePath(Manager, f)}: {key}");
            }
        }

        Assert.True(missing.Count == 0, "resource keys with no definition:\n" +
                                        string.Join("\n", missing.Distinct()));
    }

    private static bool NotBuildOutput(string path) =>
        !path.Contains(@"\obj\") && !path.Contains(@"\bin\") && !path.Contains(@"\x64\");

    private static IEnumerable<string> SourceFiles() =>
        Directory.EnumerateFiles(Manager, "*.*", SearchOption.AllDirectories)
            .Where(f => f.EndsWith(".xaml") || f.EndsWith(".cs"))
            .Where(NotBuildOutput);
}
