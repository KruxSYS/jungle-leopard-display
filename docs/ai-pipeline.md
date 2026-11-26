# AI image pipeline

*Part of the [Jungle Leopard Display](../README.md) documentation. Moved out of the README in 1.7.0, unchanged.*

The manager can generate its own wallpaper: you keep a list of short ideas, an
LLM expands each into a full prompt, [SwarmUI][swarmui] renders it, and the
result goes on the panel. **AI** on the status bar opens it.

Everything is optional and off until configured — the app is exactly as it was
if you never open that window.

[swarmui]: https://github.com/mcmonkeyprojects/SwarmUI

```
prompts ──▶ LLM enhance ──▶ SwarmUI ──▶ generated\  ──▶ library
                                            │
                        buffer of ready images
                                            │
                            dwell timer ────┴──▶ panel
```

## What you need

**SwarmUI**, running and reachable — the default install listens on
`http://localhost:7801`. Press **Test** and the window fills its model, sampler
and scheduler lists from that server, so you pick from what is actually
installed rather than from a guess.

**An LLM**, optionally. Two providers:

| Provider | For |
|---|---|
| **OpenAI-compatible** | Ollama, LM Studio, llama.cpp, OpenRouter, OpenAI — anything at `/v1/chat/completions`. Works offline. |
| **Anthropic** | Claude, via the Messages API. Needs a key. |

Set the provider to **None** to send your prompts to the image model as written.
Each provider remembers its own address, model and key, so switching between a
local model and a hosted one to compare results doesn't make you retype
anything.

## Two clocks

Generation on a home GPU takes anywhere from ten seconds to several minutes, so
generating and displaying run independently:

- **Keep ready ahead** — how many finished images to hold in reserve. A worker
  tops the buffer up in the background.
- **Show each for** — how long an image holds the panel.
- **Generate no more than** — a floor on time between generations, so a fast
  backend doesn't fill the disk in a minute. `0` generates as fast as the buffer
  empties.

The panel is never waiting on the backend: it rotates through what is already
finished. If the buffer does run dry, the current image simply stays up.

## Failure is not fatal

The pipeline is meant to be left running for days, so nothing in it stops the
slideshow permanently:

- A failed enhancement **falls back to the prompt as written** and logs it — a
  dead LLM costs picture quality, not the whole rotation. Tick *Skip generating
  if enhancement fails* to prefer stopping instead.
- A failed generation backs off, doubling from 15 s to a 5-minute cap, so an
  unreachable server is a quiet line in the log rather than a request storm.
- An expired SwarmUI session — which every restart of SwarmUI causes — is
  renewed once and the request retried.
- A disconnected panel picks its image back up when the device returns.

Each leg has its own **Test** button, so a failure isolates to one hop rather
than needing to be guessed at.

## Where the images go

Generated images land in `%LOCALAPPDATA%\JungleLeopardDisplay\generated\` and
appear under **GENERATED**, the second tab above the media grid, with the prompt
as the tooltip. They arrive on their own schedule and in bulk, so they get their
own tab rather than burying the hand-picked library — but they are ordinary
items otherwise, and can be shown, added to a playlist and pinned like anything
else.

**Keep at most** bounds how many are kept; past that the oldest go, file and
all. **Pin** exempts one — pinned images are never pruned, and neither is
whatever is currently on the panel or waiting in the buffer. **Remove** on the
GENERATED tab deletes the file as well, since nothing else would ever list it
again.

## A note on keys

API keys are encrypted with **DPAPI**, scoped to your Windows account, and
stored as base64 in `ai.json`. A copied `ai.json` is useless on another machine
or under another account. That is the whole guarantee: it stops a key
travelling, not someone already running as you. A key is never displayed once
saved — leave the box blank to keep it, type in it to replace it.

---

[Back to the README](../README.md)
