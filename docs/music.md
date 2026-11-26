# Following what's playing

*Part of the [Jungle Leopard Display](../README.md) documentation. Moved out of the README in 1.7.0, unchanged.*

Press **Follow music** in the window, or **Follow what's playing** on the tray,
and the panel starts mirroring
whatever else on the machine is playing music — a YouTube Music tab, a browser,
a desktop player. It reads the Windows media session, the same thing that feeds
the volume-key popup.

It plays nothing and controls nothing. Your player stays the remote control;
this only watches and draws.

Two things can end up on the glass:

- **The music video**, downloaded once and played from disk, when one can be
  found.
- **A now-playing card** when one cannot — the album art blurred across the
  panel with the title, artist and progress over it. Most tracks on a music
  service have no video, so this is the common case rather than a consolation
  prize.

The card is an ordinary overlay profile called **Now playing**, so it appears in
the tray menu and can be rearranged, restyled or re-themed in the overlay editor
like any other. The backdrop it draws on is a 960×480 JPEG the app builds
itself, small enough to skip ffmpeg entirely — so the card works even on a
machine that has none. While a video is being fetched, a second bar under the
timeline shows how far along the download is.

Following is remembered between runs: a display left mirroring music is still
mirroring music after a reboot.

## The track becomes sensors

Whether or not the panel is following, the current track is published as
overlay tokens, so any profile can use it:

`{music.title}` `{music.artist}` `{music.album}` `{music.source}`
`{music.state}` `{music.elapsed}` `{music.length}` `{music.remaining}`
and, as numbers for bars and visibility rules, `music.position`,
`music.duration`, `music.progress`, `music.playing`, `music.idle`,
`music.fetching` and `music.fetch`.

`{music.art}` is the current cover's path on disk, which an **image** layer can
be pointed at — that is how the card shows album art, and it works in any
profile you build yourself.

Every one of these belongs to the track and none of them is inherited by the
next one. A player that reports no length — YouTube Music in a browser often
does not — leaves `{music.length}`, `{music.remaining}` and `music.progress`
with nothing to say, so a layer bound to one hides itself and a token reads
`--`. It does **not** show the length of the song that finished, which is what
it used to do: a 1:33 track length sat on the card against a 3:29 song.

## Finding the video

The media session reports a title, an artist and usually a length. It never
reports a URL, so the video has to be searched for with `yt-dlp`, and the length
is what keeps that honest: a search for a popular song returns the track, a
lyric video, three live cuts and an hour-long loop, and only one of them runs for
the same number of seconds. Where the player reports no length — YouTube Music
in a browser often does not — it falls back to matching the title and preferring
the artist's own upload.

What it picked is shown beside the preview in the main window. That line is the
answer to "why is it showing *that*".

Which video matched which track is cached in `music-cache.json`, so a song
played twice searches once.

## The video is downloaded, not streamed

`yt-dlp` fetches the 720p rung to `music-videos\` and the panel plays the file.
That is not an optimisation, it is what makes the feature work at all.

YouTube serves fragmented MP4 at roughly playback rate, and ffmpeg can only seek
one by reading the whole file to index it. Measured on a 3½-minute track, every
seek into a stream cost **106 seconds** before the first frame appeared — the
same whether the target was 30 seconds in or 120 — against 0.4 seconds with no
seek at all. A downloaded file has an index, so the same seek takes 0.3 seconds.
Joining a song already in progress, and following you when you drag the
scrubber, are both only possible because of that.

It also settles the quality question: the whole 720p rung comes down at whatever
rate the link allows, rather than being decoded live from a throttled
connection.

And because it is a file, it is **preprocessed** like anything in the library.
Its frames are built once into the pack cache — at whatever **Settings →
Preprocessing** is set to — and the panel then plays those frames with no ffmpeg
involved at all.

That is what stops a music video skipping. A live stream has to commit to one
JPEG quality up front, and it is a guess: a busy video makes frames too big for
the panel, and those are dropped rather than sent. Preprocessed frames are
budgeted against the panel's data rate one at a time, so none of them can be.
Following the song gets sharper too, because a seek into preprocessed frames is
an index rather than ffmpeg restarted on the nearest keyframe.

**The video does not wait for that.** It starts as soon as it is downloaded, and
its frames are built behind it — at low priority, one song at a time — with the
progress shown on the card under the track, on the same bar the download uses.
When the build finishes the panel swaps onto the frames for the rest of the song,
and every later play of it starts from them. A song skipped past is left unbuilt
until it comes round again.

Building a new song takes up to a minute. It is not free on disk either: reckon
about 200 MB for a three-minute track, in the same cache the library's videos
use, oldest-unplayed evicted first. With preprocessing switched off, a followed
video is instead surveyed for a quality whose worst frame still fits, and
streamed as before.

**Settings → Music** caps how long a video may be before it is worth fetching
at all — a search for a song reliably turns up an hour of it on loop, and that
is checked against the search result, before the download rather than after it.
The default is 12 minutes.

Downloads are then swept by age and by size, and **Sweep now** applies both
immediately. The defaults keep a video for two weeks after it was last played,
within 4 GB. Unfinished downloads left by skipping tracks are cleaned up too.

## On sync

The panel and your speakers are two independent streams of the same song, and
the browser's own buffering is an offset this app cannot see or measure. The
video starts where the song has reached and both then run at their own steady
rate.

Dragging the scrubber in the browser is followed within about a second. Small
drift is deliberately left alone: correcting it restarts the decoder on a
keyframe, which is more noticeable than the drift itself.

Expect it to be close, not frame-accurate. For a music video that is fine.

---

[Back to the README](../README.md)
