using System;
using System.IO;
using System.Linq;
using JLDisplayManager.Models;
using Xunit;

namespace JLDisplayManager.Tests;

/// <summary>Settings and library persistence (audit H3, M7).</summary>
public class StorageTests
{
    [Fact]
    public void Settings_round_trip()
    {
        using var dir = new TestSupport.TempDir();
        string path = dir.File("settings.json");

        var s = new AppSettings { Rotate = 180, Fps = 24, Brightness = 40, Hwaccel = "cuda" };
        Storage.Save(path, s);

        var back = Storage.Load<AppSettings>(path);
        Assert.NotNull(back);
        Assert.Equal(180, back!.Rotate);
        Assert.Equal(24, back.Fps);
        Assert.Equal(40, back.Brightness);
        Assert.Equal("cuda", back.Hwaccel);
        Assert.False(File.Exists(path + ".tmp"));   // written beside, then moved
    }

    [Fact]
    public void Library_round_trip_keeps_items_and_playlist_order()
    {
        using var dir = new TestSupport.TempDir();
        string path = dir.File("library.json");

        var a = new MediaItem { Path = @"C:\media\a.png", Name = "a" };
        var b = new MediaItem { Path = @"C:\media\b.mp4", Name = "b", IsVideo = true };
        var lib = new AppLibrary { Shuffle = true, LastPlayed = b.Id };
        lib.Items.Add(a);
        lib.Items.Add(b);
        lib.Playlist.Add(b.Id);
        lib.Playlist.Add(a.Id);

        Storage.Save(path, lib);
        var back = Storage.Load<AppLibrary>(path)!;

        Assert.Equal(new[] { a.Id, b.Id }, back.Items.Select(i => i.Id));
        Assert.Equal(new[] { b.Id, a.Id }, back.Playlist);
        Assert.True(back.Shuffle);
        Assert.Equal(b.Id, back.LastPlayed);
        Assert.True(back.Items[1].IsVideo);
    }

    [Fact]
    public void Missing_file_loads_as_null_without_side_effects()
    {
        using var dir = new TestSupport.TempDir();
        Assert.Null(Storage.Load<AppSettings>(dir.File("absent.json")));
        Assert.Empty(Directory.GetFiles(dir.Path));
    }

    [Fact]
    public void Corrupt_file_is_kept_aside_not_overwritten()
    {
        using var dir = new TestSupport.TempDir();
        string path = dir.File("library.json");
        const string damaged = "{ \"Items\": [ { \"Path\": \"C:\\\\a.png\"";   // truncated mid-write
        File.WriteAllText(path, damaged);

        Assert.Null(Storage.Load<AppLibrary>(path));

        // The original is gone from its name - so the next save cannot clobber
        // it - and survives byte for byte beside it.
        Assert.False(File.Exists(path));
        string kept = Assert.Single(Directory.GetFiles(dir.Path, "library.json.corrupt-*"));
        Assert.Equal(damaged, File.ReadAllText(kept));

        // And a save afterwards writes a new file without touching the kept one.
        Storage.Save(path, new AppLibrary());
        Assert.True(File.Exists(path));
        Assert.Equal(damaged, File.ReadAllText(kept));
    }

    [Fact]
    public void Quarantine_never_overwrites_an_earlier_one()
    {
        using var dir = new TestSupport.TempDir();
        string path = dir.File("settings.json");

        File.WriteAllText(path, "first");
        string one = Storage.Quarantine(path);
        File.WriteAllText(path, "second");
        string two = Storage.Quarantine(path);

        Assert.NotEqual(one, two);
        Assert.Equal("first", File.ReadAllText(one));
        Assert.Equal("second", File.ReadAllText(two));
    }

    [Fact]
    public void Log_rolls_past_its_limit()
    {
        using var dir = new TestSupport.TempDir();
        string log = dir.File("manager.log");

        File.WriteAllText(log, new string('x', 100));
        Storage.RollIfLarge(log, 1000);
        Assert.True(File.Exists(log));                  // under the limit: untouched
        Assert.False(File.Exists(log + ".1"));

        File.WriteAllText(log, new string('y', 1000));
        Storage.RollIfLarge(log, 1000);
        Assert.False(File.Exists(log));                 // rolled
        Assert.Equal(1000, new FileInfo(log + ".1").Length);
    }
}
