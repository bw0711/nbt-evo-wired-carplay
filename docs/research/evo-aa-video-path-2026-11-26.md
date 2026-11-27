# Android Auto video path on NBT Evo in NBT-AAD (Sq00ky/Supra-iDrive6-Android-Auto)

Source: https://github.com/Sq00ky/Supra-iDrive6-Android-Auto, commit `0a2bcafa` (2026-11-26), folder `milestone-7`,
daemon v1.9.120. Read only through `gh api`; nothing on GitHub was changed.
Read: `src/video_manager.{hpp,cpp}`, `src/mmr_launch.{hpp,cpp}`, `src/mmr_pps.hpp`, `src/lm_geometry.hpp`,
`src/status_hint.hpp`, `src/car_config.hpp`, `src/nbt_env.hpp`, `src/frame_queue.hpp`, `src/keyframe_ask.hpp`,
`src/favourite_composition.hpp`, `src/favourite_strip.hpp`, `include/screen_video.h`, `include/a2i_client.h`,
`src/audio/{a2i_stream,browser_audio,config}.hpp`, fragments of `src/main.cpp` and `src/aa_session.cpp`,
`tools/osd1281probe.cpp`, `tools/lmrestore.cpp`, `tests/test_mmr_pps.cpp`, `plugin/plugin_abi.hpp`,
`plugin/nbt_aad_plugin.cpp` (header), `readme.md`, `LICENSE.md`, `README.md`.

Marks: **[car]** = the readme says it was checked in a car; **[not car]** = the readme says explicitly "not car-verified";
no mark = the repo does not state the status. Almost all car checks were done on a Supra (`NBTevo_T21424I`). An important
exception: PR #66 (video profiles, `NBT_VID_FPS`) was checked on an **F22 M235i `NBTevo_N18032I`**, that is, on the same
firmware as ours.

## The main points for nbt_carplay

1. **Decoder.** It uses the factory mm-renderer, the same one the factory CarPlay uses (`/opt/conn/bin/carplay/mm-renderer`).
   Its client library is loaded with `dlopen`. H.264 is fed in **QNX messages** (`name_open("/dev/name/local/mdev")`
   + `MsgSendvs_r`). There is no devctl, no fifo/pipe, no feeding through PPS, no OpenMAX and no ffmpeg. The decoding itself
   runs on the IVA-HD inside mm-renderer: in the code this is visible only from a comment (`'H264' selects init_parser_h264 in ivahd_video_decoder`).
2. **There is only one hardware decoder.** The factory CarPlay context `cpvideoctx<pid>` holds `/dev/name/local/mdev`. While it
   is connected, mdev accepts our frames (ret=0) but draws them into the window group of the factory CarPlay, and nothing appears on the screen.
   The context must be either detached (preferred) or destroyed. After it is destroyed, the factory CarPlay cannot
   create it again until a reboot (errors `contextCreate err 17` / `contextOpen err 3`).
3. **The factory CarPlay feeds `fourcc=AVC1`**, with the size from carplay.ini, `prefetch_mode 0`, `winid=1280`. AA uses
   `fourcc=H264` in Annex B. Our stream (Annex B after conversion) matches the H264 path of AA, so it can be repeated
   as is. The mdev message format for AVC1 is **not described** in the repository: this is a hypothesis for later (avcC + NAL with a 4-byte
   length, no conversion), to be checked by reverse engineering `libconn_carplay_video_mmr.so`.
4. **The picture gets on top through LayerManager, not through the Screen z-order.** Our own QNX Screen window with the numeric ID_STRING
   `"1282"` → LayerManagerService creates surface 1282 itself → show: `layer 0x80000000 render order 0x502` + `surface 256
   visibility 0`; hide: `render order 0x100` + `surface 256 visibility 1`. **[car]** (Supra, 2026-11-05, v1.9.20).
5. **Do not do this:** `ilm_displaySetRenderOrder`. It once emptied the layer list of screen 0, and `tools/lmrestore.cpp`
   was written to repair that.

## 1. Which decoder and how H.264 is fed into it

- Client: `dlopen` of the first one found among `/opt/conn/lib/carplay/libmmrndclient.so.1`, `.../libmmrndclient.so`,
  `/fs/sda0/opt/conn/lib/carplay/...`, `/fs/sda0/repository/istep/opt/conn/lib/carplay/libmmrndclient.so`. Before it comes
  `dlopen(RTLD_NOW|RTLD_GLOBAL)` of `libstrm.so` (`/opt/conn/lib/carplay/libstrm.so.1`, fallback `/opt/mm/lib/libstrm.so.1`):
  it is needed for `strm_dict_new/strm_dict_set`. All `mmr_*` functions are fetched through `dlsym` (`VideoManager::init`, steps 14-15).
- mm-renderer input: `mmr_input_attach(ctx, url, "track")`. The URL for AA:
  `mdevpush:/dev/name/local/mdev?fourcc=H264&queue_capacity=60&video.frame_rate=<fps>&video.depth=16&video.width=<W>&video.height=<H>`
  Here W×H = the phone canvas (1280×720 by default), fps = `NBT_VID_FPS` or 60.
- The resource `/dev/name/local/mdev` is created by mm-renderer itself (mdevpush / "mdev_streamer") **after** `mmr_input_attach`. If
  there is no context, the name does not appear and all frames are lost (comment in `init()`).
- Feeding: `mdev_fd_ = name_open("/dev/name/local/mdev", 0)`, then `MsgSendvs_r(coid, iov, n, NULL, 0)` with
  `TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND|_NTO_TIMEOUT_REPLY, ...)` (`sendMdevSync/Format/Config`, `workerLoop`).

The mdev protocol (from `video_manager.cpp`). Header of 24 bytes: `u32 rsvd0..rsvd3 = 0`, `u32 msg_code`, `u32 payload_len`.

| msg_code | Purpose | Payload |
|---|---|---|
| 5 | Sync / Reset | `u32 = 1` (len 4) |
| 2 | Format / Geometry | 32 bytes: `magic=2`, `fourcc=0x48323634` ('H264'), `width`, `height` (the coded canvas, including hidden rows), `depth=16`, `fps`, `0`, `0` |
| 4 | Codec config | the SPS/PPS bytes exactly as the phone sent them (Annex B) |
| 3 | Data | `u64 pts` (µs), then [cached SPS/PPS], then the access unit (Annex B); `payload_len = 8 + cfg + au` |

Order: codec config arrived → 5 → 2 → 4. A frame arrived before init (or after `endSession()`, which sets
`reset_pending_`) → first 5 and 2. According to a v1.3.14 comment, Message 2 is sent again with every config: this is the only
observed way to get into `mdemux_process_packet`.

## 2. Starting and controlling mm-renderer

**Whose process.** By default the already running factory one is used. HMood starts it only as part of the
`CONN_CARPLAY` package (Connectivity.cfg, process 18, env entries 21-24), so without CarPlay it does not exist. `mmr_connect(NULL)` is repeated
every 0.5 s up to `NBT_MMR_WAIT_S` (90 s). If there is no mm-renderer process, `mmr::spawnStep()` (`mmr_launch.hpp`) starts its own:
immediately, if there is no carplay.ini (`/var/opt/conn/carplay/carplay.ini`, `/fs/sda1/opt/conn/carplay/carplay.ini`), or
`NBT_MMR_BOOT_S` (60 s) after boot, if carplay.ini exists but CarPlay is not active (**[not car]**, v1.9.27, issue #118).
On the Supra, mm-renderer is available 22.5 s after boot **[car]** (v1.5.9).

`Launcher::start()` (`mmr_launch.cpp`):
- `spawn("/opt/conn/bin/carplay/mm-renderer")`, args `-f -vvvvvv -r /pps/services/multimedia/renderer` (`-f` keeps the process
  in the foreground; otherwise `procmgr_daemon()` runs and the pid is lost).
- env: `LD_LIBRARY_PATH=/opt/conn/lib/carplay:/opt/conn/lib/carplay/mmedia:/boot/lib:/lib:/usr/lib:/opt/os/lib:/opt/sys/lib:/opt/core/lib:/opt/car/lib:/lib/graphics/omap5430`,
  `MM_RENDERER_CONFIG=/opt/conn/etc/carplay/mm-renderer.conf`, `MM_PLAYLIST_CONFIG=/opt/conn/etc/carplay/mm-playlist.conf`,
  `MM_INIT=/opt/conn/lib/carplay/mm`, `PATH`. **LD_PRELOAD is not needed** (comment: "process 18 needs no LD_PRELOAD").
- `SPAWN_SETSID|SETSIGDEF|SETSIGMASK|EXPLICIT_SCHED`, `SCHED_RR` prio 10; fd 0-2 → `/dev/null`.
- The pid is written to `/dev/shmem/nbt-aad-mmr.pid`: the next start picks the process up (`adopt()`). On exit,
  SIGTERM, then SIGKILL after 500 ms. The process is found by `/proc/<pid>/exefile` or `DCMD_PROC_MAPDEBUG_BASE`.

**The `createDecoder()` sequence** (under `decoder_mutex_`):
1. Go through `/pps/services/multimedia/renderer/context/*`. `aadvideoctx*` (their own old ones) are destroyed:
   `mmr_context_open` → `mmr_stop` → `mmr_context_destroy` (it also closes the handle; a second close = double close).
   `cpvideoctx*` are also destroyed by default; with "CarPlay compatibility" (`NBT_CP_PURGE=0`) a detach is done (see below).
   Other contexts are not touched. Then `usleep(50 ms)`.
2. `mmr_context_create(cnx, "aadvideoctx<pid>", 0, 0x1ff)`.
3. `mmr_context_parameters(dict{prefetch_mode=-1, VideoDecoderLowLatencyMode=1, ColorSpaceMode=1})`. mm-renderer takes the dictionary;
   do not destroy it yourself.
4. `mmr_output_attach(ctx, "screen:/any?winid=1282&nodstviewport=0&dstx=0&dsty=0&dstw=<dW>&dsth=<dH>&wingrp=cpvideogrp<pid>", "video")`.
5. `mmr_output_parameters(ctx, out, dict{mmf.SCREEN_WRITER_DISABLE_CADENCE=1, mmf.SCREEN_WRITER_DISABLE_MEDIACLOCK=1, mmf.SCREEN_WRITER_POST_BUFFER_NOSYNC=1})`.
6. `mmr_input_parameters(dict{VideoDecoderLowLatencyMode=1, ColorSpaceMode=1})`.
7. `mmr_speed_set(ctx, 1000)` **before** input attach.
8. `mmr_input_attach(ctx, "mdevpush:...", "track")`.
9. Repeat steps 3 and 6 **after** attach: according to their slog, the parameters set before attach were not applied.
10. `mmr_speed_set(1000)` → `mmr_play` → `mmr_speed_set(1000)` once more.
11. `name_open("/dev/name/local/mdev", 0)`.
On an input/play error: `mmr_error_info(ctx)`, the text at offset `+0x20`.

Explanations from the comments (their reverse engineering of `mmr-mmf-routing` / `mmr-track-engine`): with `prefetch_mode >= 0`, MmResume is not
called, and mdev_streamer gets stuck in state 0x40 (deadlock). If speed = 0 at the start of the track engine, `start()`
also skips MmResume.

**Stop** (`stop()`): `mmr_stop` → `mmr_context_destroy` → (re-attach of the factory context) → `mmr_disconnect` →
`dlclose` → `name_close(mdev)` → `screen_destroy_window/context` → stopping their own mm-renderer.

**Coexisting with the factory CarPlay** (`detachFactoryContext` / `reattachFactoryContext`, v1.9.50/57; the readme says: "good
feedback from the #156 testers"):
- detach: the PPS `/pps/services/multimedia/renderer/context/cpvideoctx<pid>/{output<N>,input,param}` are read
  (line format `name:enc:value`, parser in `mmr_pps.hpp`), then `mmr_context_open` → `mmr_stop` → `mmr_input_detach` →
  `mmr_output_detach(ctx, N)`. The context and handle of the factory CarPlay stay alive.
- re-attach: from the snapshot `mmr_context_parameters(param)` → `mmr_output_attach(url, type)` + `mmr_output_parameters` →
  (if there was an input) `mmr_input_attach` + `mmr_input_parameters` + `mmr_play`.
- A snapshot from the Supra (`tests/test_mmr_pps.cpp`, `cpvideoctx16838`): output0 url
  `screen:/any?winid=1280&nodstviewport=0&dstx=0&dsty=0&dstw=1280&dsth=480&wingrp=cpvideogrp16838`, the same three
  `mmf.SCREEN_WRITER_*=1`, `type::video`; param `prefetch_mode::0`; input (from the test)
  `mdevpush:/dev/name/local/mdev?fourcc=AVC1`, `type::track`.
- Without detach (`retuneForCarPlay`): their own context is retuned for CarPlay. `mmr_input_detach`, `mmr_output_detach`,
  `prefetch_mode=0`, output `winid=1280` (without `wingrp`, otherwise the CarPlay window becomes a child of our hidden one and
  stays black), input `mdevpush:...?fourcc=AVC1&video.frame_rate=60&video.depth=16&video.width=<iniW>&video.height=<iniH>`
  (without `queue_capacity`), then `mmr_play`. The output can be changed only while the input is detached (otherwise err 7).

## 3. How the picture gets on top of the iDrive HMI

**Their own window** (`createWindow()`):
`screen_create_context(APPLICATION)` → `screen_create_window_type(APPLICATION_WINDOW)` → binding to display 0 through
`SCREEN_PROPERTY_DISPLAYS`/`DISPLAY` (without it mm-renderer complains `SCREEN_PROPERTY_DISPLAY failed errno = 2`) →
`screen_create_window_group(win, "cpvideogrp<pid>")` → `USAGE = SCREEN_USAGE_VIDEO` → `ID_STRING = "1282"` →
`SOURCE_SIZE` = panel size, `POSITION 0,0` → `FORMAT RGBA8888` → `screen_create_window_buffers(1)` → `screen_fill`
with black → `screen_post_window` → `VISIBLE 0`, `ZORDER -1000`. Then through the shell: `LayerManagerControl set surface 1282
visibility 0; ... opacity 1.0; ... zorder 2000`. With `NBT_CP_HANDOFF` (the default), the window and context are created only
when a phone connects, so as not to take surface 1280 away from the factory CarPlay.

**The LayerManager mechanism** (their decompilation of LayerManagerService, `tools/osd1281probe.cpp`): on a POST event of a window (or
on a change of `ID_STRING`), the service parses the id string as a number. If there is no surface with that number, it creates one
and binds the window to it as native content. The surface goes to the **last** window with this id that did a post.
The mm-renderer output window gets the same id (`winid=1282`) and joins our group (`wingrp`). Surface 1282 appears
about 0.5-1 s after the first post; `serviceAaSurface()` polls `LayerManagerControl get surface
1282` every 250 ms (the `native surface:` line) and repeats the show once the surface has appeared.

**Show / hide** (`setVisible()`, in one `system()` for speed, `/opt/sys/bin/LayerManagerControl`):
- show: `set layer 0x80000000 render order 0x502` (with the volume bar `0x501,0x502`); `set surface 1282 source region
  0 0 W H`; `... destination region 0 0 W H`; `set surface 256 visibility 0`; `set surface 1282 visibility 1`.
- hide: `set layer 0x80000000 render order 0x100`; `set surface 256 visibility 1`; `set surface 1282 visibility 0`.
- When hiding, the decoder **stays open** (reversing camera, park assist and so on): only the surface is hidden.
- An atomic alternative (`favourite_composition.hpp`): `dlopen("/opt/sys/lib/libilmClient.gpp.so.1")` →
  `ilm_init`, `ilm_surfaceSetVisibility(id, v)`, `ilm_layerSetRenderOrder(0x80000000, ids, n)`, `ilm_commitChanges`,
  `ilm_destroy`. On an error it falls back to the CLI.
- `LD_PRELOAD` is removed for children started through `system()` (`NBT_CHILD_NO_PRELOAD`, v1.9.69). Otherwise every `LayerManagerControl`
  pulls in the graphics stack (`unknown symbol: kdMain`), and `audioconn` crashed on `libEGL_X.gpp.so`.

**IDs** (all checked on the Supra; on our N18032I, verify with `LayerManagerControl get screen 0` / `get layer` / `get surface`):

| ID | What it is |
|---|---|
| surface 256 (0x100) | iDrive HMI (HmiMain3D), normal order of layer 0x80000000 |
| layer 0x80000000 | the head-up display layer; on this unit the **first** surface in the render order list is drawn on top **[car]** |
| surface 1280 (0x500) | the factory CarPlay window (`carplay.ini WindowName=1280`) |
| layer 0x800d0000 | the CarPlay layer; holds 1280 after the first use of CarPlay; iDrive leaves it hidden and offset |
| surface 1281 (0x501) | the NBT-AAD volume bar **[car]** |
| surface 1282 (0x502) | the AA picture (`NBT_AA_SURFACE`, allowed 1282-65535 or 1280) **[car]** |
| surface 1283 (0x503) | a copy of the top strip of iDrive (Favourite keys preview) |

Screen limitations: `SCREEN_PROPERTY_*` in `include/screen_video.h` is **not verified**. In the header VISIBLE=43, while the probe
says LayerManagerService uses 0x33; SOURCE_SIZE and BUFFER_SIZE are both 40. Only hiding and showing through LayerManager really
work. Whether the Screen z-order of the window (2000 / -1000) affects anything is not clarified in the repo.

**`lm_geometry.hpp`** is needed only in the old shared-surface mode (`NBT_AA_SURFACE=1280`). It saves the factory
geometry of surface 1280 (parsing the `source region` / `destination region` lines of `LayerManagerControl get surface 1280`;
saved in `/dev/shmem/nbt-aad-lm1280` as `src x y w h dst x y w h`; fallback: carplay.ini `[Screen]`
Width/Height) and restores it on every hide. Otherwise CarPlay with the 1180_480 encoding was shown enlarged
until a reboot. In the same mode `fixCarPlayLayer()` makes layer 0x800d0000 visible and full screen.

**The iDrive side:** the HmiMain3D plugin `libhmiaad.so` (`plugin/`, ABI from `libhmiplugin.so`, the compiler must be
`"GCC, Version 4.4.2"`, build `_gpp`) adds an entry to the Communication menu (`ST_Com_Selection`) and starts
`/opt/aa/nbt-aad`. The RHMI application `NbtAad` keeps iDrive on an empty state (state 13) while AA is shown.
`status_hint.hpp` is not about video: these are hints for the status line (`/dev/shmem/nbt-aad-hint`, "net|hint").

## 4. Latency, buffers, key frames, SPS/PPS, resolution

- mm-renderer parameters (section 2): `prefetch_mode=-1`, `VideoDecoderLowLatencyMode=1`, `ColorSpaceMode=1`, three
  `mmf.SCREEN_WRITER_DISABLE_CADENCE/DISABLE_MEDIACLOCK/POST_BUFFER_NOSYNC=1`, speed 1000 before attach and around play.
- PTS in Message 3 = `CLOCK_MONOTONIC` in µs **at the moment of sending**; the phone's PTS is not used (comment: `mdev_streamer`
  does the same, `clock_gettime(2)`/GraphClock).
- The receive thread → a queue (`frame_queue.hpp`, 15 frames by default, `NBT_VIDEO_QUEUE` 1-60, no copying of bytes)
  → the worker thread. On overflow, the **oldest frame that is neither config nor IDR** is dropped.
- A frame is sent to mdev **in one attempt with a 250 ms timeout**. There are no retries: in v1.3.12 retries held the worker
  for about 100 s. Control messages: 1 s timeout, up to 51 attempts at 10 ms intervals, exit on `-ETIMEDOUT`.
- SPS/PPS: cached from the AA message 0x0001 and sent as Message 4, and then **glued to every frame** in the iov
  of Message 3. At the end of a session the cache and the queue are cleared, and mdev gets Sync/Format again.
- An IDR is detected by NAL type 5 in the first 128 bytes of the AU (`looksLikeIdr`). After the context is recreated, frames
  are dropped until the first IDR (`wait_idr_`; if there is no IDR, frames are let through after 6 s; **[not car]**, v1.9.16).
- The key frame request in AA: video focus UNFOCUSED, then FOCUSED after 500 ms (`keyframe_ask.hpp`: not more than once per 10 s, not in
  the first 5 s of a session). It triggers after a lost frame if there is no IDR in the queue behind it; `NBT_DROP_IDR=0` switches it off
  (**[not car]**, v1.9.53). CarPlay needs its own way to ask the iPhone for an IDR: this repo has none.
- AA frames are acked immediately on arrival, with a window of `max_unacked=10` for video. This is specific to AA.
- Resolution: the phone draws a canvas `NBT_VID_RES` (1=800×480, 2=1280×720 by default, 3=1920×1080, the last one crashed a
  2 GB unit). Format/URL get the canvas size. `dstw/dsth` from `mmrDstWidth/Height()` (`car_config.hpp`) scale
  uniformly; on a 1280×480 panel a 720p canvas gives dst 1280×720, and the top 480 rows are visible 1:1 **[car]**. The panel is taken
  from carplay.ini `[Screen]`, `LayerManagerControl get screen 0` (`resolution: x=..., y=...`) or `NBT_SCREEN`.
- Frame rate: the phone sends about 30 fps, the decoder is declared at 60 by default. On the F22 N18032I, `NBT_VID_FPS=30` (the decoder
  also at 30) gave a smoother picture **[car]**, PR #66. For our 1280×480 @30 stream the natural parameters are:
  `video.width=1280&video.height=480&video.frame_rate=30`, Format 1280×480, `dstw=1280&dsth=480`.
- High profile / level 3.2: the repo does **not mention** decoder limits on profile and level, and it does not say which profile AA sends either.

## 5. Input and audio (short, for later)

- The source here: the readme, architecture section. `src/car/ccs_input.*` and `src/touch_probe.cpp` were not read line by line.
- Touch: UDP datagrams to port **30567** (from `160.48.199.115:30501`); the HMI listens on this port too. The daemon takes the port only
  while AA is on the screen. Format: header `B0 04 80 02` + BE32 length, records of 8 bytes (C5/C1/C3), contacts
  `x_hi x_lo y_hi y_lo size (state<<4|id)` in panel pixels (`touch_frame.hpp`).
- Controller and buttons: an observing MOST client to `DSYSMOSTServer` 4.5, controller status FBlock 0xE5 (`most/client`,
  `car/ccs_input`, `car/ccs_decoder.hpp`). iDrive is held on the empty state 13 during this.
- Default audio: Bluetooth A2DP (`audioconn -b 0x3a 0x1`). A2I (`/opt/mm/lib/liba2i_client.so`, cookie
  `0x0ABABABA` as in the factory CarPlay): `/input_stereo_AirPlayMain` (44.1 kHz stereo, fallback `/input_stereo_4`),
  `/input_stereo_AirPlayAlt` (navigation), `/input_mono_AirPlayDL` (speech), `/output_mono_AirPlayUL` (microphone after
  echo cancellation; Nuance scenario `Carplay_Siri_1mic`).
- **Important for CarPlay:** according to `browser_audio.hpp`, the CarPlay sources (connections 0x314-0x31e) are not allocated while the
  factory stack does not see a connected iPhone, so the PCM into AirPlayMain stays silent. The AA workaround: a browser source
  (connection 0xb8 = `eSRC_BROWSER`, shim `/opt/mm/lib/libasound/libasound.so` → `input_stereo_3`). The A2I mode is marked in the readme
  as experimental ("nothing is audible yet").

## 6. License

- `LICENSE.md`: **PolyForm Noncommercial License 1.0.0** (added 2026-10-23; the 10 commits before it had no license).
  The code may be used, changed, used to make new works and distributed, but **only for noncommercial
  purposes**. Mandatory conditions: pass on the license text (or its URL) and the line
  "Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)".
  Sublicensing is not allowed. The README separately forbids selling it and including it in paid packages.
- A short quote from LICENSE.md: "Any noncommercial purpose is a permitted purpose."
- Conclusion (not legal advice): for a noncommercial hobby project, copying the code is allowed if the Notice is kept,
  but such parts get the NC restriction and are not compatible with the MIT/GPL license of the project. If selling or
  a paid installation (including a bundle with anti-theft/binding) is possible at all, it is safer to write our own code from the
  interface facts in this file (paths, URLs, message codes, IDs) and not to carry over any code.

## What is not in the repository / what to check on our unit

- The contents of `/opt/conn/etc/carplay/mm-renderer.conf`, the name and path of the IVA-HD library, the profile and level limits,
  the mdev format for `fourcc=AVC1`.
- The surface and layer IDs on `NBTevo_N18032I` (256, 1280, 0x80000000, 0x800d0000; whether 1282 is free).
- Whether our CarPlay is coded (`/var/opt/conn/carplay/carplay.ini`), whether the factory mm-renderer is running and whether
  `cpvideoctx<pid>` exists: this decides whether spawn and detach are needed.
