# Car visit: wired CarPlay "the factory way" - a 4-step plan

Goal: factory CarPlay over the cable that the HMI treats as its own: an "Apple CarPlay" entry in the iDrive menu, with minimize and return.
Why it works this way: `../docs/research/factory-like-device-list-re-2026-11-26.md`. In short: this head unit's factory CarPlay is built around
Bluetooth. A session reaches the HMI only if, at that moment, Connectivity is connecting a phone it knows over Bluetooth.
On 26.11 Bluetooth was off, so there was no entry. The main variant (step 1) has never been tried yet.

Time: 25-30 minutes. Each step is one command in `car/`, logs go to `car/runs/<date>/`, and the result is in `summary.txt`.
Nothing is written to flash. Everything that changes (firewall rules, the in-memory patch) lives in RAM and is removed at the end of the step
or by rebooting the head unit.

## 0. Preparation
1. The head unit is on, the laptop is on the ENET cable, and in the terminal run: `ssh root@169.254.199.119 true` (you type the password yourself).
2. The iPhone is **unlocked**. If it asks "Trust?", tap "Trust": after every head unit reboot the pairing starts over.
3. Read-only, ~1 min: `./car-chip-test.sh nbt-aa-pull`, `./car-chip-test.sh conn-check`, `./car-chip-test.sh pf-check`.
   - `conn-check` must show `c1: original`, `c2: original`;
   - `pf-check` tells whether wireless CarPlay can be hidden on this head unit (needed in step 4).

## 1. Main variant (a0): Bluetooth on, Wi-Fi off
- iPhone: Bluetooth **on and connected to the car**, Wi-Fi **off in Settings** (not in Control Center).
- Plug the cable into USB, then run `WAIT_HU=1 WAIT_HU_MAX=20 ./car-chip-test.sh bridge-start`.
- At the start the script shows the head unit's `before:` lines and waits up to 20 s while the head unit looks for the phone (repeated
  `not found in available device list`). "the head unit is looking for the iPhone" is the state we are counting on. "did not look … starting anyway" means the head unit is not waiting for the phone (probably
  because Wi-Fi is off). In that case the entry will most likely not appear, and that is a direct reason for step 3 (c1).
- Watch the screen: does CarPlay appear; is there an "Apple CarPlay" entry in the iDrive menu; minimize, then return through the entry.
- If the entry is there, the **goal is reached**. Go to step 4.

## 2. If there is no entry (variant a): the bridge announces a "controller" in the cable
- Same iPhone state. Run `WAIT_HU=1 WAIT_HU_MAX=20 ./car-chip-test.sh bridge-ctrl`. The script takes the iPhone's Bluetooth address from the head unit's log
  (or set `PHONE_BT=XX:XX:XX:XX:XX:XX`).
- In the log, wait for `Connection successful with the device…`, then CarPlay and the menu entry.
- If the entry is there, go to step 4 with `bridge-ctrl`.

## 3. If there is still no entry: the c1 patch (with your "yes" at the car)
- Run `./car-chip-test.sh conn-apply c1`, then `./car-chip-test.sh bridge-start` again (without waiting: c1 is needed exactly
  when the head unit is not waiting for the phone).
- c1 makes a session that the iPhone started "active", even if the head unit was not waiting for a phone at that moment.
- After the step: `./car-chip-test.sh conn-revert c1` (or reboot the head unit), then `conn-check`.
- If there is no entry even with c1, we stop here. I will analyze the logs at home.

## 4. Wi-Fi on the iPhone turned on (as in real life)
Take the variant that worked in steps 1-3 (`bridge-start` or `bridge-ctrl`, with or without c1). On the iPhone, Wi-Fi is **on**.
If only c1 worked and it has already been rolled back, run `./car-chip-test.sh conn-apply c1` again before step 4.
`HIDE_WIFI` is a temporary firewall rule (RAM) that hides wireless CarPlay from the head unit while the cable is working. It is removed
at the end of the run. It works only if `pf-check` in step 0 said "possible"; otherwise the script does not set it and touches nothing.

- **4a. "I got in the car, the cable is already plugged in."** Turn Bluetooth off on the iPhone (wireless CarPlay will end) and plug in the
  cable. I run `HIDE_WIFI=mdns WAIT_HU=1 ./car-chip-test.sh bridge-start` (or `bridge-ctrl`). When I say
  "turn Bluetooth on", turn it on on the iPhone. The script itself waits until the head unit starts looking for the phone (up to 150 s) and only then
  starts the bridge: the session must arrive exactly at that moment. We wait for: CarPlay over the cable and the menu entry.
- **4b. "I plugged in the cable while wireless is already running."** Wireless CarPlay is on the screen, and you plug in the cable. I run
  `HIDE_WIFI=all WAIT_HU=1 ./car-chip-test.sh bridge-start` (or `bridge-ctrl`). The rule breaks the wireless session, the script waits
  until the head unit starts looking for the phone again, and then starts the bridge. Time how many seconds the screen has no CarPlay. If the head unit does not start
  looking for the phone within 150 s, the script starts the bridge anyway and says so. In that case turn Bluetooth off and on again on the iPhone.
- **4c. For comparison, without HIDE_WIFI** (if time is left): the same as 4b, without `HIDE_WIFI=`. On 26.11, in that
  state the iPhone ignored the cable.

## End
- `./car-chip-test.sh cleanup` (it also removes the HIDE_WIFI rule, if it is somehow still there). If c1 was applied and not rolled back,
  reboot the head unit or run `conn-revert c1`.
- What to note by eye: was there an "Apple CarPlay" entry, did it bring the screen back, sound, the iDrive controller, Siri, and how many seconds
  the switch in 4b took.

## What counts as success
- In the log: `CarPlay Session created`, `Session is UP and RUNNING`; in variant a also `Connection successful with the device…`.
- On the screen: CarPlay; an "Apple CarPlay" entry in the iDrive menu; after "minimize", the entry brings the CarPlay screen back.
- In step 4: the same with Wi-Fi on on the iPhone.

## How to read the result (what we expect and what it means)
Three independent questions. The logs tell them apart, so the visit gives an answer even if there is no entry.
- **Q1. Does the head unit wait for the phone when Wi-Fi is off on the iPhone?** Sign: in the `before:` lines / while waiting, repeated
  `not found in available device list`. Estimate: 50/50 (unknown whether the iPhone announces wireless CarPlay without Wi-Fi).
  No -> a0 will not give the entry; `bridge-ctrl` helps (the head unit starts connecting to our controller by itself, if it knows the phone
  over Bluetooth), or c1.
- **Q2. Will the iPhone accept the command to start a session over the cable when it is connected to the car over Bluetooth?** Sign: in the head unit's log
  `CarPlay Session created`. Estimate: probably yes (~2 in 3): on 26.11 without Bluetooth it accepted, and with wireless working
  it ignored. No -> this is a wall for every "factory-like" variant (c1 will not help), and I analyze it at home.
- **Q3. A session is "active" -> menu entry?** Sign: an "Apple CarPlay" entry in iDrive. Estimate: if Q1 and Q2 are "yes", probably yes
  (~3 in 4), because this is the same path as wireless. A session exists but no entry -> the model of the analysis is wrong somewhere. I analyze it at home with the car's HMI
  from `nbt-aa-pull`.
- Steps 1-3 together: the menu entry is about 50/50.
- Step 4 (Wi-Fi on): 4c is almost certainly as on 26.11 (the iPhone ignores the cable); 4a has a medium chance; 4b (switching from
  working wireless) is below medium, and the switch will take tens of seconds.
- If the entry is there: sound, the iDrive controller, Siri, and the instrument cluster should work as in wireless. It is the same factory receiver.

## If something goes wrong
- If the bridge program crashes, it exits by itself without a dump, so the head unit does not reboot (on 26.11 a dump caused a reboot).
- The firewall rules and the c1 patch live only in RAM: rebooting the head unit (ignition) returns everything to factory.
- The c1 patch is written only if the Connectivity code matched what was expected byte for byte; otherwise the utility writes nothing.
