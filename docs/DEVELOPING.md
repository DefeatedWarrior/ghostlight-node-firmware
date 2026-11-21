# Developing the firmware

This guide covers building for each board, switching features on and off,
how the code is laid out, and how to add a board or a feature. Every path
and command here is relative to the top of this repository. The Ghostlight
Node hardware (KiCad, BOM, docs) is in a separate repository,
[ghostlight-node](https://github.com/DefeatedWarrior/ghostlight-node).

## Building

You need ESP-IDF **v5.2.6**. Load it in your shell first:

```bash
. tools/idf_env.sh          # expects ESP-IDF in ~/esp/esp-idf (IDF_PATH_OVERRIDE to change)
```

Then use `tools/build.sh`, which wraps `idf.py` once per board:

```bash
tools/build.sh list                              # boards you can build
tools/build.sh esp-node-2rdm                     # build one board
tools/build.sh esp-node-2rdm -p PORT flash monitor
tools/build.sh esp-node-2rdm menuconfig          # change features for that board
tools/build.sh all                               # build every board
```

Each board has its own build folder, `build-<board>/`, with its own settings
in `build-<board>/sdkconfig`. That file is made the first time you build, from
`sdkconfig.defaults` (shared by every board) plus
`boards/<board>/sdkconfig.defaults`. After you edit either defaults file,
delete `build-<board>/sdkconfig` so the change takes effect.

Plain `idf.py build` builds the Ghostlight Node rev1 into `build/`, with
its settings in `./sdkconfig` (`CMakeLists.txt` picks the rev1 defaults when
no `SDKCONFIG_DEFAULTS` is given). An `sdkconfig` left over from an older
firmware keeps its old settings, so delete it first. For any other board, or
to always know which board you are building, use `tools/build.sh`.

`tools/check_compile.sh` is a quick compile-only check that works without
ESP-IDF installed. It uses the Arduino-ESP32 toolchain. It doesn't link, so it
can't replace a real build. `BOARD=<board>` checks another board's pin
header (for example `BOARD=esp32s3-devkitc tools/check_compile.sh` for the
Wi-Fi-only board), and `FEATURES=none` checks with every feature switch off.

## Boards

| Board (`tools/build.sh …`) | Chip | Flash / PSRAM | Partition table | Pin header | Status |
|---|---|---|---|---|---|
| `esp-node-2rdm` | ESP32-S3 | 16 MB / 8 MB octal | `partitions_16mb.csv` | `boards/esp_node_2rdm.h` | prototype, in use |
| `ghostlight-rev1` | ESP32-S3 (WROOM-1-N8) | 8 MB / none | `partitions_8mb.csv` | `boards/ghostlight_rev1.h` | PCB in [ghostlight-node](https://github.com/DefeatedWarrior/ghostlight-node), not run yet |
| `esp32-devkitc` | ESP32 (WROOM-32) | 4 MB / none | `partitions_4mb.csv` | `boards/esp32_devkitc.h` | builds, **never run on hardware** |
| `esp32s3-devkitc` | ESP32-S3 (DevKitC-1) | 8 MB / none | `partitions_8mb.csv` | `boards/esp32s3_devkitc.h` | Wi-Fi only (no W5500), builds, **never run on hardware** |

Pin headers live in `components/board/include/`. Settings files live in
`boards/<board>/sdkconfig.defaults` at the top of the repository.

**Partition tables differ between boards.** The app slots and LittleFS sit at
different addresses on each board. If you flash a device with a build made for
another board, the stored settings and scenes are lost. The 16 MB table is
the layout the prototype already runs, so don't change it.

The ESP32 DevKitC wiring is in the comment at the top of
`boards/esp32_devkitc.h`. On the original ESP32 the console stays on UART0
(the USB-serial chip) and the DMX ports use UART1 and UART2.

## Feature switches

Optional features are build-time switches under
**menuconfig → ESP-NODE → Features** (`main/Kconfig.projbuild`). All of them
are on by default.

| Switch | When it is off |
|---|---|
| `CONFIG_NODE_ETHERNET` | Wi-Fi only. The W5500 driver is not started. |
| `CONFIG_NODE_SACN` | Art-Net only. Ports set to sACN fall back to Art-Net. |
| `CONFIG_NODE_RDM` | No RDM controller or responder modes and no ArtTod/ArtRdm. RDM ports become plain outputs or inputs. |
| `CONFIG_NODE_SCENES` | No scenes, no power-on scene, no failsafe scene. A "scene" failsafe falls back to blackout. |
| `CONFIG_NODE_DMX_TOOLS` | No test patterns and no loopback self-test. |
| `CONFIG_NODE_WEB_LOG` | The web UI can't show the log. `CONFIG_NODE_WEB_LOG_KB` sets the log size when the switch is on. |
| `CONFIG_NODE_MDNS` | No `http://<name>.local/`. |
| `CONFIG_NODE_TESTER` | No DMX tester mode (needs `CONFIG_NODE_DMX_TOOLS`). |
| `CONFIG_NODE_SERIAL_SETUP` | No USB setup: the browser flasher can install the firmware but not set it up, and the console port only carries the log. |

To switch features for one board, run `tools/build.sh <board> menuconfig`.
That changes only `build-<board>/sdkconfig`. To make the change the default
for a board, add the line to `boards/<board>/sdkconfig.defaults`, for example
`# CONFIG_NODE_RDM is not set`. `tools/ci/minimal.defaults` switches every
feature off:

```bash
BUILD_DIR=build-minimal EXTRA_DEFAULTS=tools/ci/minimal.defaults tools/build.sh esp32-devkitc
```

Settings saved by a firmware that had a feature can still be loaded by one
that doesn't. `config_sanitize()` in `components/config_manager` maps them to
the nearest thing that is built in, for example RDM controller becomes
output and sACN becomes Art-Net.

### How a switch works (rules for the code)

- **Every component is always compiled and linked.** `REQUIRES` lists and
  `SRCS` never depend on a switch, and no header gets stubbed out. Instead,
  the *call sites* are wrapped in `#if CONFIG_NODE_<X>`. Code that nothing
  calls is dropped by the linker (`--gc-sections`), so a switched-off feature
  costs no flash. The one exception is code the web server still calls.
- Use `#if CONFIG_NODE_<X>`, not `#ifdef`. A switched-off bool is undefined
  in `sdkconfig.h`, so `#if` reads it as 0.
- A `static` function that is only called from guarded code needs the same
  guard. Otherwise you get an unused-function warning, and warnings fail the
  build.
- If a stored setting means nothing without the feature, map it in
  `config_sanitize()` / `sanitize_port()`.
- CI builds every board, plus a minimal build per chip, so both sides of
  every `#if` keep compiling.

## Code layout

```
main/                 the application: wires the components together
  main.c              start-up order, main loop, mDNS, statistics, OTA mark
  app.h               shared state (g_app), event bits, prototypes
  app_routing.c       network <-> DMX: Art-Net/sACN receive, DMX input send, sync
  app_artnet.c        ArtPollReply status, ArtAddress, ArtIpProg
  app_config.c        applies the settings to the running ports
  app_scenes.c        power-on scenes, ArtAddress failsafe record
  app_indicator.c     status LED, port LEDs, factory-reset button
  Kconfig.projbuild   board choice and feature switches
components/
  board               board.h: picks the pin header for the selected board
  config_manager      settings (JSON in LittleFS), defaults, sanitising
  dmx_handler         DMX/RDM ports on top of esp_dmx (one task per port)
  esp_dmx             DMX/RDM UART driver (vendored, with local fixes)
  merge_engine        per-port HTP/LTP/priority merge of network sources
  artnet_receiver     Art-Net 4 node (ArtPoll/Reply, ArtDmx, ArtAddress, ...)
  artnet_transmitter  ArtDmx out for DMX input ports
  sacn_receiver       E1.31 receive, multicast groups, sync
  sacn_transmitter    E1.31 out for DMX input ports
  rdm_bridge          RDM over Art-Net (ArtTod*, ArtRdm)
  scene_manager       scenes stored in LittleFS, fades
  dmx_tools           test patterns, loopback self-test
  dmx_tester          tester mode: levels to send, receive statistics, flicker watch
  serial_setup        settings over the console UART / native USB (browser flasher)
  network_manager     Ethernet / Wi-Fi / AP with automatic fallback
  web_server          HTTP API and the web UI (web_ui/, built into web_files.h);
                      web_tools.c = diagnostics and tools, web_tester.c = /api/tester
  led_manager         WS2812 status LED
  diag                log ring buffer, reset reason, core dump
  storage_manager     LittleFS mount
boards/<board>/       per-board sdkconfig.defaults
tools/                build.sh, idf_env.sh, check_compile.sh, gen_web_files.py,
                      ui_mock.py (the web UI against a fake node, no hardware),
                      ci/release.py (release images, flasher manifests)
tests/host/           unit tests and fuzzers that run on a PC (see "Tests")
```

Callbacks from the network and DMX tasks only read the routing table
(`g_app.route`) and set event bits. Anything slow or stateful runs on the
main task (`app_main`'s loop in `main.c`).

After you edit anything in `components/web_server/web_ui/`, regenerate the
embedded copy with `python3 tools/gen_web_files.py`.

### Working on the web UI without hardware

`python3 tools/ui_mock.py` serves `web_ui/` at http://localhost:8090/ and
fakes every API endpoint and the level WebSocket, with simulated DMX (a
console on the inputs, one flickering channel, a signal loss every 45 s), RDM
fixtures and a log. `--tester` starts in tester mode, `--no-ethernet`,
`--ports N` and `--off rdm,scenes,...` mimic other builds, and `--setup`
shows the first-run setup. It needs nothing beyond Python 3.

## USB setup

With `CONFIG_NODE_SERIAL_SETUP` (on by default) the node takes commands on
the port it is flashed through: the console UART (boards with a USB-serial
chip) and, on the ESP32-S3, the chip's own USB port. The browser flasher uses
it to set up a node straight after installing it; it also gets a node with a
lost address or admin password back. One command per line, JSON after
`#GL `, one reply line per command after `#GL> `:

```
#GL {"id":1,"cmd":"info"}
#GL> {"ok":true,"firmware_version":"1.0.0","board":"Ghostlight Node rev1",...,"id":1}
#GL {"id":2,"cmd":"set","config":{"node_info":{"short_name":"Stage Left"}}}
#GL> {"ok":true,"restart_needed":false,"id":2}
```

Commands: `info`, `get` (the settings without passwords), `set` (the same
document `POST /api/config` takes, checked by the same code), `password`
(admin password), `scan`, `connect` (a saved Wi-Fi network) and `restart`.
The details are in `components/serial_setup/include/serial_setup.h`. You can
try it from any serial monitor at 115200 baud. Everything else on the port is
the normal log, which keeps working.

## DMX tester mode

With `CONFIG_NODE_TESTER` built in, the web UI (Tools, or `POST
/api/tester/mode`) switches the node into a DMX tester. The choice is stored in
the settings, so it survives restarts, and switching restarts the node. In
tester mode `main.c` does not start the Art-Net and sACN receivers and
transmitters, power-on scenes or the RDM bridge: the network is up only for
the web UI. Each port gets one job (`config_t.tester.ports[]`): off, send,
receive or RDM controller, with its own output timing.
`config_tester_port_settings()` turns that into the port settings the DMX
handler runs with. `dmx_tester` holds the levels sent ports output and the
statistics of receive ports (frame intervals and jitter, histogram, signal
losses, per-channel min / max / change counts and the "changing now" mask);
it allocates its buffers only in tester mode. The API is in `web_tester.c`.

## Adding a board

1. Copy a pin header in `components/board/include/boards/` (for example
   `esp32_devkitc.h`) to `components/board/include/boards/<your_board>.h`.
   Fill in the required
   `BOARD_*` names listed at the top of `components/board/include/board.h`.
   Anything optional you leave out defaults to "not fitted" (-1).
2. Add a `config BOARD_<YOUR_BOARD>` entry to the `NODE_BOARD` choice in
   `main/Kconfig.projbuild`. Give it a `depends on IDF_TARGET_<CHIP>`, and add
   `select BOARD_HAS_W5500` if it has one.
3. Add an `#elif CONFIG_BOARD_<YOUR_BOARD>` line for it in `board.h`.
4. Create `boards/<your-board>/sdkconfig.defaults` with at least
   `CONFIG_IDF_TARGET`, `CONFIG_BOARD_<YOUR_BOARD>=y`, the flash size and
   mode, and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`. Add the PSRAM settings
   if the board has PSRAM. Reuse a `partitions_*mb.csv` that matches the flash
   size, or add a new one.
5. Run `tools/build.sh <your-board>`, then add the board to the matrix in
   `.github/workflows/build.yml`. The release workflow and the web flasher
   find the new folder on their own. The flasher lists the board with the
   first comment line of its `sdkconfig.defaults`, so make that line say
   what the board is.

Pin notes:

- ESP32-S3: avoid GPIO26–32 (flash), and GPIO33–37 on octal-PSRAM modules.
- Original ESP32: GPIO6–11 are flash, GPIO34–39 are input-only,
  GPIO2/12/15 are boot straps, and WROVER modules use GPIO16/17 for PSRAM.

Single-core chips work. `BOARD_CORE_DMX` puts the DMX tasks on core 0 there.

## Adding a feature switch

1. Add `config NODE_<X>` (bool, `default y`, with a help text) to the
   Features menu in `main/Kconfig.projbuild`.
2. Wrap the call sites in `#if CONFIG_NODE_<X>`. Leave the component itself
   alone: it stays in `REQUIRES` and its sources stay compiled.
3. Map stored settings that need the feature in `config_sanitize()`.
4. Add `# CONFIG_NODE_<X> is not set` to `tools/ci/minimal.defaults`.
5. Build a board with the feature on, and a minimal build with it off.

## Tests

`tests/host/` has unit tests and fuzzers that run on your computer, with no
ESP-IDF and no hardware. They cover the code that handles network input
and the core logic:

| Suite | Covers |
|---|---|
| `test_artnet.c` | Art-Net parsing: ArtDmx, ArtPoll/ArtPollReply, ArtAddress, ArtSync, ArtTod*, ArtRdm, ArtIpProg, bad lengths and IDs |
| `test_sacn.c` | E1.31 parsing: data, per-address priority, sync, priorities, preview / stream-terminated, bad headers |
| `test_merge.c` | merge engine: HTP, LTP, last source, backup, disable, priorities, timeouts, failsafe, sync |
| `test_config.c` | `config_to_json` / `config_from_json` round trip, loading the file, `config_sanitize` |

You need `make` and a C compiler (clang or gcc):

```bash
make -C tests/host test       # unit tests and the fuzz seeds, under ASan + UBSan
```

The fuzzers use libFuzzer, so they need LLVM clang. Apple's clang doesn't
include libFuzzer. On macOS, `brew install llvm`, and the Makefile finds it.

```bash
make -C tests/host fuzz-run FUZZ_TIME=60    # each fuzzer for 60 s
```

There are three fuzzers: `fuzz_artnet`, `fuzz_sacn` (one UDP packet each)
and `fuzz_config`, which feeds the same input to `config_from_json()` and to
`config_load()`. Then it checks that the result is already clean and that it
survives a JSON round trip unchanged. New corpus entries go to
`tests/host/build/corpus/`, and crashing inputs to `tests/host/build/crash-*`.
To reproduce a crash, run the fuzzer on that one file, for example
`tests/host/build/fuzz_sacn tests/host/build/crash-sacn-…`.

How it works:

- `stubs/` replaces the ESP-IDF, FreeRTOS, lwIP and mbedTLS headers that
  the tested components include. `fakes.c` implements them: a fake clock
  (`host_now_us`), a `sendto()` that records packets, and an in-memory
  `storage_manager`. The components are compiled unchanged. Don't add
  `#ifdef TEST` or stubs inside `components/` or `main/`.
- The Art-Net and sACN tests `#include` the component's `.c` file, so they
  can call its static `process_packet()` the way the receive task does.
  Packets go into a buffer the size of the firmware's receive buffer. The
  bytes after the packet are poisoned, so ASan reports any read past it.
- `test.h` is the assert framework (`CHECK`, `CHECK_EQ`, `RUN`). `packets.h`
  builds valid packets for the tests and for the seed corpus.
- Seeds: `fuzz/seeds/artnet` and `fuzz/seeds/sacn` are made by
  `fuzz/make_seeds.c` (run `make -C tests/host seeds` after changing it).
  `fuzz/seeds/config` holds JSON files written by hand.
- The host build is the ESP-NODE-2RDM board (2 ports) with every feature
  on (`stubs/sdkconfig.h`).

When a fuzzer finds a bug, fix it in the component and add the input as a
unit test, so it can't come back.

## Continuous integration

`.github/workflows/tests.yml` runs `make test` and runs each fuzzer for 60 s
on Ubuntu with clang. If a fuzzer crashes, the crashing inputs are uploaded
as an artifact.

`.github/workflows/build.yml` builds every
board, plus `esp-node-2rdm-minimal` and `esp32-devkitc-minimal` (every
feature off), in the `espressif/idf:v5.2.6` container. It runs `tools/build.sh` exactly as
above. ESP-IDF's own size check fails a build when the app no longer fits its
partition. The binaries, `flasher_args.json` and the `sdkconfig` of each build
are uploaded as artifacts.

## Making a release

`.github/workflows/release.yml` builds and publishes a release when you push
a tag that starts with `v`.

1. Set the version in `CMakeLists.txt`, for example
   `set(PROJECT_VER "1.1.0")`, and commit it. The web UI and ArtPollReply
   show this version, so the workflow refuses a tag that doesn't match it.
2. Tag that commit and push the tag:

   ```bash
   git tag -a v1.1.0 -m "Firmware 1.1.0"
   git push origin v1.1.0
   ```

   A tag with a `-` in it, such as `v1.1.0-rc1`, becomes a pre-release
   (with `PROJECT_VER "1.1.0-rc1"`).

For every folder under `boards/`, the workflow builds the board with
`tools/build.sh <board>` in ESP-IDF v5.2.6 and makes two files:

| File | What it is |
|---|---|
| `ghostlight-node-<board>-<version>-full.bin` | bootloader, partition table, OTA data and app merged into one image, to flash at 0x0. Used for a first install. |
| `ghostlight-node-<board>-<version>-ota.bin` | the app alone. Upload it in the web UI under Firmware Update. Settings are kept. |

It then:

- creates a GitHub Release for the tag with all these files and a
  `SHA256SUMS.txt`;
- publishes the web flasher to GitHub Pages
  (`https://<owner>.github.io/ghostlight-node-firmware/`), with the full
  images of this release. The flasher always serves the most recent tag.

The flasher page is `docs/flasher/index.html`. It uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/), pinned to one
version in the page's `<script>` tag. The workflow writes one ESP Web Tools
manifest per board (`manifests/<board>.json`, chip family taken from
`CONFIG_IDF_TARGET`) and `boards.json`, the list the page shows, with
`tools/ci/release.py manifests`. The copies in `docs/flasher/` say
`unreleased`. Regenerate them after adding a board:

```bash
python3 tools/ci/release.py manifests --out docs/flasher --version unreleased
```

To try the workflow without releasing anything, run it from the Actions tab
(**Release → Run workflow**). It builds every board and keeps the release
files and the flasher site as workflow artifacts, named `release-<board>`
and `github-pages`. It creates no release and deploys nothing.

GitHub Pages must be switched on once: **Settings → Pages → Build and
deployment → Source: GitHub Actions**.
