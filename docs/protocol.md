# Protocol notes

*Part of the [Jungle Leopard Display](../README.md) documentation. Moved out of the README in 1.7.0, unchanged.*

**Serial.** CDC-ACM at 115200/8-N-1 (the rate is advisory), DTR and RTS asserted,
no flow control. Found by walking the ports class for hardware ID
`VID_33C3&PID_7788` and reading `PortName` from its device registry key.

**Control frames.**

```
55 AA | length (u16 LE) | cmd | payload… | checksum (u16 LE)
```

`length` counts the whole frame (`payload + 7`). The checksum is a plain 16-bit
sum of every preceding byte.

| Cmd | Meaning | Reaches this panel |
|---|---|---|
| `0x01` | Restart | yes |
| `0x03` | Set backlight (one byte, 0–100) | yes |
| `0x06` | Get device info (replies with JSON) | yes |
| `0x0C` | Begin OTA firmware flash | yes |
| `0x11` | Enter / hold live mode | yes |
| `0x14` | Set motion-before-off | yes |
| `0x15` | Set motion timeout | firmware >= 2.8 |
| `0x20` | Set region (UTF-8 string) | yes |
| `0x21` | Close | firmware >= 3.1 |
| `0x23` | Set serial number, then reboot | yes |
| `0x25` | Set motor — open (`1`) / close (`2`) | only when region is `ycc28_v1` |
| `0x26` | Set real-time timeout | firmware >= 4.1, so never here |

The right-hand column is the condition the vendor app checks before it will send
that command, taken from `main/_baseClass/device.js` and evaluated against this
panel (firmware 3.1). Only `0x03`, `0x06` and `0x11` are sent from this project.

**`0x25` is not a pump-speed control.** The payload is one byte: `1` opens, `2`
closes. A 60-second CPU-temperature poll drives it — above the configured
threshold (default 50 °C) it opens, below it closes — and the device stays busy
for `actionTimeout` seconds afterwards, 35 by default, rejecting further
commands. It only runs on hardware whose region has been set to `ycc28_v1` via
`0x20`, which is not this one.

**`0x0C` and `0x23` are documented but not implemented, on purpose.** `0x23`
takes a serial number, writes it to the panel and reboots into it. `0x0C` takes
`F2 FF` followed by the firmware size as a 32-bit little-endian value, after
which the `.bin` streams through the image envelope below. Neither is gated away
from this hardware, and both are one bad byte from a panel that no longer
enumerates.

**Image frames use a different envelope** — no `55 AA`, no command byte:

```
length (u32 LE) | JPEG bytes… | checksum (u16 LE)
```

The checksum covers the length bytes too.

**Live mode lapses** unless `0x11` is re-sent about every **1500 ms**. This is
why showing a still keeps a thread alive rather than firing and forgetting.

**A still must be re-sent, not sent once.** The panel commits a frame only when
the bytes behind it arrive, and it needs a moment after `0x11` before it takes
pixels at all. Video hides both — ffmpeg's startup covers the mode change and
the next frame is 33 ms away — but a picture written once and then left alone
stays half-drawn on the glass. `HoldStill` re-sends the same frame every
**250 ms**, a fifth of the bandwidth playback already sustains.

**Stills must be 4:2:0, like every video frame.** ffmpeg matches the JPEG
encoder to its *input*, so video (`yuv420p`) lands on `yuvj420p` while an RGB
still (PNG, BMP, a screenshot) lands on a 4:4:4 layout this panel cannot
decode. The image path pins `-pix_fmt yuvj420p`, and the "already conforms,
send as-is" shortcut checks the SOF marker for baseline 4:2:0 rather than
trusting the dimensions alone.

**Before talking to the device**, the vendor app sends `FF D9 FF D9` (a JPEG
end-of-image marker, flushing any partial frame), pauses, then four zero bytes.
This client does the same.

**Size limits.** Frames must be ≤ **80 KB**. Video calibration targets **64 KB**
to leave headroom, by encoding every *keyframe* in the file (`-skip_frame nokey`,
so inter-frames are never reconstructed) and walking `-q:v` up through
`3,4,5,7,9,12,16,20,25,31` until the worst keyframe fits. Keyframes are
intra-coded and therefore the most detailed frames in the stream, which makes
them a conservative proxy. Surveying the *whole* file matters — calibrating on
the first few seconds badly underestimates a video that gets busier later.

A preprocessed item can do better: see [quality per frame](preprocessing.md#quality-per-frame),
which budgets each frame out of a data rate instead and does not calibrate at
all. Note the difference between the two limits — 80 KB is what one frame may
be, and it is *not* 30 times that per second.

**SPI-class panels.** If the image path doesn't work on your unit, check
`checkIsSPI()` in the vendor app's `main/util/common.js` against your model
string. Those take raw RGB565 big-endian with **no** length header and no
checksum — `((r>>3)<<11) | ((g>>2)<<5) | (b>>3)` as u16 BE per pixel. Control
commands are identical either way; only the pixel path differs.

---

[Back to the README](../README.md)
