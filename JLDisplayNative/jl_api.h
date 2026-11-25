// jl_api.h — flat C surface over JLDisplayCore, shaped for P/Invoke.
//
// Two rules govern everything here:
//
//   1. Nothing blocks. Preparing an image shells ffmpeg, and calibrating a long
//      video takes tens of seconds; both would freeze a UI thread. Every content
//      call starts a worker and returns immediately. Progress is polled through
//      jl_get_status.
//
//   2. There is exactly one session per process, because there is exactly one
//      COM port and only one process on the machine can hold it. No handles to
//      pass around, no lifetime for the caller to get wrong.
//
// All strings are UTF-16. All calls are safe from any thread.

#pragma once

#include <stdint.h>

#ifdef JLAPI_EXPORTS
#define JLAPI __declspec(dllexport)
#else
#define JLAPI __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

    // -----------------------------------------------------------------------

    enum JlState {
        JL_STATE_DISCONNECTED = 0,   // no session open
        JL_STATE_IDLE = 1,   // connected, showing nothing
        JL_STATE_PREPARING = 2,   // transcoding a still
        JL_STATE_CALIBRATING = 3,   // surveying a video for a safe quality
        JL_STATE_PLAYING = 4,   // pushing frames, or holding a still
        JL_STATE_ERROR = 5,   // see JlStatus::error
        JL_STATE_PREPROCESSING = 6    // building panel-ready frames up front
    };


    // How much work is done before playback starts rather than during it.
    // Session-wide rather than per item, and deliberately NOT part of
    // JlRenderOpts: it changes nothing about the pixels, so it must not feed
    // the calibration or pack keys.
    enum JlPreprocess {
        JL_PRE_OFF = 0,      // transcode continuously while playing
        JL_PRE_MEMORY = 1,   // transcode once into RAM, then let ffmpeg exit
        JL_PRE_DISK = 2      // transcode once into a file, reused across runs
    };
    // Return codes. Negative is failure; jl_get_status carries the detail.
    enum JlResult {
        JL_OK = 0,
        JL_ERR_NOT_OPEN = -1,
        JL_ERR_BUSY = -2,
        JL_ERR_BAD_ARGS = -3,
        JL_ERR_FAILED = -4
    };

    // Mirrors jl::RenderOpts. Fixed-size so the marshaller can blit it.
    typedef struct JlRenderOpts {
        int32_t stretch;       // fill the panel, ignore aspect ratio
        int32_t rotate;        // 0, 90, 180 or 270
        int32_t fps;           // 1..60; the panel refreshes at 30
        int32_t quality;       // 0 = calibrate automatically, else 2..31
        int32_t loop;
        int32_t recalibrate;   // ignore any cached calibration

        // Preprocessing only: give every frame the best quality it can afford
        // instead of encoding the whole file at the one figure its busiest
        // frame allows. Bigger packs, better picture. Ignored when `quality`
        // names a -q:v.
        int32_t perFrameQuality;
        int32_t qualityCeiling;   // 0 = default (84), else 50..98 percent

        // What the link and the panel behind it can swallow, in bytes per
        // second. Divided by fps this is what one frame may spend, and it is
        // the setting that decides whether playback stutters. 0 = default.
        uint32_t byteRate;

        wchar_t hwaccel[32];   // "" software, "auto" resolve, or an explicit method
    } JlRenderOpts;

    typedef struct JlStatus {
        int32_t state;            // one of JlState
        int32_t connected;
        wchar_t port[32];

        int64_t framesSent;
        int64_t framesDropped;
        double  fps;

        // Where the current video has reached and how long it runs, both in
        // seconds. Duration is 0 when it could not be determined: ffprobe is
        // optional, and a still has no length. Both are 0 when nothing is
        // playing.
        double  positionSeconds;
        double  durationSeconds;

        // Increments once each time an item runs to its natural end. The host
        // watches for a change rather than a flag, so a fast item cannot be
        // missed between two polls.
        int32_t finishedCount;

        // Increments each time a new preview frame is available, so the host
        // only pays for jl_get_last_frame when something actually changed.
        int32_t frameCount;

        wchar_t message[256];     // latest progress or status line
        wchar_t error[256];       // last error; cleared when a new item starts

        // Overlay diagnostics. All zero while nothing is being drawn on top.
        // They ride along on the status poll the host already does rather than
        // costing a call of their own.
        double  overlayComposeMs;   // rolling mean: decode + blend
        double  overlayEncodeMs;    // rolling mean

        // Rolling mean of the composed frame, in bytes. Multiply by the frame
        // rate for the data rate actually reaching the panel — the one number
        // that makes an overspending compositor visible, since the panel itself
        // reports a clean 30 fps and no drops while falling behind.
        double  overlayOutBytes;

        int32_t overlayQuality;     // working JPEG quality x100, 5..92
        int32_t overlayDrops;       // frames that would not fit under the cap
        int32_t overlayOverBudget;  // frames sent above the rate budget
        int32_t overlayReserved;    // pads to an 8-byte multiple; keeps the
                                    // struct's size stable for the next int32
    } JlStatus;

    // -----------------------------------------------------------------------
    // Session
    // -----------------------------------------------------------------------

    // Writes the panel's COM port name, e.g. "COM7". Returns JL_OK if attached.
    JLAPI int32_t jl_find_port(wchar_t* buf, int32_t cch);

    // `port` NULL or empty autodetects. Fails with the reason in JlStatus::error
    // — most usefully when another program already holds the device.
    JLAPI int32_t jl_open(const wchar_t* port);

    // Stops any running item, then releases the port. Safe to call when closed.
    JLAPI void jl_close(void);

    // The device's JSON info blob. Only valid while idle: it reads a reply off
    // the port, which would collide with a running item.
    JLAPI int32_t jl_get_info(wchar_t* buf, int32_t cch);

    JLAPI int32_t jl_set_brightness(int32_t percent);

    // -----------------------------------------------------------------------
    // Content — all asynchronous, all replace whatever is currently showing
    // -----------------------------------------------------------------------

    // Transcodes and sends one still, then holds live mode until something else
    // is asked for. A still has no natural end, so finishedCount never advances
    // for one — dwell timing belongs to the host's playlist.
    JLAPI int32_t jl_show_image(const wchar_t* path, const JlRenderOpts* opts);

    // Calibrates if needed, then streams. finishedCount advances when the source
    // ends, unless opts->loop is set.
    JLAPI int32_t jl_play_video(const wchar_t* path, const JlRenderOpts* opts);

    // Plays a live network source — an http(s) URL ffmpeg can open.
    //
    // Deliberately not jl_play_video with a URL in it. A stream is never packed
    // and never calibrated: there is no file identity to key a pack on, and a
    // survey pass would fetch the whole thing a second time to decide a quality
    // it would be finished needing by the time it arrived. It uses the figure
    // calibration lands on for nearly all content anyway, so it sends no more
    // than a file does.
    //
    // `inputArgs` is passed to ffmpeg verbatim before -i — "-user_agent ..." and
    // "-headers ...", which a signed media URL usually needs. It is a separate
    // argument rather than a field on JlRenderOpts because RenderOpts is key
    // material for the caches, and a URL that changes every play would poison
    // them.
    //
    // `opts->quality` is honoured here as an explicit -q:v: a stream cannot be
    // surveyed, but a caller holding a calibrated figure for the same source
    // (see jl_calibrate) can hand it over rather than leave this to the
    // built-in guess. 0 takes the guess.
    //
    // `startSeconds` joins a source already in progress; `durationSeconds` is
    // how long it runs, which the caller knows and ffprobe would cost a request
    // to ask. Both may be 0.
    //
    // opts->loop is ignored: a signed URL expires, so looping one is not a thing
    // that works. finishedCount advances when the source ends.
    JLAPI int32_t jl_play_stream(const wchar_t* url, const wchar_t* inputArgs,
        double startSeconds, double durationSeconds,
        const JlRenderOpts* opts);

    // Builds this file's frames into the on-disk pack cache without touching
    // the device, so playing it later starts immediately and runs no ffmpeg.
    // Needs no open session, blocks for as long as the build takes, and is safe
    // on a background thread while something else is playing.
    //
    // The render options must be the ones playback will use — they are part of
    // what the frames are, and a pack built under different ones is a different
    // pack. JL_OK when the cache holds a pack for them afterwards.
    JLAPI int32_t jl_preprocess(const wchar_t* path, const JlRenderOpts* opts);

    // Whether playing this file with these options would find a pack waiting,
    // without building one. JL_OK when it would.
    JLAPI int32_t jl_pack_ready(const wchar_t* path, const JlRenderOpts* opts);

    // How many frames the jl_preprocess call in flight has built, or -1 when
    // there is none. Poll it to show progress: a caller that knows how long the
    // source runs turns it into a percentage — frames / (seconds * fps).
    JLAPI int32_t jl_preprocess_frames(void);

    // The last warning or error from the most recent jl_preprocess, which is
    // the only account of why one failed: its thread's log sink is separate
    // from playback's so a build cannot overwrite what the panel is saying.
    JLAPI int32_t jl_preprocess_message(wchar_t* buf, int32_t cch);

    // Surveys a video and caches the result without touching the device, so
    // pressing play later is instant. Needs no open session.
    JLAPI int32_t jl_calibrate(const wchar_t* path, const JlRenderOpts* opts);

    // Stops the current item and blanks the panel. Returns once it has stopped.
    JLAPI void jl_stop(void);

    // Moves the currently playing video to `seconds`. Takes effect at the next
    // frame boundary, which is the only safe moment to move: a preprocessed
    // item jumps straight to the frame, while a streamed one restarts ffmpeg at
    // the mark and therefore lands on the nearest keyframe before it.
    //
    // Ignored when nothing is playing, or when what is playing is a still.
    // Values outside the item are clamped into it.
    JLAPI void jl_seek(double seconds);

    // -----------------------------------------------------------------------
    // Observation
    // -----------------------------------------------------------------------

    JLAPI void jl_get_status(JlStatus* out);

    // Copies the last JPEG sent to the panel, for a preview of what is actually
    // on screen. Returns the byte count, or the size needed when `cap` is too
    // small, or 0 when there is no frame. Pass buf=NULL to ask for the size.
    JLAPI int32_t jl_get_last_frame(uint8_t* buf, int32_t cap);

    // -----------------------------------------------------------------------
    // Preprocessing
    // -----------------------------------------------------------------------

    // Takes effect on the next item started; anything already playing carries
    // on as it began. Off is the original behaviour and the fallback whenever a
    // pack cannot be built — a source over the memory budget, a cache that
    // cannot be written — so this never turns a playable item into a failure.
    JLAPI void jl_set_preprocess(int32_t mode);

    JLAPI int32_t jl_get_preprocess(void);

    // How much each mode may use, in bytes. Pass 0 for either to restore its
    // default (512 MB in memory, 8 GB on disk); anything below the floor the
    // core considers usable is raised to it, so read the values back rather
    // than assuming what was set is what took effect.
    //
    // The memory figure applies to the next pack built. The disk figure is
    // enforced the next time one is written, so lowering it does not delete
    // anything until then — use jl_pack_cache_clear to reclaim the space now.
    JLAPI void    jl_set_pack_budgets(int64_t memoryBytes, int64_t diskBytes);
    JLAPI int64_t jl_memory_budget(void);
    JLAPI int64_t jl_disk_budget(void);

    // Bytes currently held by the on-disk pack cache, and a way to empty it.
    // Clearing is safe while something is playing: a pack already mapped keeps
    // running to the end of its item.
    JLAPI int64_t jl_pack_cache_bytes(void);
    JLAPI void    jl_pack_cache_clear(void);

    // -----------------------------------------------------------------------
    // Overlay
    //
    // Statistics, clocks and gauges drawn on top of whatever is playing. The
    // host renders them — it owns the fonts, the layout and the editor — and
    // hands down finished pixels; this side only blends and re-encodes.
    //
    // The surface is 960x480 BGRA with PREMULTIPLIED alpha, which is what
    // WPF's Pbgra32 already produces, so no conversion is needed on either side.
    // -----------------------------------------------------------------------

    // Turns compositing on or off. Off is free: no decode, no encode, and
    // frames reach the panel exactly as they did before this existed. Takes
    // effect on the next frame, including mid-playback.
    JLAPI void jl_overlay_set_enabled(int32_t on);

    // Replaces the overlay surface. `w` and `h` must be 960 and 480; anything
    // else is refused. Copies the pixels, so the caller may reuse its buffer
    // immediately. Safe to call while something is playing — that is the normal
    // case, several times a second.
    JLAPI int32_t jl_overlay_update(const uint8_t* bgraPremultiplied,
        int32_t w, int32_t h);

    // Drops the surface, leaving nothing drawn. Cheaper than pushing a fully
    // transparent one, and unlike jl_overlay_set_enabled it forgets the pixels.
    JLAPI void jl_overlay_clear(void);

    // -----------------------------------------------------------------------
    // Media session
    //
    // What is playing elsewhere on the machine — a browser tab, a music player
    // — read from the Windows media session (SMTC). The panel has no audio and
    // this plays nothing; it exists so the host can follow along with somebody
    // else's playback.
    //
    // A dedicated thread does the reading, because the calls behind this are
    // cross-process into the player and must not run on a UI thread. These
    // functions only copy out what it last saw.
    // -----------------------------------------------------------------------

    typedef struct JlMediaStatus {
        int32_t running;           // the reader is up
        int32_t available;         // the session manager was obtained at all
        int32_t present;           // a session is being followed
        int32_t playing;           // 0 paused or stopped, 1 playing

        // Seconds. `position` is extrapolated from the last timeline report,
        // because players refresh that every few seconds rather than every
        // frame — read it as "where the track is now", not "what was reported".
        // `duration` is 0 when the player does not say.
        double  position;
        double  duration;

        // Bumps when the title, artist or album changes, and when the art does.
        // Watching a counter rather than diffing strings means a track that
        // repeats is still seen as a new track.
        int32_t trackVersion;
        int32_t thumbnailVersion;

        wchar_t title[256];
        wchar_t artist[256];
        wchar_t album[256];
        wchar_t sourceApp[128];    // "chrome.exe", "msedge.exe", …
    } JlMediaStatus;

    // Starts the reader, or repoints an already-running one. `preferredApp` is
    // a source app id to follow, or NULL/empty to choose automatically — which
    // prefers a session that is actually playing, and otherwise stays with
    // whatever it was already following rather than flapping between players.
    JLAPI int32_t jl_media_start(const wchar_t* preferredApp);

    // Stops the reader and forgets everything it saw. Waits for the thread,
    // which may be inside a call into the player.
    JLAPI void jl_media_stop(void);

    JLAPI void jl_media_get(JlMediaStatus* out);

    // Every session currently registered, as app ids: NUL between entries and
    // an extra NUL at the end, like any Windows multi-string. Returns the
    // characters needed, so pass buf=NULL to ask for the size.
    JLAPI int32_t jl_media_sessions(wchar_t* buf, int32_t cch);

    // The current track's album art, exactly as the player supplied it — PNG or
    // JPEG, whatever it chose. Returns the byte count, or the size needed when
    // `cap` is too small, or 0 when there is none. Same shape as
    // jl_get_last_frame.
    JLAPI int32_t jl_media_thumbnail(uint8_t* buf, int32_t cap);

    // -----------------------------------------------------------------------

    // Writes the resolved ffmpeg.exe path. JL_OK if present. Worth calling once
    // at startup: without ffmpeg every non-conforming item fails identically.
    JLAPI int32_t jl_ffmpeg_path(wchar_t* buf, int32_t cch);

#ifdef __cplusplus
}
#endif
