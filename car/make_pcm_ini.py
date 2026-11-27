#!/usr/bin/env python3
"""make_pcm_ini.py <car's carplay.ini> <out>

The head unit's CarPlay receiver offers the iPhone only the audio formats listed in [Audio] FormatSupport lines of
/var/opt/conn/carplay/carplay.ini. The car's file lists only AAC-ELD / AAC-LC (the wireless set), and over the cable
the iPhone opens no audio stream at all (car 2026-11-27). This adds uncompressed 16-bit PCM lines - the wired set the
working upstream receiver offers (xcertplay CarPlayAudioFormats) - and keeps every existing line, so wireless keeps its
AAC. RE of the parser: wired-carplay/audio-pcm-re-2026-11-27.md.

Parser facts the lines rely on (libconn_carplay.so CCarPlayServiceCore::readConfig):
  Stream: MainAudio (100) | AltAudio (101) | MainHighAudio (102); Type: any string, one format mask per Type
  Format: PCM -> 'lpcm' (also ALAC, AACLC, AACELD, OPUS); Bit compared as the string "16" or "24"; SR, Channel: atoi
  InLatency / OutLatency: atoi (microseconds)
Fails (exit 1) if the input has no [Audio] section or already has PCM lines.
"""
import sys

MARK = "# --- wired CarPlay: uncompressed PCM (added by make_pcm_ini.py) ---"

# (stream, type, [(sample rate, channels)], in latency, out latency); latencies as the car's own lines of that type
PCM = [
    ("MainAudio", "default", [(24000, 1), (32000, 1), (44100, 1), (48000, 1)], 83400, 140000),
    ("MainAudio", "compatibility", [(24000, 1), (32000, 1), (44100, 1), (48000, 1)], 83400, 140000),
    ("MainAudio", "media", [(44100, 2), (48000, 2)], 0, 131000),
    ("MainAudio", "telephony", [(8000, 1), (16000, 1), (24000, 1)], 133400, 190000),
    ("MainAudio", "speechRecognition", [(24000, 1), (32000, 1), (44100, 1), (48000, 1)], 133400, 190000),
    ("MainAudio", "alert", [(44100, 1), (44100, 2), (48000, 1), (48000, 2)], 0, 50000),
    ("AltAudio", "default", [(44100, 1), (44100, 2), (48000, 1), (48000, 2)], 0, 131000),
    ("AltAudio", "compatibility", [(44100, 1), (44100, 2), (48000, 1), (48000, 2)], 0, 131000),
]


def pcm_lines():
    out = [MARK]
    for stream, typ, fmts, lin, lout in PCM:
        for sr, ch in fmts:
            out.append("FormatSupport=Stream:%s;Type:%s;Format:PCM;SR:%d;Bit:16;Channel:%d;InLatency:%d;OutLatency:%d"
                       % (stream, typ, sr, ch, lin, lout))
    out.append("# --- end of the PCM lines ---")
    return out


def main():
    src = open(sys.argv[1], "rb").read()
    crlf = b"\r\n" in src
    text = src.decode("latin1")
    lines = text.replace("\r\n", "\n").split("\n")
    if any("Format:PCM" in l for l in lines):
        sys.exit("already has PCM lines: nothing to do")
    try:
        a = next(i for i, l in enumerate(lines) if l.strip() == "[Audio]")
    except StopIteration:
        sys.exit("no [Audio] section")
    # after the section's last FormatSupport line
    last = a
    for i in range(a + 1, len(lines)):
        if lines[i].strip().startswith("["):
            break
        if lines[i].startswith("FormatSupport="):
            last = i
    new = lines[:last + 1] + pcm_lines() + lines[last + 1:]
    data = "\n".join(new)
    if crlf:
        data = data.replace("\n", "\r\n")
    open(sys.argv[2], "wb").write(data.encode("latin1"))
    print("%d PCM lines added after line %d" % (len(pcm_lines()) - 2, last + 1))


main()
