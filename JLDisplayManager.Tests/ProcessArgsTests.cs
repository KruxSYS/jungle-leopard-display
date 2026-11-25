using System.Linq;
using JLDisplayManager.Services.Music;
using Xunit;

namespace JLDisplayManager.Tests;

/// <summary>yt-dlp arguments (audit M3).</summary>
public class ProcessArgsTests
{
    [Theory]
    [InlineData("dQw4w9WgXcQ", true)]
    [InlineData("-abcdEFG_12", true)]    // ids may start with '-'
    [InlineData("_abcdEFG-12", true)]
    [InlineData("short", false)]
    [InlineData("dQw4w9WgXcQx", false)]  // 12 chars
    [InlineData("..\\..\\evil1", false)] // a path, not an id
    [InlineData("dQw4w9 gXcQ", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void Video_id_shape(string? id, bool ok) =>
        Assert.Equal(ok, MusicVideoCache.IsVideoId(id));

    [Fact]
    public void Id_comes_last_after_end_of_options()
    {
        var args = MusicVideoCache.DownloadArgs("-abcdEFG_12", @"C:\cache\-abcdEFG_12.mp4");

        Assert.Equal("-abcdEFG_12", args[^1]);
        Assert.Equal("--", args[^2]);
        // Nothing positional before the marker: every earlier argument is an
        // option or an option's value.
        Assert.Equal(1, args.Count(a => a == "--"));
        Assert.Contains("-o", args);
        Assert.Equal(@"C:\cache\-abcdEFG_12.mp4", args[args.IndexOf("-o") + 1]);
    }
}
