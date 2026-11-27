# Documents

How wired CarPlay on the NBT Evo was found, built and checked. Read them in this order if you want the whole story;
the [README](../README.md) has the short version.

## Research notes ([`research/`](research/))

| Date | Note | What it answers |
|---|---|---|
| 2026-11-26 | [feasibility](research/feasibility-2026-11-26.md) | Is wired CarPlay possible on the Evo at all? Overview and plan |
| 2026-11-26 | [prior-art](research/prior-art-2026-11-26.md) | Who tried what before: the open `nbt_carplay` for the old NBT, other attempts |
| 2026-11-26 | [medialaunch-re](research/medialaunch-re-2026-11-26.md) | `vdev-medialaunch` and the USB host/device role switch |
| 2026-11-26 | [connectivity-re](research/connectivity-re-2026-11-26.md) | How `Connectivity` finds and accepts a CarPlay phone |
| 2026-11-26 | [evo-aa-video-path](research/evo-aa-video-path-2026-11-26.md) | How projected video reaches the screen on the Evo |
| 2026-11-26 | [factory-like-design](research/factory-like-design-2026-11-26.md) | Design of the "factory-like" path through the head unit's own receiver |
| 2026-11-26 | [factory-like-device-list-re](research/factory-like-device-list-re-2026-11-26.md) | Where the iDrive "Apple CarPlay" entry comes from (DipoState, Bluetooth address); patches c1/c2 |
| 2026-11-27 | [audio-pcm-re](research/audio-pcm-re-2026-11-27.md) | Why the sound stayed on the phone over the cable, and the PCM lines that fix it |
| 2026-11-27 | [iap2-factory-re](research/iap2-factory-re-2026-11-27.md) | How "now playing" reaches the cluster, and why the adapter and patch `w1` |

## Car visits ([`../car/`](../car/))

| Visit | Result | Checklist used |
|---|---|---|
| 2026-11-26 | [Chip, first session, bridge to the factory receiver](../car/RESULT-2026-11-26.md) | [CHECKLIST](../car/CHECKLIST.md) |
| 2026-11-27 morning | [Factory CarPlay with the iDrive entry, no patches](../car/RESULT-2026-11-27.md) | [CHECKLIST-factory-entry](../car/CHECKLIST-factory-entry.md) |
| 2026-11-27 afternoon | [PCM sound, Siri with the car's mic, track on the HUD](../car/RESULT-2026-11-27-pm.md) | [CHECKLIST-next-visit](../car/CHECKLIST-next-visit.md) |
| 2026-11-27 evening | [Autostart installed, every scenario without the laptop](../car/RESULT-2026-11-27-evening.md) | [CHECKLIST-autostart](../car/CHECKLIST-autostart.md) |

## Components

[`nbt-carplay-evo/`](../nbt-carplay-evo/README.md) · [`iap2-adapter/`](../iap2-adapter/README.md) ·
[`conn-patch/`](../conn-patch/README.md) · [`car/autostart/`](../car/autostart/README.md) ·
[`probe-evo/`](../probe-evo/README.md) · [`emu-usb/`](../emu-usb/README.md)
