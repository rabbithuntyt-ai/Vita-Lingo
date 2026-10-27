# Vita Lab

Remote control and testing for PS Vita homebrew, from a PC on the same Wi-Fi:
see the Vita's screen, tap it and press its buttons, install and uninstall
apps, swap a test build's program in seconds, measure frame rate and CPU load
per core, set clocks, and collect crash dumps. Made for testing Lingo, and
general enough for any homebrew (an emulator you are optimising, say).

Two parts:

- **VitaLab.vpk** (the Vita): an app that installs two background plugins
  into taiHEN, shows the Vita's address, and removes them again.
- **vitalab.exe** (the PC; also built for macOS and Linux): a control panel
  in your browser, and a command line for scripted tests.

## Set up (once)

1. Install **VitaLab.vpk** with VitaShell, open **Vita Lab**, choose
   **Install plugins**, then **Reboot now**.
2. After the reboot, open Vita Lab again: *Running now* should show a version,
   and it shows the Vita's IP address.
3. In **Settings > Power Save Settings**, turn off **Enter Standby Mode
   Automatically**, and keep the Vita charging. The plugin also stops the Vita
   sleeping while it runs.
4. On the PC, run **vitalab.exe**. It finds the Vita by itself and opens the
   control panel in your browser. If Windows asks, allow it on **private
   networks**: it listens for the Vita's reply to "who's there?" (UDP 1340).
   If it can't find the Vita, run `vitalab --host 192.168.x.x` with the
   address Vita Lab shows.

## Control panel

The Vita's screen, live. Click to tap, drag to swipe; keyboard: arrows for the
d-pad, Z ✕, X ○, A □, S △, Q L, W R, Enter Start, Shift Select, Home PS.
Beside it: frame rate, CPU load per core and clocks; clock presets; a
benchmark; install a VPK, launch, quit or uninstall by title ID; crash dumps;
reboot; and a box for raw commands.

## Command line

```
vitalab find                          Vitas on this network
vitalab shot screen.png --step 1      screenshot (step 2 = half size, quicker)
vitalab stats                         fps, CPU per core, clocks, app on screen
vitalab bench 60 --csv run.csv        frame rate and CPU load over a minute
vitalab press cross | press l+r 500   buttons (held for ms)
vitalab tap 480 272 | swipe 100 300 800 300
vitalab stick left 128 0 1000
vitalab install build.vpk | uninstall TITLEID | launch TITLEID | quit all | apps
vitalab push-eboot TITLEID eboot.bin  replace just the program, restart it
vitalab clock 444 222 222 166 | clock default
vitalab crashes --get dumps/          crash dumps (open with vita-parse-core)
vitalab ls ux0:data | get ux0:data/log.txt log.txt | put f ux0:data/f | rm ux0:data/f
vitalab cmd "press cross; wait 100ms; release cross"
vitalab script test.txt               one command per line; "sleep 2s" waits
```

## What runs on the Vita

A fork of [vitacompanion](https://github.com/devnoname120/vitacompanion) (MIT),
loaded by taiHEN into SceShell and the kernel:

| Port | What |
| --- | --- |
| 1337 | FTP (the whole memory card) |
| 1338 | commands: launch, quit, press/release, promote, reboot, nosleep… plus Vita Lab's `delete`, `apps`, `stats`, `clock` |
| 1339 | screenshots: whatever is on screen, as RGB565 |
| 1340 | UDP discovery |

Vita Lab's additions: uninstalling (the system package promoter), reading the
displayed framebuffer of whichever app owns it, counting framebuffer flips for
the frame rate, per-core CPU load from the kernel's idle counters, and setting
CPU, bus, GPU and crossbar clocks (games may set their own again).

## Safety

- **Anyone on your network can control the Vita while the plugins run**
  (no password, as with vitacompanion and VitaShell's FTP). Use them on a
  network you trust, and remove them in Vita Lab when you're done.
- If the Vita ever fails to boot after installing, **hold L while it starts**:
  taiHEN skips all plugins. Then open Vita Lab and choose **Remove plugins**.
  The first change also saved your taiHEN config as
  `config.txt.vitalab-backup` next to it.
- The control panel listens on this PC only (127.0.0.1) and refuses requests
  from other web pages.

## Licences

The Vita plugins are a modified vitacompanion (MIT, © devnoname120 and
contributors), with libftpvita (MIT, © Sergi Granell), a VitaDeploy package
template (MIT, © skgleba) and a public-domain SHA-1; see `lab/vita/LICENSE`
and `lab/vita/THIRD_PARTY_NOTICES.md`.
