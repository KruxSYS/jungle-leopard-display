using System;
using System.IO;
using System.Threading;

namespace JLDisplayManager.Tests;

internal static class TestSupport
{
    /// <summary>A fresh empty directory under %TEMP%, removed by Dispose.</summary>
    public sealed class TempDir : IDisposable
    {
        public string Path { get; } = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(), "jl-tests-" + Guid.NewGuid().ToString("N"));

        public TempDir() => Directory.CreateDirectory(Path);

        public string File(string name) => System.IO.Path.Combine(Path, name);

        public void Dispose()
        {
            try { Directory.Delete(Path, recursive: true); } catch { }
        }
    }

    /// <summary>The repository root: the directory holding the .sln.</summary>
    public static string RepoRoot
    {
        get
        {
            string? dir = AppContext.BaseDirectory;
            while (dir != null)
            {
                if (System.IO.File.Exists(System.IO.Path.Combine(dir, "Jungle Leopard Display.sln")))
                    return dir;
                dir = System.IO.Path.GetDirectoryName(dir);
            }
            throw new InvalidOperationException("could not find the repository root above " +
                                                AppContext.BaseDirectory);
        }
    }

    /// <summary>Runs <paramref name="action"/> on an STA thread, as WPF requires.</summary>
    public static void Sta(Action action)
    {
        Exception? failure = null;
        var t = new Thread(() =>
        {
            try { action(); }
            catch (Exception ex) { failure = ex; }
        });
        t.SetApartmentState(ApartmentState.STA);
        t.Start();
        t.Join();
        if (failure != null) throw new Exception("STA test failed", failure);
    }
}
