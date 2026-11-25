# Audit, release 1.7.0

A review of all four projects (`JLDisplayCore`, `Jungle Leopard Display` (CLI), `JLDisplayNative`,
`JLDisplayManager`) done for the 1.7.0 KruxSYS release. Every finding is listed with where it is, how
bad it is, the evidence, and what happened to it. Line numbers are as of the fix commit, or of
release 1.7.0 for anything deferred.

**Hardware.** The panel was not available for this release. Everything below was built and run on a
Windows 11 VM with no device attached. Nothing that depends on the panel's behaviour (protocol,
pacing, what a frame looks like on the glass) is claimed as verified. Fixes on a playback path are
marked **needs hardware verification**.

## Method

1. **Compiler.** Native projects rebuilt at `/W4` (`_CL_=/W4` over a solution rebuild). Result: no
   code warnings in any of the three projects and no suppressions anywhere in the tree (`#pragma
   warning`, `NoWarn`, `#nullable disable`: none). The shipped level was raised to `/W4` to keep it
   that way (`ea1a830`). The manager builds with `<Nullable>enable</Nullable>` and produced
   **0 warnings**, nullable included.
2. **Static analysis.** `semgrep --config p/default --config p/csharp --config p/c` over all four
   projects: 97 files scanned, **0 findings**. One parse error (`jl_api.h`, a C header semgrep's
   C grammar rejects), not a code problem.
3. **Reading, by checklist**, in this order of priority:
   - the work carried into this release from the previous session (music follow: build a pack behind
     playback and swap onto it, `MusicPrepare`, `jl_preprocess_frames`, the pack file handle, the
     ladder hint),
   - native/managed interop: struct layouts, strings, buffers and delegates handed to native code,
   - threading: the `cmd`/`state`/device lock order, the 250 ms poll, cancellation and stop paths,
   - child processes: ffmpeg, ffprobe, yt-dlp, schtasks (quoting, inheritance, pipes, timeouts),
   - persistence: settings/library/overlay/AI JSON, the caches, atomic writes, disk growth,
   - DPAPI, the AI HTTP clients, the music follower,
   - handle/GDI/WIC lifetimes, tray icon and timer disposal,
   - behaviour with no device and on disconnect.
4. **Running it.** The manager was launched with no device and every window opened; corrupt-file,
   first-run and CLI path cases were reproduced on the VM before and after their fixes.

## Summary

| Severity | Found | Fixed | Deferred |
|---|---|---|---|
| Critical | 0 | 0 | 0 |
| High | 3 | 3 | 0 |
| Medium | 9 | 9 | 0 |
| Low | 12 | 0 | 12 |

## Findings

### High

| # | Where | Category | Status |
|---|---|---|---|
| H1 | `JLDisplayCore/jl_ffmpeg.cpp:54` `StartFfmpegPipe` | threading / deadlock | **fixed** `9fc6445`, needs hardware verification |
| H2 | `JLDisplayNative/jl_api.cpp:344` `ResolvedAuto` (was inline in `OptsFromApi` and `StartWorkerLocked`) | threading / data race | **fixed** `a369146` |
| H3 | `JLDisplayManager/Models/AppSettings.cs:488` `Storage.Load` | correctness / data loss | **fixed** `f18095f` |

**H1. A child ffmpeg could inherit another pipe's write end, hanging stop.**
`StartFfmpegPipe` created ffmpeg with `bInheritHandles=TRUE` and no handle list, so the child received
*every* inheritable handle in the process. That includes the write end of a pipe another thread had
just created for its own ffmpeg. Two pipes in flight at once is routine since the previous session:
the music follower streams a video while it builds a pack of the same video behind it
(`RawVideoCommand` and `StreamCommand`). A pipe whose write end has leaked into an unrelated process
cannot reach EOF until that process exits. The play loop then blocks in `ReadFile`, and
`StopWorkerLocked` waits for the worker with `INFINITE` (`jl_api.cpp:610`), from the UI thread and from
the suspend handler. The result is a UI freeze lasting as long as the other build, and a suspend
handler that overruns its two seconds. AGENTS.md already records one instance of this class (the pack
file handle leaking into ffmpeg); this was the general case.
*Fix:* an explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` naming only this call's stdout pipe and
inheritable duplicates of stdin and stderr.
*Residual:* the window between `CreatePipe` and `CloseHandle(writeEnd)` is still visible to .NET's own
`Process.Start` with redirection (thumbnails, yt-dlp), which inherits everything. It is microseconds
wide, and those children are short-lived.
*Verification:* covered by the native test `pipe_not_inherited` (an inheritable pipe created by the
test must still see EOF while a long-lived child started through `StartFfmpegPipe` runs). Playback
end to end needs hardware verification.

**H2. Unsynchronised `std::wstring` write in the hwaccel resolve.**
"auto" hwaccel is resolved once per process into `g.hwaccelResolved` / `g.hwaccelKnown`. That was
read and written without a lock from three threads: the UI thread (`StartWorkerLocked`, under
`cmd`), plus `jl_preprocess` and `jl_pack_ready`, which take no session lock *by design*. The music
follower calls `jl_pack_ready` from its prepare task and `jl_preprocess` from the pack thread while
the UI thread starts the stream (`MusicFollower.cs:698`, `:771`). With the default `auto`, the first
of them to arrive could be writing the string while another read or wrote it: heap corruption.
*Fix:* `ResolvedAuto()` resolves under a dedicated `CRITICAL_SECTION` and returns a copy.

**H3. An unparseable settings or library file was overwritten with defaults.**
`Storage.Load` caught the parse error, logged it and returned `null`, so the app came up on defaults.
The next `Save` (any edit, or quitting, which calls `SaveAll`) then replaced the file. A
`library.json` that failed to parse once, through a crash mid-write on an old version, a disk fault
or a file from a newer release, was gone for good.
*Fix:* the unreadable file is renamed to `<name>.corrupt-<yyyyMMdd-HHmmss>` first, and the log names it.
*Verification:* reproduced on the VM with a truncated `library.json`: the app started, the log reads
`could not read library.json: … - kept it as library.json.corrupt-20261125-153547`, and the file was
intact beside it. Covered by `StorageTests`.

### Medium

| # | Where | Category | Status |
|---|---|---|---|
| M1 | `JLDisplayManager/Services/ThumbnailService.cs:150` | resource leak / hang | **fixed** `8d72226` |
| M2 | `JLDisplayManager/Services/YoutubeService.cs:82` `YtDlp.Runs` | hang | **fixed** `fb98555` |
| M3 | `JLDisplayManager/Services/Music/MusicVideoCache.cs:164`, `YoutubeService.cs:339` | correctness / security | **fixed** `2f0a32a` |
| M4 | `JLDisplayManager/Services/Music/MusicFollower.cs:749` `StartPacking` | threading / performance | **fixed** `0ccf6b8`, needs hardware verification |
| M5 | `Jungle Leopard Display/Jungle Leopard Display.cpp:88` | correctness | **fixed** `a154b0b` |
| M6 | `JLDisplayCore/jl_util.cpp:149` `ValidateOpts` | security (hardening) | **fixed** `1092d27` |
| M7 | `JLDisplayManager/Models/AppSettings.cs:557` `Storage.Log` | disk growth / correctness | **fixed** `6101efd` |
| M8 | `JLDisplayNative/jl_api.cpp:24` | interop / layout | **fixed** `01effae` |
| M9 | `JLDisplayManager/App.xaml.cs:167` | UX | **fixed** `bb9a057` |

**M1. Thumbnail ffmpeg could block forever on its own stderr.** stdout and stderr were redirected into
pipes nothing read, and the wait had no timeout. A damaged video that logs more than a pipe buffer of
decode errors left ffmpeg blocked on the write: the thumbnail never appeared, and the process held the
source file open until the app exited. *Fix:* no redirection, `-nostdin`, and a kill after 30 s.

**M2. The yt-dlp `--version` probe could hang.** Same shape: redirected, unread, unbounded
`WaitForExit()`. *Fix:* no redirection, and the probe is killed and treated as "not found" after 20 s.

**M3. A video id or URL beginning with `-` was parsed by yt-dlp as an option.** The follower passed
the id as yt-dlp's first argument. YouTube ids are 11 of `[A-Za-z0-9_-]`, and about 1 in 64 start
with `-`. yt-dlp rejected those as an unknown option, so that song's video could never download. The
download dialog passed a pasted URL in the same position, and yt-dlp has `--exec`. *Fix:* `--` before
the id or URL, and ids are checked against the YouTube id shape before use (`IsVideoId`), since the id
also names a cache file. `ArgumentList` was already quoting correctly; quoting was never the problem.

**M4. A pool thread was left at below-normal priority.** `StartPacking` set
`Thread.CurrentThread.Priority = BelowNormal` inside `Task.Run` and never restored it. That pool thread
then ran every later job below normal, including the sensor registry's timer callbacks, which
AGENTS.md records as sensitive to pool starvation. It also parked a pool thread for a minute-long
blocking build. *Fix:* a dedicated background thread at below-normal priority.

**M5. The CLI could not open a file with a non-ASCII name.** `main`'s `argv` is in the ANSI code page,
and `Widen` decoded it as UTF-8, so `Música.mp4` or any Cyrillic or CJK path arrived as replacement
characters. *Fix:* paths, the port and the hwaccel name come from `CommandLineToArgvW`.
*Verification:* on the VM, `--image "Música-ж.jpg"` (ж is not in cp1252) now reads the file and stops
at "Device not found".

**M6. The hwaccel value reached ffmpeg's command line unvalidated and unquoted.** `-hwaccel <value>`
with a space in the value becomes extra ffmpeg arguments, an output file among them. The value comes
from the user's own settings or command line, so no privilege boundary is crossed, but a damaged
settings file should give an error, not an unintended ffmpeg invocation. *Fix:* `ValidateOpts` accepts
only `[a-z0-9_]{0,31}`.

**M7. `manager.log` grew without bound, and concurrent appends lost lines.** *Fix:* it rolls to
`manager.log.1` at 2 MB, and appends are serialised.

**M8. Native struct layout drift was invisible.** `VerifyLayout()` checks the *managed* sizes against
fixed numbers, which catches a change on the C# side only. *Fix:* `static_assert`s on the same three
sizes in `jl_api.cpp` (100 / 1184 / 1832). Layouts are unchanged.

**M9. The first launch after install showed nothing.** `StartMinimised` defaults to on, so a fresh
install started hidden, with the tray icon in Windows 11's overflow area, and looked like it had not
started. *Fix:* with no `settings.json` yet, the window is shown. The logon task passes `--startup`
and is unaffected. *Verification:* on the VM.

### Low (all deferred)

| # | Where | Category | Why deferred |
|---|---|---|---|
| L1 | `JLDisplayCore/jl_pack.cpp:120`, `jl_calibrate.cpp:69` | cache key | Path characters are truncated to their low byte in the key material. A collision would also need identical size and 100 ns mtime, and changing the key would invalidate every user's packs and calibrations. |
| L2 | `JLDisplayCore/jl_pack.cpp:1215` `PrepareImageCached` | correctness | A `.jlf` still is written in place, not atomically, and read back on size alone. Only a crash mid-write of a ≤80 KB file triggers it, and it heals the next time the source changes. |
| L3 | `JLDisplayManager/Models/AppSettings.cs:538` `Storage.Save` | correctness | Every save of one file uses the same `.tmp` name. Every caller found is on the UI thread, so nothing races today. |
| L4 | same | durability | The temp file is not flushed to disk before the move, so a power cut right after a save can leave an empty file. H3 now keeps such a file instead of losing it. |
| L5 | `JLDisplayManager/Services/Music/TrackResolver.cs:377` | correctness | The resolve cache is written in place, not atomically. It is a cache; the cost of losing it is re-searching. |
| L6 | `JLDisplayManager/Services/StartupTask.cs:180` | hang (theoretical) | Reads stdout to the end before stderr. schtasks writes a line or two. |
| L7 | `JLDisplayNative/jl_api.cpp:891` `jl_preprocess` | correctness | Leaves its per-thread log sink installed on the caller's thread, and progress is one process-wide counter. The one caller now uses a dedicated thread that ends with the build (M4). |
| L8 | `JLDisplayCore/jl_pack.cpp:970` | disk | A build killed by exiting the app leaves an unstamped `.jlp` under its final name. `AdoptFile` rejects it, the next build of that key truncates it, and LRU eviction counts it. |
| L9 | `JLDisplayManager/Services/Ai/AiPipeline.cs:456` | data loss (minor) | A generated image enters the library but is not saved until the next `SaveAll`, so a crash forgets it. The file stays in `generated\`. |
| L10 | `JLDisplayCore/jl_calibrate.cpp:231` | performance | `jl_calibrate` with "auto" launches `ffmpeg -hwaccels` on every call rather than using the session's resolved value. |
| L11 | `JLDisplayCore/jl_ffmpeg.cpp:90` | UX (CLI) | ffmpeg inherits the console's stdin, so keys typed into the CLI's console can reach it. Unchanged behaviour, kept so the CLI's existing use is not altered. |
| L12 | `JLDisplayManager/Services/Music/MusicFollower.cs:698` | performance | With per-frame quality turned **off**, `PackReady` resolves a calibrated quality to name the pack, which surveys the file the first time. |

## Checked and found sound

Recorded so the next audit need not redo them:

- **Lock order** (`jl_api.cpp`). `cmd` → `child` / `state`. The worker takes `state` through the log
  sink while holding the device lock, and nothing takes the device lock while holding `state`
  (`connected` is mirrored for this reason). No new path inverts it.
- **The 250 ms poll** (`DisplayService.Poll`). `jl_get_status` copies under `state` only and never
  blocks on an operation. `jl_seek` and the overlay calls take no session lock.
- **Stop paths.** `StopWorkerLocked` terminates the child before waiting (the 1.6 fix), and with H1
  fixed the wait ends when the child does.
- **Interop.** Struct fields and offsets match the header, including the 8-byte alignment of
  `framesSent`. Strings are `ByValTStr` with null-terminated copies capped by `CopyTo`. Every
  buffer-returning call (`jl_get_last_frame`, `jl_media_thumbnail`, `jl_media_sessions`) takes a
  size-query/copy pair and checks the length. No delegate crosses into native code except the power
  callback, which is held in a field and unregistered before release
  (`DisplayService.cs:631`). Pack-build progress is polled, so no callback lifetime is involved.
- **DPAPI** (`Secrets.cs:102`). User scope, `CRYPTPROTECT_UI_FORBIDDEN`, the input buffer freed with
  `FreeHGlobal` and crypt32's output with `LocalFree`. Failures read as "no key set".
- **AI HTTP.** Every call is bounded by a linked `CancellationTokenSource`. Timeouts, non-2xx
  responses and bad JSON become typed exceptions, and the pipeline keeps running after one.
- **Serial I/O** (`jl_device.cpp:148`). Write timeout 5 s, reads bounded, each message sent under the
  device lock.
- **Pack files.** Opened `"wbN"` with `_SH_DENYWR` (no inherit, readers allowed), magic stamped last,
  and LRU eviction that never removes the pack about to be mapped.
- **Cache keys.** The pack key covers source path, size, mtime, filter, fps, quality or ceiling,
  per-frame budget, panel size and `kPackVersion`. The calibration key covers path, size, mtime,
  filter and size target. hwaccel is deliberately in neither (decode only).
- **No device.** The manager starts, shows "Not connected / device not found", and polls for the
  port every 2 s with `jl_find_port` before attempting an open, so the unplugged case stays quiet.
  Every window opens. Content calls return `JL_ERR_NOT_OPEN` without blocking.
- **Exit.** Following stops before `SaveAll` (so the card is never saved as the user's profile),
  then the overlay, the display session and the tray icon are disposed in that order.

## Needs hardware verification

To check on a real panel before calling 1.7.0 verified:

1. **Music follow, play then build then swap** (carried in from the previous session): a new song
   starts streaming within seconds, the card's bar shows "Preparing video" and a percentage, and the
   swap onto the pack lands at the song's position without a visible jump.
2. **H1**: stopping or changing the item while a pack builds behind playback returns at once.
3. **M4**: playback stays smooth while a pack builds (the build is on its own below-normal thread).
4. Everything else in this release (theme, installer, tests, the other fixes) is host-side and was
   verified without the panel.
