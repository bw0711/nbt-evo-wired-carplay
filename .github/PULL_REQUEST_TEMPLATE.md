## What and why

<!-- What changes, and why. Link the issue. -->

## Head unit

- [ ] Does not change what the head unit does, or the README table "What it changes on the head unit" is updated
      (with backup and undo)
- [ ] Wireless CarPlay, the factory iPod/USB behaviour and nbt-aa behave as before when the wired path is not in use
- [ ] Fails closed on anything unknown (other firmware, unexpected bytes or answers)

## Tested

- [ ] `./scripts/test-host.sh` passes; new behaviour has a new host test
- [ ] `./scripts/build-all.sh` passes (import checks included)
- [ ] IDEmu: <!-- what you ran and saw, or "not needed because …" -->
- [ ] Car: <!-- firmware NBTevo_…, iPhone model, iOS version, what you saw; or "not yet" (keep the PR a draft) -->

New SHA-256 of changed head-unit binaries:

```
```

## Checks

- [ ] Commits are signed off (`git commit -s`)
- [ ] Nothing from `car/runs/`, no files from a head unit or firmware, no MAC/UDID/VIN/names/certificates/keys
- [ ] Docs updated (README status, component README, `car/RESULT-*.md` for a car visit)
- [ ] An AI agent helped: <!-- yes / no -->
