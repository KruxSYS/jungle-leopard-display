# AGENTS.md

Instructions for working in this repo. Keep it true; delete stale or generic lines.

## What this is

Drive a **TZMRIT PF360 / "Jungle Leopard"** pump LCD (960×480 panel on an AIO
cooler head) from Windows. Windows 10/11, **x64 only**. Reverse-engineered
protocol (firmware 3.1). See `README.md` for full detail; this is the
hard-earned context an agent would otherwise guess wrong.

## Build & verify

No test project ships in the solution — verify by building, and for anything
touching the overlay, **by rendering it and looking at it**. That is not a
figure of speech: the overlay bugs that assertions missed and a rendered image
caught include an auto-scaled bar chart implying load on an idle GPU, a graph
fill drawing grey under a green line, and a glow that read as a halo at 44 px
and a blob at 22 px. Geometry assertions passed for all three.

Requires **Visual Studio 2022 (v143 toolset)** + **.NET 9 SDK**.

```sh
msbuild "Jungle Leopard Display.sln" -p:Configuration=Release -p:Platform=x64 -restore
```

- Output lands in `x64\Release\` (native + managed together) so `DllImport`
  resolves with no probing paths.
- **Solution quirk:** `Debug|x64` maps onto `Release|x64` for **all four**
  projects — inherited from the original `.sln`. So a plain solution build, in
  any configuration, produces fully optimised code.
- **A solution build never emits PDBs.** Native Release sets
  `DebugInformationFormat=None` / `GenerateDebugInformation=false`, and the
  manager sets `DebugType=none` unconditionally.
- The `x86` solution configuration builds the native projects as `Win32` but
  the manager as `x64` (it is `<Platforms>x64</Platforms>`).
- **Native DLL build-order gotcha:** `JLDisplayManager.csproj` pins
  `<SetPlatform>Platform=x64</SetPlatform>` on the `JLDisplayNative` vcxproj
  reference. Letting the platform float → MSBuild builds the vcxproj a second
  time under a different platform → `LNK1257` (front/back-end not compatible)
  in the shared intermediate dir.

## Native ↔ managed interop (touch both sides)

`JLDisplayManager/Interop/NativeMethods.cs` **blits** structs into
`JLDisplayNative.dll` — it is not field-by-field marshalled, so a layout drift
corrupts memory silently. `VerifyLayout()` checks `sizeof(JlRenderOpts)==100`
and `sizeof(JlStatus)==1184` at startup, turning drift into one clear exception.
(`JlStatus` was 1128 originally; 1144 with the playback-position doubles; 1168
with the four overlay diagnostics; 1184 now those carry the composed frame size
and the over-budget count. `JlRenderOpts` was 88 before the two per-frame
quality fields, 96 before its byte rate.)

If you change a struct in `JLDisplayNative/jl_api.h`, update the matching C#
struct **and** the expected sizes in `VerifyLayout()`.

## Runtime prerequisites

- **ffmpeg** — does all transcoding. `winget install "FFmpeg (Essentials Build)"`
  or drop `ffmpeg.exe` beside the binaries. Without it, only a pre-made ≤80 KB
  960×480 JPEG can show. `ffprobe` optional (else calibration reports elapsed
  time, not progress).
- **.NET 9 Desktop Runtime** — for the manager only; the CLI is native.

## One port owner

Only one process can hold the COM port at a time. **Don't run the CLI and the
manager together.** The manager's **Settings → Release device** hands the port
over. The tray app uses a process mutex (`Local\JungleLeopardDisplayManager.Instance`)
so a second launch hands over to the running instance instead of failing.

## Data paths

| Path | What |
|---|---|
| `%LOCALAPPDATA%\JungleLeopardDisplay\settings.json` | manager settings |
| `%LOCALAPPDATA%\JungleLeopardDisplay\library.json` | library + playlist |
| `%LOCALAPPDATA%\JungleLeopardDisplay\thumbnails\` | extracted video frames |
| `%LOCALAPPDATA%\JungleLeopardDisplay\downloads\` | videos fetched from YouTube |
| `%LOCALAPPDATA%\JungleLeopardDisplay\generated\` | AI-pipeline output |
| `%LOCALAPPDATA%\JungleLeopardDisplay\overlays.json` | overlay profiles |
| `%LOCALAPPDATA%\JungleLeopardDisplay\overlay-assets\` | images used by overlay layers |
| `%LOCALAPPDATA%\JungleLeopardDisplay\manager.log` | connection events, errors |
| `%LOCALAPPDATA%\jl_display\calibration.txt` | **shared** with CLI |
| `%LOCALAPPDATA%\jl_display\packs\*.jlp` / `*.jlf` | preprocessed frames, **shared** |

Calibration/packs are keyed on file path, size, timestamp, filter chain (+ frame
rate, and either the calibrated quality or the per-frame ceiling) so re-encoding
or replacing invalidates automatically, and a video calibrated in one program is
instant in the other.

## Architecture

```
JLDisplayCore   (static lib)  protocol · device I/O · ffmpeg · calibration · packs
  │
  ├── Jungle Leopard Display.exe    thin CLI (single self-contained exe)
  │
  └── JLDisplayNative.dll         flat C API, async workers
            │
            └── JungleLeopardDisplayManager.exe   WPF tray app (net9.0-windows, P/Invoke)
```

- Core is **static** so the CLI stays a single exe; the DLL exists only to give
  C# something to P/Invoke.
- **Nothing blocks the UI:** every content call starts a native worker and
  returns; C# polls a status struct every 250 ms. Calibration is separately
  cancellable.
- Core log sink is **per-thread**; `jl::Device` locks at whole-message
  granularity. DLL mirrors the connection flag rather than calling
  `Device::IsOpen()` under the status lock (status lock is taken via the log
  sink while holding the device lock — query the other way and it deadlocks).
- Native plays **one item**; the playlist lives in C#.
- **Overlays composite natively** (`JLDisplayCore/jl_overlay.cpp`, encoder in
  `jl_jpeg.cpp`, WIC): the
  panel has no overlay plane and no alpha, so every frame is decoded, blended
  and re-encoded in the frame path. C# renders to a premultiplied BGRA bitmap on
  its own STA thread and hands it over double-buffered with a version counter;
  C++ blends and re-encodes. Costs ~2 ms — **the overlay's cost is bytes, not
  time**; see below.

## Manager specifics

- Entry point: `JLDisplayManager/App.xaml.cs`. `ShutdownMode = OnExplicitShutdown`
  — closing the window hides it (panel keeps running); **Exit** on the tray stops
  the process.
- Playlist **and** AI pipeline drive the one panel; starting either stops the
  other (arbitrated in `App.xaml.cs`).
- AI pipeline is **off by default**; SwarmUI at `http://localhost:7801`. API keys
  DPAPI-encrypted, base64 in `%LOCALAPPDATA%\JungleLeopardDisplay\ai.json`.
- `--hwaccel` affects **decoding only** (no GPU MJPEG encoder on NVENC/AMF).

## Per-frame quality

The pack builder can encode each frame at the best quality *that frame* affords,
instead of at the one `-q:v` calibration picked from the busiest frame in the
file (`RenderOpts::perFrameQuality`, on by default in the manager). Measured on
a 30 s clip, SSIM against source: 0.91-0.95 uniform against 0.97-0.99 per frame,
in slightly fewer bytes.

**What a frame may spend is a data rate divided by the frame rate, NOT
kSafeJpegBytes.** This is the one thing to get right here, and the first version
got it wrong: it filled the 64 KB target on every frame, which is 1.4 MB/s at
30 fps, and the panel stuttered. 80 KB is what ONE frame may be, and it is not
30 times that per second.

**It is not the link, and nothing reports it.** Writes flat out measure
3.05 MB/s on this hardware with no frame's write blocking past 20 ms — the panel
takes everything offered. What runs out is its own decode, which scales with
JPEG size. It applies no backpressure and says nothing, so the host reports a
clean 30 fps and zero drops while the picture visibly hitches; `PlaybackStats`
cannot be trusted to notice this class of bug. Somebody has to look at the
panel. Calibration never had to know, since `-q:v` stops at 3 and lands near
37 KB a frame regardless — about 1.1 MB/s, which is what `kDefaultByteRate` is,
so a per-frame pack sends no more than the app always has and just spends it
better. `FrameBudget()` in `jl_core.h` is where the two limits meet, rate first.

**`FrameBudget()` is inline in `jl_core.h` and not local to the pack builder,
because everything that decides how big a frame may be has to come through it.**
It was private to `jl_pack.cpp` and the overlay compositor therefore never saw
it — see the overlay section. If you add a third thing that encodes a frame,
that thing gets the budget too, or the setting silently stops meaning anything.

The budget is a flat per-frame cap rather than a leaky bucket, so a busy frame
cannot borrow from a simple neighbour. A bucket would allocate better and is
the obvious next idea — but it makes the chosen quality depend on preceding
frames, which breaks the determinism the dedupe below needs. Flat already beats
uniform on every frame measured; that trade is not worth reopening lightly.

Three more things that are load-bearing:

- **The search must stay a pure function of the frame's pixels.** It walks a
  fixed ladder in `jl_jpeg.cpp` rather than bisecting WIC's float range, and it
  carries no state between frames — deliberately unlike the overlay compositor's
  rate control. Two identical frames have to encode to identical bytes or the
  pack builder's dedupe stops recognising them and packs balloon.
- **WIC's ImageQuality reads far lower than it encodes.** The ladder's floor was
  0.30, which sounds like nothing and is not: a dense 960×480 frame — foliage,
  gravel, anything with no flat region — measures **73 KB** at 0.30. A frame
  needing to reach a 39 KB budget could not get there, so `EncodeJpegToFit` took
  its documented fallback and shipped it oversized. In a real 66k-frame pack
  that was 15% of frames over budget, peaks of 1.85× the configured rate and one
  **50-second continuous overrun**. The floor is 0.05 now (35 KB for that same
  frame at 0.10). Never reason about these numbers from the 0..1 scale; measure.
- The deeper ladder costs encodes on dense content — about **5.5 per frame**
  against the 1.3 a ceiling of 84 used to average. That is preprocessing time,
  paid once, and is not a playback cost.
- **The quality ceiling is now the second constraint, not the first.** At the
  default rate the byte budget binds on nearly all content, and the ceiling only
  decides how good a frame simple enough to fit anyway may get. It is not in the
  UI for that reason; the data rate is.
- **A per-frame build does not calibrate at all**, so an uncached video now
  decodes once rather than twice. `jl_api.cpp` defers `ResolveQuality` to the
  streaming fallback, which still needs one figure for the whole run.

Frames come out of ffmpeg as raw BGR (`RawVideoCommand`) and every encode is
WIC's. Pinning `-pix_fmt bgr24` matters there for the same reason `yuvj420p`
matters on the mjpeg path: a GIF decodes to bgra, and without it the bytes
arrive in a different layout than the encoder reads.

## Overlays

The largest subsystem added since the original release. `docs/overlay-plan.md`,
`overlay-ai-plan.md` and `overlay-drawing-plan.md` carry the full record,
including the decisions that turned out wrong — read those before redesigning
anything here.

**The compositor answers to `FrameBudget()`, and has to be told what it is.**
`Overlay::SetBudget()` is called from `StartWorkerLocked` in `jl_api.cpp`; the
compositor sees frames, not `RenderOpts`, so nothing else would give it the
figure. It originally steered on `kMaxJpegBytes` — the 80 KB *per-frame* cap —
whose soft recovery threshold worked out to 49.5 KB a frame, i.e. **1.45 MB/s at
30 fps**, against a configured 1.15. Measured on real frames, a busy stretch
went out at **1667 KB/s with p90 pinned at the cap**, and on easy content the
recovery rule walked quality up to 0.91 and parked it there. That is the whole
of "it stutters worse with the overlay on". It cost ~2 ms of CPU a frame the
entire time — if you are looking at compose timings to explain an overlay
stutter, you are looking at the wrong number.

**Its constants are tuned for a target that binds.** The descent is
overshoot-proportional (`kMinStep`/`kMaxStep`) rather than a flat step, and
`kRecoverAfter` is 20 rather than 90. Both changed when the budget replaced the
cap: a flat 0.07 down against a limit that is now hit constantly dived to 0.14
and spent a busy stretch at 61% of the rate it was allowed, and recovering that
at 0.02-per-90-frames took the better part of two minutes. Retune together if
you touch either — the asymmetry between descent and recovery is what keeps it
from oscillating.

**Over budget is not a drop.** The budget is a rate; a gap costs more than one
expensive frame. Only `kMaxJpegBytes` decides what cannot be sent at all, and
`Stats::overBudget` counts the difference. A climbing `overBudget` with no drops
means the rate is set too low for the content, not that anything is broken.

**Layers are plain objects with no WPF types.** The renderer runs on its own STA
thread and WPF objects have thread affinity, so a `Brush` stored on a layer could
not be drawn by both the render thread and the editor. Colours are strings.

**Colours are role names, not hex.** `good`, `warm`, `track`, `panel`, `dim`,
`line` resolve through the profile's theme **at draw time**, which is what makes
switching theme restyle everything coherently. A literal `#RRGGBB` passes
through untouched. If you add a colour, add a role.

**The render thread must never touch the live layer list.** `OverlayService`
keeps a shallow copy (`OverlayProfile.ShallowCopy`) — a `Collection was modified`
crash came from exactly this, and `ShallowCopy` forgetting `Theme` was a second
bug on top.

**The skip signature is how frames are avoided.** ~80% of ticks draw nothing
because nothing visible changed. **A new layer type that shows live data must be
added to `OverlayService.Signature`** or it will never redraw. Keyed on the
formatted value, or on `SensorSnapshot.Version` for a graph.

**Sensor ids are permanent.** They end up saved in user profiles, so renaming
one breaks people's layouts.

**Undo is snapshot-based** (`Views/Overlay/EditHistory.cs`), so it covers any
property without knowing about it — which is the point, given how often layers
gain fields. Two rules when touching the editor: an edit must go through
`Commit()`, not `Save()` (`Save` is for things that are not edits — switching
profile, the master enable — and must not enter the history); and anything that
auto-repeats must pass a coalesce key, or a held arrow key costs thirty presses
of Ctrl+Z. Snapshots cover the profile list and the active id, never the enable
switch or the render rate.

### The AI path

`Services/Overlay/Ai/`. A prompt becomes a compact `LayerSpec` list, expanded by
`LayerFactory`, styled by `StyleApplier`, positioned by `LayoutEngine`.

**The model never writes pixel coordinates or colours** — only kinds, sensors,
anchors, sizes and named roles. That is the whole reason a small local model is
reliable enough to be useful. Widening the schema is the standing temptation and
the standing risk; every field added is one more thing to get wrong.

`OverlaySystemPrompt` is composed per call and is ~2300 tokens. It carries the
live sensor list, so a prompt can only ask for sensors this machine has.

Everything from the model is treated as hostile input: unknown values are
dropped with a note, numbers are clamped, an invented sensor drops the layer
rather than producing a dead one.

### Sensors

`Services/Sensors/`. PDH and NVML need nothing installed; **CPU temperature does
not exist as a Windows counter** and needs LibreHardwareMonitor (web server,
:8085) or HWiNFO shared memory, both opt-in.

`HardwareNames.Match()` maps vendor labels to ids and **its ordering is
load-bearing**: `tctl` must be checked before anything else, because a Ryzen
reports six labels containing "Tdie" and matching those gave a reading 12°C low.
GPU hotspot matches `hot` only — `junction` also appears in a VRAM label.

The registry polls on a **`System.Threading.Timer`**, i.e. the thread pool. Code
that floods the pool thins out the sensor history; this was measured, not
theorised.

## Stopping an item blocks the caller

`jl_show_image`, `jl_play_video` and `jl_play_stream` all begin with
`StopWorkerLocked()`, which sets `abortItem` and then waits `INFINITE` for the
worker. **Every one of them is called from the UI thread** — the playlist, the
tray, `ShowOne`, the music follower — so however long that wait is, the app is
frozen for it.

**"Checked per frame" is a promise the abort cannot keep on its own.** The play
loop spends nearly all its time blocked in `ReadFile` on ffmpeg's pipe, and only
tests the flag between frames. A source still producing frames stops in about
33 ms. A source producing *nothing* — a stalled network stream, a connection
being refused — never reaches the check at all, and the wait becomes ffmpeg's
own timeout. Measured with a firewalled ffmpeg and `SendMessageTimeout` against
the main window: **a 7.3-second UI freeze on every track change**, 0 ms after
the fix.

So `PlayVideo` takes a `ChildFn` and hands up the ffmpeg process handle as each
pass starts, `jl_api.cpp` keeps it in `Session::childProcess`, and
`StopWorkerLocked` terminates it *before* waiting. The pipe closes, `ReadFile`
returns, the loop unwinds through its ordinary path.

**The handle needs a lock, not just an atomic.** The worker withdraws it
(`onChild(nullptr)`) before `pipe.Close()`, and both that and the terminate run
under `Session::child` — otherwise a stop can call `TerminateProcess` on a
handle the worker is closing, which by then may name an unrelated process.

If you add another play loop, it gets the same treatment, or it reintroduces the
freeze.

## Following what's playing

`Services/Music/`, plus `JLDisplayNative/jl_media.cpp`. Reads the Windows media
session (SMTC) — whatever a browser tab or a music player is playing — and puts
it on the panel: the music video streamed live where one can be found, a card
built from the album art where one cannot. It **plays nothing and controls
nothing**; the browser stays the remote control.

`docs/music-plan.md` carries the design and the reasoning.

**The SMTC reader is native, and that is the whole reason it is native.** The C#
route to `GlobalSystemMediaTransportControlsSession` is the Windows SDK
projection, which means retargeting the manager to `net9.0-windows10.0.19041.0`
and shipping `Microsoft.Windows.SDK.NET.dll` + `WinRT.Runtime.dll` — about
twenty megabytes onto a drop that is under one. C++/WinRT is header-only, needs
only `windowsapp.lib`, and the release stays three binaries. Don't "simplify"
this back into C#.

**`JlMediaStatus` is 1832 bytes and is in `VerifyLayout()`** with the other two.

**A source app id is an identity, not a label.** Firefox reports itself as
`308046B0AF4A39CB`. Never parse it, never show it raw — `MediaSession.FriendlyName`
maps the ones worth naming and falls back to the id.

**YouTube Music in a browser frequently reports no track length at all.**
Measured on Firefox: some tracks report `EndTime` correctly (134.12 s for a 2:14
song), others report 0 for the whole track while the position advances perfectly.
So:

- Every `music.*` layer that needs a length is hidden when there is none, rather
  than drawing an empty bar and a `--`.
- `TrackResolver` cannot lean on the duration match alone. It has a title-and-
  artist path for exactly this case, and that path is **not** a rare fallback.

**Position, by contrast, is reliable.** The reader extrapolates it from
`LastUpdatedTime` (players refresh the timeline every few seconds, not every
frame), and it tracks wall clock to about 10 ms per second.

**The card is an ordinary overlay profile** with a fixed id
(`OverlaySettings.NowPlayingId`), added to the user's profile list so it shows in
the tray menu and can be restyled in the editor. Two consequences:

- The follower swaps the active profile **per mode, not per session**: the card's
  scrim covers all 960x480, so leaving it up over a video would draw the video
  through a dark sheet. Video gets whatever overlay the user had.
- Following must be stopped **before** `SaveAll()` in `ExitApp`, or the card is
  persisted as the profile the user had chosen.

**`ImageLayer.File` is a template now**, resolved through `TokenFormatter` at
draw time — that is how `{music.art}` puts the current cover on the panel. Two
things had to change with it, and both are the kind that fail silently:

- `OverlayService.Signature` appends the **resolved** path. Appending the
  template would mean the art never changed as far as the skip check was
  concerned.
- `OverlayRenderer`'s image cache is keyed on path and never expires, so the
  follower calls `ClearImageCache()` on every track change. Without it every
  cover played stays decoded in memory for the life of the process.

**The card's backdrop deliberately skips ffmpeg.** `MusicArt` writes a 960x480
baseline 4:2:0 JPEG under 80 KB, which `PrepareImage` sends as-is when
`rotate == 0` — so the card works on a machine with no ffmpeg at all, and
`ShowStill` passes rotate 0 for that reason (the backdrop is a blur; it has no
right way up, and everything legible is the overlay's, which rotates itself).
**JPEG quality is 80 because WIC abandons 4:2:0 above roughly 90**, and a 4:4:4
still is one the panel cannot decode.

**`jl_play_stream` is separate from `jl_play_video` on purpose.** A stream is
never packed and never calibrated — no file identity to key a pack on, and a
survey would fetch the whole thing a second time. Its URL and headers are a
separate argument rather than fields on `JlRenderOpts` because that struct is key
material for the calibration and pack caches; a signed URL that changes every
play would turn every cache entry into a miss.

**ffmpeg needs outbound network permission for the video path, and nothing says
so when it does not have it.** On this machine — default-deny outbound plus
GlassWire — ffmpeg fails with `Error number -138` (`ETIMEDOUT`) on *every* URL,
including plain HTTP to an IPv4-only host, while yt-dlp works fine because it has
its own rule. Symptoms look like a bad URL or an expiring signature and are
neither. Check `ffmpeg -i https://anything` before debugging the resolver.
Dropping `ffmpeg.exe` beside the binaries gives a stable path to allow, and is
the path `FindFfmpeg` prefers anyway.

**A stream reports "playing" before it has a picture.** `JL_STATE_PLAYING` is set
when the worker starts, not when the first frame lands, so a stream that never
connects is indistinguishable from one that is about to — except by
`framesSent`. The follower writes one off after `StreamStartTimeout` (12 s,
longer than ffmpeg's own `-rw_timeout` so ffmpeg normally gives up first) and
falls back to the card.

**Never correct drift while frames are not flowing.** A stalled stream's
position stands still while the song runs on, so the apparent drift grows
without bound and every correction restarts the decoder that is already stuck.
The log from before the guard reads `corrected +21.74s`, then `corrected
+41.65s` twenty seconds later — that shape means the stream is dead, not that
the tolerance is wrong.

**Sync is open loop and cannot be otherwise.** Two independent streams of one
song, and the browser's own buffering is a standing offset nothing here can
measure. Drift is corrected with `jl_seek` only past 1.5 s and at most once every
20 s, because a seek restarts ffmpeg and lands on a keyframe — correcting more
often looks worse than the drift. Corrections are logged; that log is the only
evidence there is.

## Sleep and wake

The panel is a USB device across a power transition and the reconnect loop
**cannot** see what goes wrong there, so `DisplayService` does not try to detect
it — it closes the port on suspend and again on resume, and lets the ordinary
reconnect loop do the retrying.

Why detection cannot work here is the point worth keeping:

- `MaybeReconnect` only runs when `Connected` goes false, and `Connected` only
  goes false when a **write fails**. With nothing playing there are no writes,
  so a stale handle is never noticed.
- Worse, the panel can come back with its COM port still present and the handle
  still valid, having reset itself while the host slept. Writes keep succeeding,
  the host reports a clean connection at 30 fps, and the picture never moves.
  This is the same blindness that makes the data rate unmeasurable from the
  host: the panel accepts everything and reports nothing.

Confirmed from the logs before the fix — sleep at 20:31:40, resume at 20:31:43,
and not one line logged afterwards on any day the machine slept.

**It uses `PowerRegisterSuspendResumeNotification`, not
`SystemEvents.PowerModeChanged`.** .NET does not map `PBT_APMRESUMEAUTOMATIC`
onto `PowerModes.Resume`, and that is the notification Windows sends for *every*
wake; `PBT_APMRESUMESUSPEND` only follows when a person woke the machine. On
SystemEvents alone, a box that wakes itself for maintenance stays stuck. The two
resume notifications usually both arrive and are collapsed on a 5-second window
— on time rather than on having seen a suspend, since a resume without one has
to still work.

Two things that will bite if you touch it:

- The callback delegate is held in a field. The OS keeps the raw pointer, so a
  collected delegate is a crash at the exact moment the machine sleeps.
- The suspend handler runs the close **inline on the callback thread**, not
  through the dispatcher. Windows allows a suspend handler only a couple of
  seconds; that is enough to close the port but not to also wait for the UI
  thread to come free.

Restoring content after the reconnect is four separate paths, because there are
four things that can own the panel: `PlaylistPlayer`, `AiPipeline` and
`MusicFollower` each re-subscribe themselves to `Reconnected`, and `App` covers
the item shown by `ShowOne`, which has none of them running and would otherwise
have nobody to put it back. That last one lives in `App` for the same reason the rest of the panel
arbitration does — it is the only place that knows about both.

## Frame pacing

`detail::Pacer` (`jl_internal.h`, defined in `jl_player.cpp`) is shared by
streaming and pack playback. Two things about it are load-bearing:

- **Never time it with `GetTickCount`.** That is what it did, and `GetTickCount`
  only advances every ~15.6 ms however fine `Sleep`'s resolution happens to be,
  so every slot quantised onto a tick boundary. Measured over 300 frames at
  30 fps: gaps bimodal at 31 ms and 46 ms, **16.8 ms of jitter — half a frame
  period** — with 14% of frames more than 5 ms out. That is visible on the panel
  at any bitrate, and it is why a data-rate fix alone did not stop the stutter.
  It is QPC plus a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer now: 1.35 ms
  jitter, nothing outside ±5 ms, zero drift over 300 frames.
- **The slot comes from the frame counter, not an accumulated period.**
  `1000/30` is 33, and that missing third of a millisecond is a whole frame of
  drift every 100 seconds.

The high-resolution timer needs Win10 1803; the fallback raises the global timer
period with `timeBeginPeriod(1)` and puts it back in the destructor. Never both
— that one is system-wide and costs power everywhere.

## Gotchas

- **`pwsh.exe is not recognized`** at build = vcpkg's `applocal.ps1` falling back
  to `powershell.exe`. Harmless; install pwsh to quiet it.
- **Changing what a frame encodes to means bumping `kPackVersion`.** The pack
  key is built from the source, the filter, the fps, the ceiling and the budget
  — none of which move when the *ladder* changes, so an existing pack would
  otherwise be reused with frames built under the old rules. `kPackVersion` is
  in the key material and in the header, so bumping it retires old packs both
  ways. It went 2 → 3 when the ladder floor dropped. Old `.jlp` files are not
  deleted, only ignored; they age out against the disk budget.
- Per-item rotation/stretch overrides exist in the model, honoured at playback,
  but have **no UI** yet.
- **ComboBox items render through a generated `TextBlock`**, which picks up the
  app's implicit `TextBlock` style — an explicit setter that beats anything
  inherited, so combos render as invisible text. The fix is the `ComboText`
  `DataTemplate` in `App.xaml`, **not** a `Foreground` on the ComboBox. That was
  tried first and does nothing.
- **Do not load `App.xaml` as a `ResourceDictionary`** in a test harness: it is
  the `ApplicationDefinition`, so parsing it constructs a second `App` and WPF
  throws "cannot create more than one Application instance". Instantiate `App`
  and call `InitializeComponent()`.
- Overlay text effects cost roughly the same to encode (outline, glow, pill are
  all within ~1 KB against an 80 KB cap). Earlier comments claiming outlines were
  expensive were **wrong** and are corrected; do not reintroduce that advice.
- The Anthropic client is raw HTTP (not the SDK) to keep the portable drop free
  of third-party assemblies.
- `.gitattributes` forces **CRLF** on `*.sln`, `*.vcxproj`, `*.filters`,
  `*.csproj`, `*.props`, `*.targets`, `*.manifest`;
  `*.png/.ico/.jpg/.dll/.lib/.exe/.pdb` stay binary.
