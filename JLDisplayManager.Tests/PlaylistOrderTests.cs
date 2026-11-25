using System;
using System.IO;
using System.Linq;
using JLDisplayManager.Models;
using JLDisplayManager.Services;
using Xunit;

namespace JLDisplayManager.Tests;

/// <summary>The order a playlist plays in (PlaylistPlayer.BuildOrder).</summary>
public class PlaylistOrderTests
{
    private static (AppLibrary lib, MediaItem[] items) Library(TestSupport.TempDir dir, int n)
    {
        var lib = new AppLibrary();
        var items = Enumerable.Range(0, n).Select(i =>
        {
            string path = dir.File($"item{i}.png");
            File.WriteAllBytes(path, new byte[] { 1 });
            return new MediaItem { Path = path, Name = $"item{i}" };
        }).ToArray();
        lib.Items.AddRange(items);
        return (lib, items);
    }

    [Fact]
    public void Follows_the_playlist_not_the_library()
    {
        using var dir = new TestSupport.TempDir();
        var (lib, it) = Library(dir, 4);
        lib.Playlist.AddRange(new[] { it[2].Id, it[0].Id, it[3].Id });

        var order = PlaylistPlayer.BuildOrder(lib, new Random(1));
        Assert.Equal(new[] { it[2], it[0], it[3] }, order);
    }

    [Fact]
    public void Skips_removed_items_and_missing_files()
    {
        using var dir = new TestSupport.TempDir();
        var (lib, it) = Library(dir, 3);
        File.Delete(it[1].Path);
        lib.Playlist.AddRange(new[] { it[0].Id, Guid.NewGuid(), it[1].Id, it[2].Id });

        var order = PlaylistPlayer.BuildOrder(lib, new Random(1));
        Assert.Equal(new[] { it[0], it[2] }, order);
    }

    [Fact]
    public void Shuffle_is_a_permutation_and_seed_deterministic()
    {
        using var dir = new TestSupport.TempDir();
        var (lib, it) = Library(dir, 12);
        lib.Playlist.AddRange(it.Select(i => i.Id));
        lib.Shuffle = true;

        var a = PlaylistPlayer.BuildOrder(lib, new Random(42));
        var b = PlaylistPlayer.BuildOrder(lib, new Random(42));

        Assert.Equal(a, b);                                         // same seed, same order
        Assert.Equal(it.OrderBy(i => i.Id), a.OrderBy(i => i.Id));  // every item once
        Assert.NotEqual(it, a);                                     // and actually shuffled
    }

    [Fact]
    public void Empty_playlist_gives_empty_order()
    {
        using var dir = new TestSupport.TempDir();
        var (lib, _) = Library(dir, 3);
        Assert.Empty(PlaylistPlayer.BuildOrder(lib, new Random(1)));
    }
}
