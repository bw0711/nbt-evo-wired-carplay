# Audio over the cable: why there is none and how to enable uncompressed PCM (analysis of 2026-11-27)

The owner's goal: uncompressed audio over the cable, as in factory wired CarPlay. Car visit of 2026-11-27: over the cable
the iPhone did not open a single audio stream to the head unit (no `[CarPlay][AudioStream]`, no `Audio SETUP received`);
music, Siri and calls stayed in the phone. Wireless CarPlay on the same head unit does send audio to the car (AAC).

I analysed the car's `libconn_carplay.so` (`car/runs/20261126-195127/files-conn/opt_conn_lib_carplay_devctrl_libconn_carplay.so`)
and `libconn_carplay_audio_a2i.so`. Tool: `tools/annot.py` (capstone; call names via the PLT, strings
via the GOT, `--got=r4` if the GOT base is set in a register before the range). 📄 = seen in the code, 🤔 = inference.

## What was found
1. 📄 **The formats offered to the iPhone come only from `[Audio] FormatSupport` in `carplay.ini`.**
   `AirPlayReceiverServerPlatformCopyProperty` (0x3c424) → `APSAudioSessionGetSupportedFormats()` →
   `CarPlayAudioSessionGetSupportedFormats` (0x30b64): stream 100 → table +0x27c, 101 → +0x288, 102 → +0x294; each is a
   `map<Type, vector<uint64>>`; the mask = OR of all rows of that `Type`. There is no check of the channel type (WiFi/USB/"?") on the way.
2. 📄 **Row parsing** (`CCarPlayServiceCore::readConfig()` 0x33f94, range 0x35e14–0x36390):
   `getParamCount("Audio","FormatSupport")`, then `getMultiParams` for each row → key:value pairs.
   - `Stream`: `MainAudio` (100), `AltAudio` (101), `MainHighAudio` (102).
   - `Type`: any string (the table key).
   - `Format`: `PCM` → `'lpcm'`, `ALAC` → `'alac'`, `AACLC` → `'aac '`, `AACELD` → `'aace'`, `OPUS` → `'opus'`.
   - `Bit`: compared **as a string** with `"16"` / `"24"` (for lpcm it sets the bit depth, for alac it sets flags).
   - `SR`, `Channel`, `InLatency`, `OutLatency`: `atoi`.
   - then `ASBDToAirPlayAudioFormat()` → the AirPlay format bit, `push_back` into the stream's table.
3. 📄 **Bit table** (strings in .rodata, order = bit number starting from 2): `PCM/8000/16/1`, `/2`, `PCM/16000/16/1`, …,
   `PCM/44100/16/1`, `/2`, `PCM/44100/24/1`, `/2`, `PCM/48000/16/1`, `/2`, `PCM/48000/24/…`, `ALAC/…`, `AAC-LC/…`,
   `AAC-ELD/…`, `OPUS/…`. This matches the bits of `xcertplay`/the original project (8 kHz mono = bit 2, 48 kHz 16 bit = 14/15).
4. 📄 **The car's `carplay.ini` has only AAC** (the wireless set): AAC-ELD for default/telephony/speechRecognition/alert/
   AltAudio, AAC-LC for `MainHighAudio` media. 🤔 Over the cable the iPhone wants PCM (the original project gets only LPCM
   from the iPhone over the cable and declares only LPCM); when it does not find it, it does not open audio. This is the main and the
   only cause found; it needs to be checked in the car.
5. 📄 **The A2I output accepts uncompressed audio.** `AudioStreamPrepare` (a2i 0x26e0) writes PCM to A2I (`A2I_Client_Out_Write`);
   for AAC the receiver decodes it itself before that (`AudioConverter`, fdkaac). Before preparing a stream, A2I calls
   `CarPlayCheckAudioFormatSupport(streamType, audioType, sampleRate, channels, input)` (libconn 0x30658), which checks against the same
   table from `FormatSupport`; on refusal the log says `format check error`. For "media" there is an extra condition on the flag
   `input`, which I did not fully analyse (🤔): if it refuses, it will be visible in the log at once.
6. 📄 `NetTransportTypeIsWireless( inSession->transportType )` is only in `AirPlayReceiverSessionSendiAPMessage`:
   iAP inside the AirPlay session is wireless only (this agrees with the `mm-ipod` probe of 2026-11-27).
7. 📄 **Which file is live:** `/var/opt/conn/carplay/carplay.ini`. Connectivity (`CConnOnOffDipoClient`) builds it from
   `/opt/conn/data/carplay/carplay.ini` and rebuilds it only when the coding changes ("Dipo: no values have changed in
   carplay.ini"). nbt-aa once edited `[iAP2] LibPath` in it (copy `carplay.ini.nbt-orig`); its script does not
   touch the file. It is read when Connectivity starts, so after an edit the head unit must be rebooted.

## The edit (ready, not applied)
`car/make_pcm_ini.py` adds 29 rows of 16-bit PCM after the AAC rows (it does not touch them); the set = "wired" of the original project:

| Stream | Type | PCM |
|---|---|---|
| MainAudio | default, compatibility, speechRecognition | 24, 32, 44.1, 48 kHz mono |
| MainAudio | media | 44.1, 48 kHz stereo |
| MainAudio | telephony | 8, 16, 24 kHz mono |
| MainAudio | alert | 44.1, 48 kHz mono and stereo |
| AltAudio | default, compatibility | 44.1, 48 kHz mono and stereo |

Latencies are the same as in the car's rows of the same type. The masks were checked: all 8 (stream, type) pairs match the original project.

`car-chip-test.sh` stages: `pcm-check` (read), `pcm-apply` (a copy `carplay.ini.before-pcm` on the head unit and in the run folder,
a write with an SHA check, refuses if PCM is already there or the copy already exists), `pcm-revert` (copy back, SHA check).
Checked on the Mac on a copy of the car's file: apply → 37 FormatSupport rows, a second apply is refused, revert is byte for byte.

## Not checked
- That the iPhone over the cable really opens streams with PCM (the main version, but an inference).
- The `input` flag in `CarPlayCheckAudioFormatSupport` for media/microphone.
- Whether a wireless iPhone will start to choose PCM instead of AAC (Wi-Fi can carry it, but the behaviour will change); check on the spot.
- Microphone: the input mask is apparently the same type mask (telephony 8/16/24 kHz, Siri 24–48 kHz mono).
