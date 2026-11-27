# Instructions for AI agents

Before any work in this repository, read:
1. [README.md → Installing: for an agent](README.md#installing-for-an-agent): rules, stage kinds (read-only / RAM /
   FLASH) and the sequence with the output each step must show.
2. [CONTRIBUTING.md](CONTRIBUTING.md): head-unit facts, design and code rules, testing, and what must never be
   committed.

The short version: FLASH stages need the car owner's explicit yes for that stage at that moment; never handle the
head unit's password; read-only checks first; stop on anything unexpected; `./scripts/test-host.sh` before any
change reaches a car; nothing from `car/runs/` or from a head unit goes into git.
