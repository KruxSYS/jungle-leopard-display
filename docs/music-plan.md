# YouTube Music on the panel — implementation plan

Follow whatever YouTube Music is playing in the browser and put it on the glass:
the music video streamed live where one can be found, and a now-playing card
(album art, title, artist, progress) where one cannot.

Status: **built.** Phases 1-3 are in; phase 4 is not. This document is the
design and the record of what changed once it met real hardware — see §12, which
is the part worth reading before touching any of it.

---

## 0. What was decided before anything was designed

Three choices, made deliberately, that the rest of this document assumes:

1. **We follow; we do not play.** YouTube Music keeps running in the browser,
   the browser keeps making the sound, and this app never touches audio. What
   we get from that arrangement is a *title and a position*, not a URL — which
   is the single hardest fact in this whole design (§5).
2. **Video first, card as fallback.** Most YTM tracks are audio-only in the
   browser even when an official video exists, so the card is not an edge case
   and cannot be an afterthought.
3. **No discovery UI.** No search box, no library, no login. The browser is the
   remote control. The app has one switch: follow, or don't.

---

## 1. The constraints that shape it

**The panel takes JPEGs and nothing else.** 960×480, 4:2:0, under
`kMaxJpegBytes`, at a data rate the panel never reports and never complains
about — `FrameBudget()` in `JLDisplayCore/jl_core.h` is where that is decided,
and a live stream has to come through it like everything else.

**There is no audio, and there is no clock we share with the browser.** Two
independent streams of the same song, one of which we cannot observe. Everything
in §7 about drift follows from that.

**The fast path does not apply.** The manager's picture quality comes from
preprocessing a file into a `.jlp` pack, keyed on path, size and mtime
(`jl_pack.cpp:128`), after a calibration pass that surveys the whole source
(`jl_api.cpp`, the `calibrate` lambda). A live URL has neither an identity to
key on nor a file to survey twice. Streaming mode is the only mode available
here, and phase 1 of this plan must force it rather than let `GetPack` fail its
way into it.

**A seek costs a visible hitch.** `jl_seek` on a streamed item restarts ffmpeg
at the mark and lands on the nearest keyframe before it (`jl_api.h`). It is the
only tool we have for correcting drift, and it is not a tool you want to use
every few seconds.

---

## 2. Architecture

```
  Browser (music.youtube.com)
        │  Windows media session (SMTC)
        v
  JLDisplayNative — C++/WinRT reader ─────> jl_media_get / jl_media_thumbnail
        │                                              │
        │                                              v
        │                              MediaSessionProvider : ISensorProvider
        │                                              │
        │                                     SensorRegistry  ──> overlay tokens
        │                                                          {music.title} …
        v
  MusicFollower (C#)  ── third owner of the panel, beside PlaylistPlayer and AiPipeline
        │
        ├── TrackResolver ── yt-dlp ──> video id (cached) ──> direct URL (never cached)
        │                                     │
        │                                     v
        │                          jl_play_stream(url, inputArgs, opts)
        │
        └── no video ──> art cache (ffmpeg blur) ──> jl_show_image + "Now playing" overlay
```

Nothing here is a new subsystem. The sensor registry, the overlay renderer, the
still path and the streaming player all already exist; this feature is a reader,
a resolver, and one new native entry point.

---

## 3. Reading the session — native, C++/WinRT

**Why native.** The C# way to reach `GlobalSystemMediaTransportControlsSession`
is the Windows SDK projection, which means moving the manager to
`net9.0-windows10.0.19041.0` and shipping `Microsoft.Windows.SDK.NET.dll` and
`WinRT.Runtime.dll` — roughly twenty megabytes onto a portable drop that is
currently under one. C++/WinRT is header-only, already in the SDK the native
projects target (`WindowsTargetPlatformVersion 10.0`, `stdcpp17` — C++/WinRT
needs C++17 and gets it), links against `windowsapp.lib`, and adds nothing to
the drop. The release stays three binaries.

**Shape.** A worker thread initialises an MTA apartment, calls
`GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get()`, and
holds the manager for the process lifetime. Subscribe to `CurrentSessionChanged`,
and on the chosen session to `MediaPropertiesChanged`, `PlaybackInfoChanged` and
`TimelinePropertiesChanged`. Each event refreshes a snapshot behind a lock; the
flat C getter copies it out. Same discipline as the rest of the DLL: nothing
blocks, the caller polls.

```c
    typedef struct JlMediaStatus {
        int32_t present;          // a session is being followed
        int32_t playing;          // 0 paused/stopped, 1 playing
        double  position;         // seconds, extrapolated — see below
        double  duration;         // seconds, 0 when unknown
        int32_t thumbnailVersion; // bumps when the art changes
        wchar_t title[256];
        wchar_t artist[256];
        wchar_t album[256];
        wchar_t sourceApp[128];   // "chrome.exe", "msedge.exe", …
    } JlMediaStatus;

    JLAPI int32_t jl_media_start(const wchar_t* preferredApp);  // NULL = auto
    JLAPI void    jl_media_stop(void);
    JLAPI void    jl_media_get(JlMediaStatus* out);
    JLAPI int32_t jl_media_sessions(wchar_t* buf, int32_t cch);  // NUL-separated app ids
    JLAPI int32_t jl_media_thumbnail(uint8_t* buf, int32_t cap); // encoded bytes, as jl_get_last_frame
```

**`JlMediaStatus` is blitted like the others.** `NativeMethods.VerifyLayout()`
checks `sizeof(JlRenderOpts)` and `sizeof(JlStatus)` at startup precisely so a
layout drift is one clear exception instead of silent corruption — this struct
joins that check the day it is added. Fixed-size arrays, no pointers, padded to
an 8-byte multiple.

**Which session.** Sessions are identified by `SourceAppUserModelId`, which for
the browser is just `chrome.exe` or `msedge.exe` — it does not say *which tab*.
So: enumerate sessions, expose the list to Settings, and let auto mode prefer a
browser session that is currently playing, with an explicit override for a
machine that also runs Spotify or VLC. A wrong-session bug is invisible from the
code and obvious from the panel, which is an argument for surfacing what we
picked in the UI (§8).

**Position is extrapolated, not read.** `GetTimelineProperties()` gives
`Position`, `EndTime` and `LastUpdatedTime`; browsers refresh it every few
seconds, not every frame. The reader reports
`Position + (UtcNow − LastUpdatedTime) × PlaybackRate` while playing, clamped to
`EndTime`, and reports `Position` unmodified while paused. Do this in the native
reader, once, rather than in three callers.

---

## 4. Sensors and the card — no new overlay machinery

`ISensorProvider` already has `PublishText` for "a reading that is text by
nature — a clock, an uptime, **a track title**". The plumbing was built for this.

`Services/Sensors/MediaSessionProvider.cs` publishes:

| id | kind | notes |
|---|---|---|
| `music.title` | text | |
| `music.artist` | text | |
| `music.album` | text | often empty from the browser |
| `music.source` | text | "YouTube Music", or the app id |
| `music.position` | number | seconds |
| `music.duration` | number | seconds, 0 when unknown |
| `music.progress` | number | 0–100, so a `BarLayer` works with no new layer type |
| `music.playing` | number | 0/1, usable as a `VisibleSource` |

**These ids are permanent** — they end up in saved user profiles, and renaming
one breaks somebody's layout. Get them right the first time.

What falls out for free: overlay text tokens, the editor's sensor list, the
token picker, and the AI overlay generator (`OverlaySystemPrompt` composes the
live sensor list per call, so "put the track title bottom-left" starts working
with no prompt changes).

**The skip signature.** ~80% of overlay ticks draw nothing because
`OverlayService.Signature` says nothing changed. Text and numeric sensors are
already covered by it — but album art is an `ImageLayer`, keyed on a file path,
and a card whose art never changes because the path never changed is exactly the
class of bug that section of `AGENTS.md` warns about. Art is written per track
as `<hash>.jpg` in a new `music-art\` cache directory, and `ImageLayer.File` goes
into the signature if it is not there already.

**The card is composition, not a new renderer.** Album art out of
`jl_media_thumbnail`, run through ffmpeg once
(`scale=960:480:force_original_aspect_ratio=increase,crop=960:480,boxblur=20,
eq=brightness=-0.2`) into a cached background still, shown with `jl_show_image`,
with a built-in "Now playing" overlay profile drawn on top: art thumbnail, title,
artist, a progress bar on `music.progress`. Every piece of that already exists
and is already tuned.

---

## 5. Finding the video — the hard part

**SMTC gives a title, not a URL.** Everything below is best-effort, and the
design admits it rather than pretending otherwise.

1. **Exact, when we can get it.** Chrome and Edge expose the address bar through
   UI Automation, and the WPF stack already brings `UIAutomationClient` in
   (verify before relying on it). If the foreground tab reads
   `music.youtube.com/watch?v=<id>`, that is the answer with no guessing. It
   only works while the YTM tab is in front, so treat it as an opportunistic
   upgrade that seeds the cache — never as the mechanism.

2. **Search, with a duration check.** `yt-dlp "ytsearch5:<artist> <title>"
   --print id --print duration --print title --skip-download`. Accept the first
   candidate whose duration is within ±3 s of the session's `EndTime` and whose
   title contains the artist. **The duration check is what makes this usable** —
   without it, the first hit for a popular song is as likely to be a lyric video,
   a live cut, or an eight-hour loop as the official one.

3. **Nothing matched** → the card. This is a normal outcome, not a failure, and
   must not produce an error dialog.

**Cache the id, never the URL.** `%LOCALAPPDATA%\JungleLeopardDisplay\music-cache.json`,
keyed on `artist|title|album|duration`, holding the resolved video id or a
"no video" sentinel with a timestamp so a miss is not re-searched on every play.
Direct googlevideo URLs expire in hours: resolve one per play with
`yt-dlp -g -f "bv*[height<=?720][protocol^=https]/bv*[height<=?720]"`, and pull
the user-agent alongside it with `--print "%(http_headers.User-Agent)s"` — some
formats 403 without it.

Resolution runs off the UI thread and takes a second or two. The card shows
immediately on a track change and the video replaces it when the answer arrives;
that ordering also means a resolver that is slow or broken degrades to the card
rather than to a blank panel. `YoutubeService` already knows how to find yt-dlp,
point it at our ffmpeg, supply a JS runtime, and suppress the JS-runtime
advisory so it never masquerades as the real error — reuse it, don't re-solve it.

---

## 6. Streaming it — native

```c
    // Plays a live URL. Never packed, never calibrated: a stream has no file
    // identity to key a pack on and no second pass to survey.
    JLAPI int32_t jl_play_stream(const wchar_t* url,
                                 const wchar_t* inputArgs,
                                 const JlRenderOpts* opts);
```

**A separate call, not a field on `JlRenderOpts`.** That struct is blitted,
size-checked, and feeds the calibration and pack cache keys — a URL and a
user-agent must not enter that key material, and growing it costs a
`VerifyLayout()` change for something that is not a rendering option.

Inside: a third `ItemKind`, `Preprocess::Off` forced for the item, the
`calibrate` lambda skipped entirely, and a new `StreamCommand` beside
`VideoCommand` and `RawVideoCommand` in `jl_ffmpeg.cpp` — new rather than four
more parameters on `VideoCommand`, which already takes eleven. It adds
`-user_agent`, `-headers`, `-reconnect 1 -reconnect_streamed 1
-reconnect_delay_max 5`, and `-ss <position>` to join a song already in progress.

**Quality without a survey.** Two options, and the phasing matters:

- *Phase 3:* a fixed `-q:v 3`. Calibration stops at 3 and lands near 37 KB a
  frame regardless, which is about `kDefaultByteRate` — so this sends no more
  than the app already does. One line, ships immediately.
- *Phase 4:* route the live path through `RawVideoCommand` and encode each frame
  under `FrameBudget()`, steered by rate control. Note the deliberate difference
  from the pack builder: its ladder is a **pure function of the frame's pixels**
  because dedupe requires two identical frames to produce identical bytes. A
  live stream has no dedupe, so the **stateful, hysteresis-based** control in
  `jl_overlay.cpp` — overshoot-proportional descent, `kRecoverAfter` — is the
  right model to borrow here, not the ladder.

---

## 7. Following — `Services/Music/MusicFollower.cs`

A **third owner of the panel**, next to `PlaylistPlayer` and `AiPipeline`.
`App.StartMusic()` stops both, exactly as `StartAi()` and `StartPlaylist()` stop
each other, and `MusicFollower` **re-subscribes itself to `Display.Reconnected`**
— the sleep/wake section of `AGENTS.md` describes three restore paths for three
possible panel owners; this makes four, and a follower that goes silent after
the first sleep is the predictable bug if it is forgotten.

Driven off the 250 ms status poll that already exists:

| transition | action |
|---|---|
| title/artist changed | resolve (async) → card now, stream when resolved |
| paused | hold the last frame |
| resumed | correct position if drift is large, else nothing |
| session gone / stopped | hand the panel back — setting: previous playlist, hold, or blank |
| track ended | wait for the next `MediaPropertiesChanged`; do not guess |

**Drift control, stated honestly.** Compare `Display.PositionSeconds` against
the extrapolated session position each poll. Correct **only** when the gap
exceeds ~1.5 s, and at most once every ~20 s. A seek restarts ffmpeg and jumps
to a keyframe, so frequent micro-correction looks strictly worse than the drift
it fixes. Log every correction — that log is the only evidence available when
somebody says it "drifts".

And the part to put in the README rather than discover later: **this will not be
lip-sync tight.** The browser's own buffering is a standing offset of some
hundreds of milliseconds that we cannot observe, measure or subtract. For a
music video that is fine; nobody should promise more.

---

## 8. UI

- **Tray:** a "Follow YouTube Music" toggle beside the playlist and AI entries
  (`BuildTray` / `UpdateTray` in `App.xaml.cs`).
- **Settings:** enable; which media session to follow (from `jl_media_sessions`,
  with auto); video + card / card only; what happens when the music stops;
  and a switch for the yt-dlp search, so somebody who does not want the app
  searching YouTube on its own can run card-only.
- **Main window:** a small now-playing strip — what we detected, and what we
  resolved it to. **"It's showing the wrong video" is the most likely complaint
  this feature will ever get**, and this strip is the only place a user can see
  why.

---

## 9. Phases

1. **Sense.** Native SMTC reader + `MediaSessionProvider` + tokens. No panel
   behaviour at all — but you can immediately put the current track on an
   overlay over your existing playlist. Independently useful, and its worst
   failure mode is "no sensors".
2. **Card.** Art cache, built-in "Now playing" profile, `MusicFollower` owning
   the panel for the card only. Complete feature, no yt-dlp, no sync problem.
3. **Video.** Resolver, `jl_play_stream`, drift control. The only phase with a
   moving target underneath it.
4. **Quality.** Rate-controlled per-frame live encode; measure a streamed video
   against a packed copy of the same file, on the panel, side by side.

Each phase ships on its own. That ordering is the point: everything fragile is
in phase 3, and phases 1–2 are still worth having if phase 3 is a disappointment.

---

## 10. Risks

- **Search picks the wrong video.** Mitigated by the duration check and made
  visible by the now-playing strip; not eliminated.
- **Browsers differ.** Firefox reports less through SMTC than Chrome, and some
  sites report no timeline at all. Verify per browser before the README promises
  any of them.
- **YouTube moves.** Format selection and URL signing are yt-dlp's problem and it
  is already bundled and already updated by hand; a broken resolve must fall back
  to the card, never to an error.
- **Verify by looking at the panel.** `AGENTS.md` is emphatic and this feature is
  the perfect trap: the host reports a clean 30 fps and zero drops while a live
  stream hitches, exactly as it does for an overspending overlay. Watch a card at
  960×480 and a full three-minute video before calling any phase done.

---

## 12. What the build changed

Four things were wrong or missing in the plan above, all of them found by
running it rather than by reading it.

**Firefox reports no track length, often.** The plan assumed a duration from the
session and made it the resolver's primary filter. Measured: some tracks report
`EndTime` correctly, others report 0 for the whole track while the position
advances perfectly — same browser, same service, minutes apart. So the resolver
grew a real title-and-artist path rather than a token fallback, and every card
layer that needs a length hides itself when there is none. Position *is*
reliable, which is the half that sync depends on.

**Source app ids are opaque.** Firefox calls itself `308046B0AF4A39CB`. The
plan's "prefer a browser-shaped app id" heuristic is not possible; auto mode
prefers whichever session is playing and Settings lists friendly names.

**The overlay has to swap per mode, not per session.** The plan had the follower
switch to the card profile while running. But the card's scrim covers all
960x480, so a video would have played through a dark sheet. It swaps on every
transition instead: card profile for the card, the user's own for a video.

**`ImageLayer.File` had to become a template, and two things had to change with
it.** `OverlayService.Signature` now appends the *resolved* path — appending the
template would have meant album art never changing as far as the skip check was
concerned — and the follower clears `OverlayRenderer`'s image cache on every
track, because that cache is keyed on path and never expires.

**Following froze the app on every track change.** The plan said "nothing
blocks" and left it there. But every content call starts by stopping the current
worker and waiting for it, and the abort is only checked between frames — so a
stream producing nothing pinned the UI thread for as long as ffmpeg took to give
up. Measured at 7.3 s per track change; 0 ms once `StopWorkerLocked` learned to
terminate the child first. Two smaller faults fell out of the same root: the
follower could not tell a stream that had not started yet from one that never
would, and it "corrected" the growing phantom drift of a stalled stream every
twenty seconds forever.

And one thing the plan did not anticipate at all:

**ffmpeg has to be allowed onto the network, and nothing says so when it is
not.** On a machine with default-deny outbound (plus GlassWire here), ffmpeg
fails with `Error number -138` — `ETIMEDOUT` — on every URL, including plain
HTTP to an IPv4-only host, while yt-dlp works because it has its own rule. It
reads as an expiring URL or a bad format selector and is neither. The README now
says so, and `ffmpeg -i https://anything` is the one-line check.

The alternative considered was piping `yt-dlp` straight into ffmpeg's stdin,
which sidesteps the firewall, expiring URLs, user agents and DASH fragments in
one move — at the cost of a second process and of seeks that have to discard
rather than range-request. It remains the better fallback if the direct URL ever
becomes unreliable.

## 11. Still open

- **Paused:** hold the last frame, or draw a dimmed "paused" card?
- **Music stops or the browser closes:** return to the playlist that was running,
  hold the last frame, or blank?
- **Firefox:** required, or is Chrome/Edge enough for the first release?
