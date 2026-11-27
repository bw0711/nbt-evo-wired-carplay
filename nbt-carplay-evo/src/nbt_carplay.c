/* nbt_carplay: wired CarPlay on the NBT, one iPhone session (started by native_service when an iPhone is plugged in;
 * docs/CARPLAY.md).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 * (license and the section 7 permission for the head unit's libraries: NOTICE.md, "GPLv3 parts")
 *
 *   USB (cp_usb.c)        the iPhone's CarPlay configuration: multiplexor + NCM
 *   network (cp_ncm.c)    NCM <-> lwIP, IPv6 link-local (step a)
 *   lockdown, iAP2        pairing ("Trust", once), carkit service, identification, authentication with the car's
 *                         chip, CarPlayStartSession (cp_lockdown.c, cp_iap2.c)
 *   AirPlay (step b)      the iPhone connects to port 7000: pairing, auth-setup, streams (cp_airplay.c)
 *   screen (step c)       H.264 into nbt_decoder's FIFO (cp_screen.c)
 *   audio, microphone     the car's audio manager (cp_audio.c, cp_mic.c)
 *   keys                  nbt_knob's key lines -> knob / media HID, Siri (cp_input.c)
 *
 * usage: nbt_carplay [--stop-after usb|iap2|net|airplay|video [--seconds N]] [--pcap FILE] [--dump FILE]
 *                    [--audio-dump DIR] [--no-audio] [--no-mic] [--fps N] [--knob-invert] [--state DIR]
 *                    [--fifo PATH|none] [--input PATH]
 *   --stop-after   end N seconds (default 15) after that step worked: usb = configuration and pipes, iap2 =
 *                  CarPlayStartSession sent, net = the iPhone connected to us over NCM, airplay = pairing and the
 *                  screen stream set up, video = 60 frames written
 * Environment (native_service): NBT_AA_MEDIA_OUT (default input_stereo_1), NBT_AA_PROMPT_OUT (prompts while another
 * source plays, e.g. input_stereo_3), NBT_AUX_SOURCE (our main source, default f6), NBT_KNOB_INVERT=1.
 * Exit status: 0 after a session, 1 when it could not start, 2 wrong arguments.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "psa/crypto.h"
#include "cp.h"
#include "cp_lockdown.h"
#include "cp_usb.h"

struct cp_options g_opt;
volatile int g_stop;

static volatile long long g_reached_at;

void cp_reached(int step)
{
    if (g_opt.stop_after == step && !g_reached_at) {
        g_reached_at = ms_now();
        say("nbt_carplay: step reached; ending in %d s (--stop-after)", g_opt.seconds);
    }
}

static void* watch(void* arg)
{
    (void)arg;
    while (!g_stop) {
        if (g_reached_at && ms_now() - g_reached_at >= g_opt.seconds * 1000LL) {
            say("nbt_carplay: --stop-after: done");
            g_stop = 1;
        }
        if (usb_gone())
            g_stop = 1;
        if (g_opt.auto_mode && !g_stop && auto_ended()) {   /* Evo port: the service starts us again */
            say("nbt_carplay: the head unit ended the session; ending too");
            g_stop = 1;
        }
        sleep_ms(200);
    }
    return 0;
}

/* Evo port: on this head unit a core dump restarts it (car 2026-11-26), so a fault ends us without one */
static void fatal(int s)
{
    static const char m[] = "nbt_carplay: fatal signal; ending without a core dump (a dump restarts the head unit)\n";
    (void)!write(2, m, sizeof(m) - 1);
    _exit(128 + s);
}

static void stopped(int s)
{
    (void)s;
    g_stop = 1;
}

static struct usb g_usb;

static unsigned long long g_send_errors;

static int ncm_send(void* ctx, const unsigned char* block, unsigned n)
{
    unsigned actual = 0;
    const int rc = usb_xfer(&g_usb.ncm_out, (void*)block, n, 1000, &actual);
    (void)ctx;
    if (rc || actual != n) {
        if (++g_send_errors <= 5)
            say("ncm: send of %u bytes: result %d, status %08x, %u sent", n, rc, g_usb.ncm_out.status, actual);
        return -1;
    }
    return 0;
}

/* NCM in, with what the transfers end with (car 2026-11-23: no frame came from the iPhone; the reads may fail) */
static void* ncm_rx(void* arg)
{
    static unsigned char b[65536];
    unsigned long long reads = 0, empty = 0, bytes = 0, fin, fout;
    unsigned last_status = 0, logged = 0;
    long long next = ms_now() + 10000;
    (void)arg;
    while (!g_stop && !usb_gone()) {
        unsigned actual = 0;
        const int rc = usb_xfer(&g_usb.ncm_in, b, g_usb.ntb_in + 2048, 1000, &actual);
        if (actual) {
            if (reads++ < 6)   /* whole, to read a block format the parser does not know (car 2026-11-24: "ncmx") */
                hexdump("ncm: transfer in", b, actual < 512 ? actual : 512);
            bytes += actual;
            ncm_input(b, actual);
        } else {
            ++empty;
            if (g_usb.ncm_in.status != last_status && logged < 8) {
                ++logged;
                say("ncm: read ended without data: result %d, status %08x", rc, g_usb.ncm_in.status);
            }
            last_status = g_usb.ncm_in.status;
            if (rc && rc != ETIMEDOUT)
                sleep_ms(100);
        }
        if (ms_now() >= next) {
            next = ms_now() + 10000;
            ncm_counts(&fin, &fout);
            say("ncm: %llu transfers in (%llu bytes), %llu empty, frames %llu in / %llu out, %llu send errors", reads,
                bytes, empty, fin, fout, g_send_errors);
        }
    }
    return 0;
}

static void* ncm_notify(void* arg)
{
    unsigned char b[64];
    unsigned n = 0;
    (void)arg;
    while (!g_stop && !usb_gone()) {
        unsigned actual = 0;
        const int rc = usb_xfer(&g_usb.ncm_int, b, sizeof(b), 1000, &actual);
        if (actual && n++ < 10)
            hexdump("ncm: notification", b, actual);
        else if (!actual && rc && rc != ETIMEDOUT)
            sleep_ms(500);
    }
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: nbt_carplay [--stop-after usb|iap2|net|airplay|video [--seconds N]] [--pcap FILE] "
                    "[--dump FILE] [--audio-dump DIR] [--no-audio] [--no-mic] [--fps N] [--knob-invert] "
                    "[--state DIR] [--fifo PATH|none] [--input PATH] [--bridge|--bridge-start|--bridge-ctrl BTADDR]\n"
                    "  --bridge (NBT Evo): no session of our own; the iPhone's USB network becomes the head unit's tap0\n"
                    "  --bridge-start (NBT Evo): --bridge, and CarPlayStartSession sends the iPhone to the head unit's\n"
                    "    own AirPlay receiver as it announces itself on tap0\n"
                    "  --bridge-ctrl BTADDR (NBT Evo): --bridge-start, after the head unit's CarPlay has connected to a\n"
                    "    controller announced on tap0 in the iPhone's name (BTADDR: the iPhone's Bluetooth address)\n"
                    "           for the factory CarPlay (needs the iPhone paired by Bluetooth with wireless CarPlay on)\n"
                    "  --auto (NBT Evo): the service: wait for an iPhone and for the head unit to look for it (its Wi-Fi off),\n"
                    "    then --bridge-start --iap2-factory until it is unplugged or the session ends; pair record in\n"
                    "    " AUTO_STATE " (--no-iap2-factory: without iAP2 for the cluster)\n"
                    "  --iap2-factory (NBT Evo): answer the head unit's own iAP2 driver (mm-ipod) in the iPhone's place\n"
                    "    through wired-carplay/iap2-adapter: now playing and calls for the cluster and head-up display\n"
                    "    (--iap2-factory-noauth: without asking mm-ipod's signature, for an emulator without the chip)\n");
    return 2;
}

int main(int argc, char** argv)
{
    static const char* steps[] = {"", "usb", "iap2", "net", "airplay", "video"};
    struct iap2_carplay cp;
    struct stream* carkit;
    char mac[20];
    pthread_t t;
    int i, r, state_given = 0;
    setvbuf(stdout, 0, _IOLBF, 0);
    g_opt.width = 1280;
    g_opt.height = 480;
    g_opt.fps = 60;
    g_opt.width_mm = 243;   /* 10.25 inch, 8:3 */
    g_opt.height_mm = 91;
    g_opt.seconds = 15;
    g_opt.state_dir = CP_RAM "/nbt-carplay-state";   /* Evo port: RAM (the NBT kept it under /var/opt/hmi, i.e. flash) */
    g_opt.fifo = CP_RAM "/nbt-aa-native/annexb.fifo";
    g_opt.input = CP_RAM "/nbt-aa-native/input.fifo";
    g_opt.audio = g_opt.mic = 1;
    g_opt.media_out = getenv("NBT_AA_MEDIA_OUT") ? getenv("NBT_AA_MEDIA_OUT") : "input_stereo_1";
    g_opt.prompt_out = getenv("NBT_AA_PROMPT_OUT");
    g_opt.aux_source = getenv("NBT_AUX_SOURCE") ? (unsigned)strtoul(getenv("NBT_AUX_SOURCE"), 0, 16) : 0xf6;
    g_opt.knob_invert = getenv("NBT_KNOB_INVERT") && !strcmp(getenv("NBT_KNOB_INVERT"), "1");
    for (i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--stop-after") && v) {
            unsigned k;
            for (k = 1; k < sizeof(steps) / sizeof(steps[0]) && strcmp(v, steps[k]); ++k) {
            }
            if (k == sizeof(steps) / sizeof(steps[0]))
                return usage();
            g_opt.stop_after = (int)k;
            ++i;
        } else if (!strcmp(a, "--seconds") && v) {
            g_opt.seconds = atoi(v);
            ++i;
        } else if (!strcmp(a, "--pcap") && v) {
            g_opt.pcap = v;
            ++i;
        } else if (!strcmp(a, "--dump") && v) {
            g_opt.dump = v;
            ++i;
        } else if (!strcmp(a, "--audio-dump") && v) {
            g_opt.audio_dump = v;
            ++i;
        } else if (!strcmp(a, "--state") && v) {
            g_opt.state_dir = v;
            state_given = 1;
            ++i;
        } else if (!strcmp(a, "--fifo") && v) {
            g_opt.fifo = strcmp(v, "none") ? v : 0;
            ++i;
        } else if (!strcmp(a, "--input") && v) {
            g_opt.input = v;
            ++i;
        } else if (!strcmp(a, "--fps") && v) {
            g_opt.fps = atoi(v);
            ++i;
        } else if (!strcmp(a, "--no-audio"))
            g_opt.audio = g_opt.mic = 0;
        else if (!strcmp(a, "--no-mic"))
            g_opt.mic = 0;
        else if (!strcmp(a, "--bridge"))   /* Evo port */
            g_opt.bridge = 1;
        else if (!strcmp(a, "--bridge-start"))   /* Evo port */
            g_opt.bridge = g_opt.bridge_start = 1;
        else if (!strcmp(a, "--bridge-ctrl") && v && strlen(v) == 17) {   /* Evo port: the iPhone's BT address */
            static char id[18];
            unsigned j;
            for (j = 0; j < 17; ++j)
                id[j] = (char)(v[j] >= 'a' && v[j] <= 'f' ? v[j] - 'a' + 'A' : v[j]);
            id[17] = 0;
            g_opt.bridge = g_opt.bridge_start = 1;
            g_opt.bridge_ctrl = id;
            ++i;
        }
        else if (!strcmp(a, "--iap2-factory"))   /* Evo port */
            g_opt.iap2_factory = 1;
        else if (!strcmp(a, "--auto")) {   /* Evo port: the service (cp_auto.c) */
            g_opt.auto_mode = g_opt.bridge = g_opt.bridge_start = 1;
            if (!g_opt.iap2_factory)
                g_opt.iap2_factory = 1;
            if (!state_given)
                g_opt.state_dir = AUTO_STATE;
        }
        else if (!strcmp(a, "--no-iap2-factory"))   /* Evo port: --auto without iAP2 for the cluster */
            g_opt.iap2_factory = -1;
        else if (!strcmp(a, "--iap2-factory-noauth"))   /* Evo port: an emulator without the chip */
            g_opt.iap2_factory = 2;
        else if (!strcmp(a, "--knob-invert"))
            g_opt.knob_invert = 1;
        else
            return usage();
    }
    if (g_opt.iap2_factory < 0)
        g_opt.iap2_factory = 0;
    if (g_opt.fps < 15 || g_opt.fps > 60 || g_opt.seconds < 0 || (g_opt.auto_mode && g_opt.stop_after))
        return usage();
    mkdir(CP_RAM "/nbt-aa-native", 0755);   /* Evo port: on the NBT native_service made it */
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, fatal);
    signal(SIGBUS, fatal);
    signal(SIGILL, fatal);
    signal(SIGFPE, fatal);
    signal(SIGABRT, fatal);
    say("nbt_carplay: %dx%d at %d fps, state in %s%s%s", g_opt.width, g_opt.height, g_opt.fps, g_opt.state_dir,
        g_opt.stop_after ? ", stop after " : "", g_opt.stop_after ? steps[g_opt.stop_after] : "");
    if (cp_random_init() || psa_crypto_init() != PSA_SUCCESS) {
        say("nbt_carplay: no random numbers");
        return 1;
    }
    if (acp_open()) {
        say("nbt_carplay: the authentication chip does not answer");
        return 1;
    }
    airplay_identity();
    if (pthread_create(&t, 0, watch, 0))
        return 1;
    if (usb_open(&g_usb, g_opt.auto_mode ? 100000000u : 60)) {   /* --auto: as long as it takes */
        if (g_opt.auto_mode)
            auto_finish();
        return g_opt.auto_mode && !g_stop && !auto_taken() ? 0 : 1;   /* 0: unplugged while waiting */
    }
    if (g_opt.pcap)
        ncm_pcap(g_opt.pcap);
    if (g_opt.bridge) {   /* Evo port: the link into the head unit's network, nothing of our own on it */
        if (ncm_bridge_start(g_usb.mac, ncm_send, 0) || pthread_create(&t, 0, ncm_rx, 0))
            return 1;
        if (g_opt.bridge_ctrl)
            ncm_bridge_ctrl(g_opt.bridge_ctrl, ctrl_start(ncm_bridge_ip4()));
    } else if (net_init() || ncm_start(g_usb.mac, ncm_send, 0) || pthread_create(&t, 0, ncm_rx, 0))
        return 1;
    if (g_usb.have_int)
        pthread_create(&t, 0, ncm_notify, 0);
    cp_reached(STOP_USB);
    if (!g_opt.bridge) {
        if (airplay_start(AIRPLAY_PORT))
            return 1;
        if (g_opt.audio)
            audio_start();
        input_start();
    }
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", g_usb.mac[0], g_usb.mac[1], g_usb.mac[2],
             g_usb.mac[3], g_usb.mac[4], g_usb.mac[5]);
    memset(&cp, 0, sizeof(cp));
    cp.address = ncm_address();
    cp.port = AIRPLAY_PORT;
    cp.mac = mac;
    cp.public_key = airplay_public_hex();
    cp.source_version = SOURCE_VERSION;
    cp.ncm_interface = g_usb.ncm_ctrl;
    r = -1;
    if (!g_stop && !carkit_connect(&g_usb, &carkit))
        r = iap2_run(carkit, &cp);
    g_stop = 1;
    carkit_close();
    if (g_opt.bridge) {
        ncm_bridge_stop();
        ctrl_stop();
    }
    else
        audio_stop();
    usb_close(&g_usb);
    if (g_opt.auto_mode) {
        auto_finish();
        if (auto_code()) {
            say("nbt_carplay: over (exit %d)", auto_code());
            return auto_code();
        }
    }
    say("nbt_carplay: over");
    return r ? 1 : 0;
}
