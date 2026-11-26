# How it works

*Part of the [Jungle Leopard Display](../README.md) documentation. Moved out of the README in 1.7.0, unchanged.*

```
JLDisplayCore  (static lib)   protocol · framing · ffmpeg · calibration · packs
      │
      ├── Jungle Leopard Display.exe    thin CLI over the core
      │
      └── JLDisplayNative.dll           flat C API, async workers
                │
                └── JungleLeopardDisplayManager.exe   WPF tray app (P/Invoke)
```

The core is a **static** library so the CLI stays a single self-contained exe;
`JLDisplayNative.dll` exists only to give C# something to P/Invoke.

Two constraints shape everything:

**One port owner.** Only one process on the machine can hold the COM port, so
the tray app is the sole owner and the GUI lives inside it. There is no IPC and
no second binary to keep in step.

**Nothing blocks the UI.** The device is a blocking serial handle fed by a child
ffmpeg process, and calibrating a long video takes tens of seconds. So every
content call starts a native worker and returns immediately; C# polls a status
struct every 250 ms. Calibration is separately cancellable — without that,
picking a large video would wedge the UI until it finished.

Some consequences worth knowing if you touch this code:

- The core's **log sink is per-thread**. Background calibration runs alongside
  playback, and a global sink would interleave their progress into one garbled
  line.
- `jl::Device` is **internally locked at whole-message granularity**. That's why
  a brightness change from the UI thread lands cleanly between two video frames
  instead of halfway through a JPEG.
- The DLL mirrors the connection flag rather than calling `Device::IsOpen()`
  under the status lock — the core takes the status lock (via the log sink)
  while holding the device lock, so querying the other way round deadlocks.
- The native side plays **one item**; the playlist lives in C#. Timing-critical
  work stays in C++, scheduling stays where it's easy to write and persist.
- The **overlay is composited natively**, inside the frame path, because the
  panel has no overlay plane: every frame is decoded, blended and re-encoded on
  the way past. C# renders the overlay to a bitmap on its own thread and hands
  it over; C++ blends and re-encodes. Drawing is where WPF is good, and the
  frame path is where C++ has to be.

## Native API

```c
int32_t jl_open(const wchar_t* port);          // NULL = autodetect
void    jl_close(void);
int32_t jl_show_image(const wchar_t*, const JlRenderOpts*);   // async
int32_t jl_play_video(const wchar_t*, const JlRenderOpts*);   // async
int32_t jl_calibrate (const wchar_t*, const JlRenderOpts*);   // blocking, device-free
void    jl_stop(void);
void    jl_get_status(JlStatus*);              // poll this
int32_t jl_get_last_frame(uint8_t*, int32_t);  // the JPEG actually on the glass
```

Full contract in [`JLDisplayNative/jl_api.h`](../JLDisplayNative/jl_api.h). The C#
side checks `sizeof(JlRenderOpts) == 100` and `sizeof(JlStatus) == 1184` at
startup, so struct drift fails loudly instead of corrupting memory silently.

## Where things are stored

| Path | What |
|---|---|
| `%LOCALAPPDATA%\JungleLeopardDisplay\settings.json` | manager settings |
| `%LOCALAPPDATA%\JungleLeopardDisplay\library.json` | library and playlist |
| `%LOCALAPPDATA%\JungleLeopardDisplay\thumbnails\` | extracted video frames |
| `%LOCALAPPDATA%\JungleLeopardDisplay\downloads\` | videos fetched from YouTube |
| `%LOCALAPPDATA%\JungleLeopardDisplay\overlays.json` | overlay profiles and layers |
| `%LOCALAPPDATA%\JungleLeopardDisplay\overlay-assets\` | images used by overlay layers |
| `%LOCALAPPDATA%\JungleLeopardDisplay\music-art\` | album art and blurred backdrops, cache |
| `%LOCALAPPDATA%\JungleLeopardDisplay\music-cache.json` | which video matched which track |
| `%LOCALAPPDATA%\JungleLeopardDisplay\music-videos\` | downloaded music videos, swept by age and size |
| `%LOCALAPPDATA%\JungleLeopardDisplay\manager.log` | connection events, errors |
| `%LOCALAPPDATA%\jl_display\calibration.txt` | **shared** with the CLI |
| `%LOCALAPPDATA%\jl_display\packs\*.jlp` | preprocessed video frames, **shared** |
| `%LOCALAPPDATA%\jl_display\packs\*.jlf` | preprocessed stills, **shared** |

The calibration cache is keyed on each file's absolute path, size, timestamp and
filter chain, so re-encoding or replacing a video invalidates its entry
automatically — and a video calibrated in one program is instant in the other.

Packs are keyed on the same material plus the frame rate and either the
calibrated quality or the per-frame cap and byte budget, and the key is the
filename, so a stale pack can never be mistaken for a
current one — it simply stops being looked up and ages out. The CLI and the
manager share them the same way they share calibration.

## Repository layout

```
JLDisplayCore/        protocol, device I/O, ffmpeg, calibration cache, frame packs
JLDisplayNative/      flat C API over the core, for P/Invoke
JLDisplayManager/     WPF tray app (net9.0-windows, x64)
  Services/Ai/        SwarmUI client, LLM clients, prompt enhancer, pipeline
  Services/Music/     media-session follower, album art, video resolver
  Themes/             KruxSYS.xaml (every colour and shape token), Theme.cs
Jungle Leopard Display/   the CLI
JLDisplayCore.Tests/      native tests for the core (console exe, no framework)
JLDisplayManager.Tests/   xUnit tests for the manager
assets/brand/         KruxSYS logo and the product mark (SVG, plus PNG renders)
installer/            Inno Setup script and wizard art
tools/                make_icons.py (icons from the SVGs), release.ps1 (one-command release)
docs/                 these pages, brand.md, the audit, screenshots
```

---

[Back to the README](../README.md)
