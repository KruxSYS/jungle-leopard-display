# Troubleshooting

*Part of the [Jungle Leopard Display](../README.md) documentation. Moved out of the README in 1.7.0, unchanged.*

**"COM7 is in use by another program"** — something else holds the port: the
vendor app, a running CLI, or the manager itself. In the manager, use
**Settings → Release device** to hand it over, then **Reconnect** to take it
back.

**The panel is stuck on an old frame after the machine slept** — this was a real
bug and is fixed; if you see it again, the log is the place to look. The manager
now releases the port when Windows says it is suspending and reopens it on wake,
because neither the handle nor the port going stale is something the host can
detect: the panel will happily accept writes it is no longer drawing. You should
see `suspending: released the panel` and then `resumed: reconnecting to the
panel` in `manager.log`. This needs **Auto-reconnect** on; with it off, a wake
leaves the panel released deliberately, and the log says so.

**"ffmpeg.exe not found"** — install it, or drop `ffmpeg.exe` next to the
binaries. The manager checks once at startup and says so via a tray balloon,
rather than failing identically on every item.

**Nothing appears, no error** — the panel drops out of live mode after ~1.5 s
without a keepalive. If you're driving it yourself, keep sending `0x11`.

**Video is choppy** — check the dropped-frame count in the status line. Frames
over 80 KB are skipped; force a lower quality with `--quality 12` or
`--recalibrate`.

**"cannot reach SwarmUI"** — SwarmUI isn't running, or is on another port.
Check the address in the AI window and press **Test**; a working server reports
its version and model count.

**AI images generate but never appear** — the playlist and the AI slideshow both
drive the one panel, and starting either stops the other. **Start** in the AI
window is what hands the panel over.

**Enhanced prompts look like chat** — the enhancer strips code fences, quotes and
lead-in lines, but a model that ignores the system prompt entirely will still
produce prose. Lower the temperature, or reword the system prompt under
*Instructions and limits*.

**Build says `pwsh.exe is not recognized`** — that's vcpkg's `applocal.ps1`
integration trying PowerShell 7 and falling back to `powershell.exe`. Harmless;
install pwsh to quiet it.

## Known rough edges

- The solution maps **`Debug|x64` to `Release|x64`** for every project. That's
  inherited from the original `.sln`; the new projects mirror it so CRT settings
  match and linking works. It does mean "Debug" builds optimised code.
- Per-item rotation and stretch overrides exist in the model and are honoured at
  playback, but have no UI yet — only dwell time is editable per item.
- x64 only. The `Win32` solution configurations build the native projects but
  not the manager.
- The Anthropic client is raw HTTP rather than the official SDK, to keep the
  portable drop free of third-party assemblies. It covers one POST to
  `/v1/messages`; anything more — tools, streaming — would be worth the
  dependency instead.
- The AI pipeline generates one image at a time. SwarmUI will happily batch, and
  a backend with the VRAM for it is left idle between requests.

---

[Back to the README](../README.md)
