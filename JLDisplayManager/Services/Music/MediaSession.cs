using System;
using System.Collections.Generic;

using JLDisplayManager.Interop;
using JLDisplayManager.Models;

namespace JLDisplayManager.Services.Music;

/// <summary>
/// The one Windows media session reader, shared by everything that wants to
/// know what is playing.
///
/// The native side is a process-wide singleton with a plain start/stop, which
/// is fine until two callers want it: the sensor provider that publishes
/// <c>music.*</c> tokens, and the follower that puts the track on the panel.
/// Either one disposing would have stopped the reader out from under the other.
/// So acquisition is counted here, and the native reader runs while anybody
/// holds it.
/// </summary>
public static class MediaSession
{
    private static readonly object Lock = new();
    private static int _holders;
    private static string? _preferredApp;

    /// <summary>
    /// Source app ids to friendly names. These are identities, not labels —
    /// Firefox reports <c>308046B0AF4A39CB</c> — so anything unrecognised falls
    /// back to the id with any <c>.exe</c> trimmed off.
    /// </summary>
    private static readonly Dictionary<string, string> FriendlyNames =
        new(StringComparer.OrdinalIgnoreCase)
        {
            ["308046B0AF4A39CB"] = "Firefox",
            ["firefox.exe"] = "Firefox",
            ["Chrome"] = "Chrome",
            ["chrome.exe"] = "Chrome",
            ["MSEdge"] = "Edge",
            ["msedge.exe"] = "Edge",
            ["Brave"] = "Brave",
            ["brave.exe"] = "Brave",
            ["opera.exe"] = "Opera",
            ["vivaldi.exe"] = "Vivaldi",
            ["Spotify.exe"] = "Spotify",
            ["SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify"] = "Spotify",
            ["Zen.exe"] = "Zen",
            ["zen.exe"] = "Zen",
        };

    /// <summary>
    /// Starts the reader if it is not already running, and counts the caller as
    /// a holder. Safe to call repeatedly; every Acquire needs a Release.
    /// </summary>
    public static void Acquire(string? preferredApp)
    {
        lock (Lock)
        {
            _preferredApp = Normalise(preferredApp);
            _holders++;

            try
            {
                NativeMethods.jl_media_start(_preferredApp);
            }
            catch (Exception ex)
            {
                Storage.Log($"media session: could not start the reader: {ex.Message}");
            }
        }
    }

    public static void Release()
    {
        lock (Lock)
        {
            if (_holders == 0) return;
            if (--_holders > 0) return;

            try
            {
                NativeMethods.jl_media_stop();
            }
            catch (Exception ex)
            {
                Storage.Log($"media session: could not stop the reader: {ex.Message}");
            }
        }
    }

    /// <summary>
    /// Changes which player is followed. Takes effect on the reader's next tick;
    /// there is no restart involved.
    /// </summary>
    public static void Repoint(string? preferredApp)
    {
        lock (Lock)
        {
            _preferredApp = Normalise(preferredApp);
            if (_holders == 0) return;

            try
            {
                NativeMethods.jl_media_start(_preferredApp);
            }
            catch (Exception ex)
            {
                Storage.Log($"media session: could not repoint the reader: {ex.Message}");
            }
        }
    }

    /// <summary>What the reader last saw. Cheap — it copies a struct.</summary>
    public static NativeMethods.JlMediaStatus Read()
    {
        try
        {
            NativeMethods.jl_media_get(out NativeMethods.JlMediaStatus status);
            return status;
        }
        catch
        {
            // The DLL is missing or too old. Nothing playing is the right answer.
            return default;
        }
    }

    /// <summary>
    /// The current track's album art as the player supplied it — PNG or JPEG,
    /// its choice — or null when there is none.
    /// </summary>
    public static byte[]? Thumbnail()
    {
        try
        {
            int size = NativeMethods.jl_media_thumbnail(null, 0);
            if (size <= 0) return null;

            byte[] buf = new byte[size];
            int got = NativeMethods.jl_media_thumbnail(buf, size);

            // A track that changed between the two calls can report a different
            // size the second time; take it next poll rather than half of each.
            return got == size ? buf : null;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>Every player currently registered with Windows, by app id.</summary>
    public static IReadOnlyList<string> Sessions()
    {
        try
        {
            int need = NativeMethods.jl_media_sessions(null, 0);
            if (need <= 1) return Array.Empty<string>();

            char[] buf = new char[need];
            if (NativeMethods.jl_media_sessions(buf, need) > need) return Array.Empty<string>();

            var list = new List<string>();
            int start = 0;
            for (int i = 0; i < buf.Length; i++)
            {
                if (buf[i] != '\0') continue;
                if (i > start) list.Add(new string(buf, start, i - start));
                start = i + 1;
            }

            return list;
        }
        catch
        {
            return Array.Empty<string>();
        }
    }

    /// <summary>A name worth showing for a source app id.</summary>
    public static string FriendlyName(string? appId)
    {
        if (string.IsNullOrWhiteSpace(appId)) return "";
        if (FriendlyNames.TryGetValue(appId, out string? name)) return name;

        return appId.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            ? appId[..^4]
            : appId;
    }

    private static string? Normalise(string? app) =>
        string.IsNullOrWhiteSpace(app) ? null : app.Trim();
}
