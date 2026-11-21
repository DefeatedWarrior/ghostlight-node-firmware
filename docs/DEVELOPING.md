# Developing the firmware

This guide covers building for each board, switching features on and off,
how the code is laid out, and how to add a board or a feature. Every path
and command here is relative to the `firmware/` folder of the
ghostlight-node repository.

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
can't replace a real build.

## Boards

| Board (`tools/build.sh …`) | Chip | Flash / PSRAM | Partition table | Pin header | Status |
|---|---|---|---|---|---|
| `esp-node-2rdm` | ESP32-S3 | 16 MB / 8 MB octal | `partitions_16mb.csv` | `boards/esp_node_2rdm.h` | prototype, in use |
| `ghostlight-rev1` | ESP32-S3 (WROOM-1-N8) | 8 MB / none | `partitions_8mb.csv` | `boards/ghostlight_rev1.h` | PCB in this repository (`../hardware/`), not run yet |
| `esp32-devkitc` | ESP32 (WROOM-32) | 4 MB / none | `partitions_4mb.csv` | `boards/esp32_devkitc.h` | builds, **never run on hardware** |

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
  network_manager     Ethernet / Wi-Fi / AP with automatic fallback
  web_server          HTTP API and the web UI (web_ui/, built into web_files.h)
  led_manager         WS2812 status LED
  diag                log ring buffer, reset reason, core dump
  storage_manager     LittleFS mount
boards/<board>/       per-board sdkconfig.defaults
tools/                build.sh, idf_env.sh, check_compile.sh, gen_web_files.py
```

Callbacks from the network and DMX tasks only read the routing table
(`g_app.route`) and set event bits. Anything slow or stateful runs on the
main task (`app_main`'s loop in `main.c`).

After you edit anything in `components/web_server/web_ui/`, regenerate the
embedded copy with `python3 tools/gen_web_files.py`.

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
   `.github/workflows/firmware.yml` (at the top of the repository).

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

## Continuous integration

`.github/workflows/firmware.yml` (at the top of the repository) builds every
board, plus `esp-node-2rdm-minimal` and `esp32-devkitc-minimal` (every
feature off), in the `espressif/idf:v5.2.6` container. It runs `tools/build.sh` exactly as
above. ESP-IDF's own size check fails a build when the app no longer fits its
partition. The binaries, `flasher_args.json` and the `sdkconfig` of each build
are uploaded as artifacts.
