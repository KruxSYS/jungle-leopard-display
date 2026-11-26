# Preprocessing

*Part of the [Jungle Leopard Display](../README.md) documentation. Moved out of the README in 1.7.0, unchanged.*

The panel is not a display — it is an MJPEG sink. Everything it accepts is one
fixed shape: 960×480, baseline 4:2:0, under 80 KB, arriving at a steady rate. So
what ffmpeg produces for a given source is a pure function of that source's bytes
and the render options, which is the same assumption the calibration cache has
always made. Those frames can therefore be computed once and replayed.

| Mode | What happens | Cost |
|---|---|---|
| **Off** | ffmpeg streams MJPEG for as long as the item plays | a running ffmpeg, always |
| **Memory** | frames built into RAM once, then ffmpeg exits | ~1.2 MB/s of RAM, rebuilt each launch |
| **Disk** | frames built into a pack file once, reused forever | ~70 MB per minute of video |

**Memory** is the default. It writes nothing, and a source too long to hold
falls back to streaming on its own — so its worst case is exactly the old
behaviour.

Both limits are settable in **Settings → Preprocessing**, which shows whichever
one the selected mode actually uses (Off has none). Defaults are 512 MB in
memory and 8 GB on disk, and each preset is labelled with roughly how much video
it holds. The two limits behave differently on purpose: reaching the memory
limit makes that one item stream instead, while reaching the disk limit evicts
the least recently used packs. So raising the memory limit widens what benefits
— it never decides what will play.

A pack is deliberately dull: a header, the frame blobs, then an index. Identical
frames share one blob and the index points at it more than once, which collapses
GIFs and static footage to a fraction of their nominal size. Disk packs are
memory-**mapped** rather than read, so a long video costs address space rather
than working set, and the cache evicts least-recently-used once it passes its
limit.

## Quality per frame

Calibration has to pick one `-q:v` for a whole file, set by its **busiest**
frame, because a stream commits to a figure before it knows what is coming. A
pack does not: nothing about the panel requires two frames to have been encoded
the same way. So with **Give every frame the best quality it fits** on — the
default — the builder encodes each frame at the best quality that still fits
under 64 KB, and stores exactly that.

What one frame may spend is **the data rate divided by the frame rate**, not the
80 KB a single frame is allowed. That distinction is the whole of it: 80 KB is a
per-frame maximum, and the panel cannot decode 30 of those a second. Not a link
limit — writes measure 3 MB/s flat out — but the panel's own decoding, which
takes longer for a bigger JPEG and gives no sign at all when it falls behind.
Calibration never had to know, because `-q:v` stops at 3 and that lands near
37 KB a frame here — about 1.1 MB/s, whatever the target said. Budget by rate
instead and the same 1.1 MB/s buys a markedly better picture, because those
bytes go where they are needed.

Measured on a 30 s clip that calibrated to `-q:v 3`, against the source frames:

| | pack | mean / peak frame | SSIM |
|---|---|---|---|
| one `-q:v` for the file | 32.3 MB | 36.8 / 51.8 KB | 0.911 – 0.954 |
| per frame, 1.1 MB/s | 30.9 MB | 35.2 / 38.3 KB | 0.965 – 0.986 |
| per frame, 1.4 MB/s | 42.7 MB | 48.5 / 63.9 KB | 0.982 – 0.991 |

The default is the middle of the five offered: better on every frame measured,
in slightly **fewer** bytes than the uniform pack and with a third off the peak
frame. The 1.4 MB/s row is sharper again and is offered, but it is past what
this panel decodes in time — it stutters. Since nothing detects that but your
own eyes, raise it only while watching the panel.

There are two rates **below** the default as well, because 1.1 MB/s is a default
and not a floor. It was measured on one panel against one kind of content, and
what exhausts a panel's decoder is density — foliage, gravel, rain, anything
without a flat region to compress. If a particular video still hitches at the
default, the next thing to try is 0.9 MB/s, not a different setting.

Two more consequences: frames are encoded by **WIC** rather than ffmpeg, and a
per-frame build skips calibration entirely — one pass over the file instead of
two.

Two more things worth knowing:

- **The frame rate is part of what a frame is.** It is baked into the pack's key
  along with rotation, stretch and either the calibrated quality or the
  per-frame cap, so changing any of them rebuilds every pack. That is correct rather than wasteful — the old frames
  really are the wrong frames — but it is why the frame-rate slider feels
  expensive in Disk mode.
- **Stills benefit too, in every mode but Off.** A still in a playlist used to be
  re-transcoded on every rotation; now it is prepared once and remembered.

---

[Back to the README](../README.md)
