using System;

namespace JLDisplayManager.Services.Music;

/// <summary>
/// What is being done right now to get a track's video ready, and how far it
/// has got.
///
/// This exists because the work is invisible otherwise. A download takes
/// seconds and reports itself; building a video's frames takes up to a minute,
/// and the first version of it gave no sign at all — the panel sat on the card
/// and the app said nothing, which is indistinguishable from broken.
///
/// Static because of who reports into it and who reads it: the download is run
/// by a static cache, the frame build by the follower, and the reader is a
/// sensor provider holding neither. One track is being prepared at a time, so
/// there is nothing here worth keying by file.
/// </summary>
public static class MusicPrepare
{
    private static volatile string _stage = "";
    private static volatile int _percent;

    /// <summary>Something is being prepared, so the card's bar should show.</summary>
    public static bool Busy => _stage.Length > 0;

    /// <summary>A short label for it: "Preparing video". Empty when idle.</summary>
    public static string Stage => _stage;

    /// <summary>0-100. Held at 0 until the work reports its first figure.</summary>
    public static int Percent => _percent;

    public static void Begin(string stage)
    {
        _percent = 0;
        _stage = stage;
    }

    public static void Report(int percent) => _percent = Math.Clamp(percent, 0, 100);

    public static void End()
    {
        _stage = "";
        _percent = 0;
    }
}
