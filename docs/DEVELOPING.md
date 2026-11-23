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
can't replace a real build. `BOARD=<board>` checks another board with its
own chip, PSRAM and pin header (for example `BOARD=esp32-devkitc` uses the
ESP32 compiler and no PSRAM), and `FEATURES=none` checks with every feature
switch off. Any warning fails the check.

The web UI's build date is the time the app was linked:
`tools/stamp_build_date.py` writes it into the app description after each
link (ESP-IDF's own `__DATE__` only changes on a clean build).
`SOURCE_DATE_EPOCH` sets it for reproducible builds.

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
                      (and of another port's DMX input, see "DMX splitter / merger")
  port_rules          per-port channel rules (patch, limit, park) on what goes out
  source_monitor      every Art-Net / sACN sender heard, for the signal check,
                      and the other Art-Net nodes (their ArtPollReply)
  artnet_receiver     Art-Net 4 node (ArtPoll/Reply, ArtDmx, ArtAddress, ...)
  artnet_transmitter  ArtDmx out for DMX input ports
  sacn_receiver       E1.31 receive, multicast groups, sync, universe discovery
  sacn_transmitter    E1.31 out for DMX input ports
  rdm_bridge          RDM over Art-Net (ArtTod*, ArtRdm)
  scene_manager       scenes stored in LittleFS, fades
  dmx_tools           test patterns, loopback self-test
  dmx_tester          tester mode: levels to send, receive statistics, flicker watch
  serial_setup        settings over the console UART / native USB (browser flasher)
  network_manager     Ethernet / Wi-Fi / AP with automatic fallback;
                      captive_dns.c answers DNS for devices on the node's own AP
  web_server          HTTP API and the web UI (web_ui/, built into web_files.h);
                      web_tools.c = diagnostics and tools, web_tester.c = /api/tester,
                      web_signal.c = /api/network/sources, /api/network/nodes
                      and the captive redirect, fw_tag.c = board check on uploads
  led_manager         WS2812 status LED
  diag                log ring buffer, reset reason, core dump, memory / stacks
  storage_manager     LittleFS mount, whole-file writes (temp file + rename)
boards/<board>/       per-board sdkconfig.defaults
tools/                build.sh, idf_env.sh, check_compile.sh, gen_web_files.py,
                      ui_mock.py (the web UI against a fake node, no hardware),
                      ci/release.py (release images, flasher.json)
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
`--ports N` and `--off rdm,scenes,...` mimic other builds, `--setup`
shows the first-run setup and `--no-login` starts with sign-in turned off.
`--signal good|offbyone|nothing|filtered|lossy|multicast` picks what the
signal check hears. It needs nothing beyond Python 3.

## Channel rules

Each port's settings have a `rules` list (at most 8), applied to what an
output port sends, after the merge and before the DMX frame goes out
(`app_dmx_frame_source` in `main/app_routing.c`, through `port_rules`):

```json
"port1": {"rules": [
  {"type": "patch", "channel": 101, "count": 10, "from": 1},
  {"type": "limit", "channel": 20,  "count": 2,  "level": 128},
  {"type": "park",  "channel": 5,   "count": 1,  "level": 255}
]}
```

All patches go first and each reads the levels as they arrived, so two
patches can swap blocks. Then limits cap channels, then parks set them.
`config_port_rule_problem()` checks a rule; `POST /api/config` and USB
`set` refuse a list with a bad rule, while a settings file keeps the good
rules and drops the bad ones. A posted `rules` list replaces the port's
whole list (`[]` clears it). Rules do nothing in tester mode or while a DMX
tools test pattern runs. Scenes and the failsafe record what arrives,
before the rules.

## DMX splitter / merger

An output port can take other ports' DMX input (boards with 2 or more
ports). The web UI sets one input per port ("DMX from another port" in the
port's settings); the setting itself takes a list, so a board with more
ports could merge several.

```json
"port1": {"local_inputs": [2, 4], "local_only": false}
```

`local_inputs` lists the input ports (never the port itself; stored as a
bit mask, bit p-1 = port p); `local_only` drops the network for that port
and is cleared while the list is empty. `app_on_dmx_input()` hands every
received frame to each output port that takes it (`feed_local_outputs()` in
`main/app_routing.c`) as a merge engine source with protocol
`SOURCE_PROTOCOL_DMX_IN` (the input port number stands in for the sender's
address, so it never matches an Art-Net sender). Each input joins the
port's other sources under its merge mode, and the protocol filter never
drops it. With `local_only` the port ignores Art-Net and sACN
(`app_on_*_dmx` skip it, its sACN universe is not joined). With exactly one
input and no network it is a splitter: it triggers its output on every
input frame, so the DMX goes out right after it arrives instead of at the
port's own refresh rate (with several inputs it keeps its refresh rate).
The output still uses its own slot count, break and MAB; RDM does not pass
through. A lost input is handled like a lost network source (merge timeout,
then the failsafe). The input port keeps sending to the network as before.
An input the port no longer takes is dropped from the merge at once.
Nothing is routed in tester mode or during the loopback self-test.

## Loss behaviour and delta output

Each output port can have its own behaviour for when every source is lost,
or follow the node's (`merge.loss_behavior`, "Merging and failsafe"):

```json
"port1": {"loss_behavior": 4, "delta_output": true}
```

`loss_behavior` is -1 for the node's (the default), or 0 hold the last look,
1 blackout, 2 full, 3 the port's failsafe scene (only with scenes built in),
4 stop output. "Stop output" sends nothing at all until a source comes
back, so the fixtures do what they do on a DMX loss (most hold, some fade
out): the frame source (`app_dmx_frame_source()`) returns false while
`merge_engine_output_stopped()` says so, and the output task skips the
frame. A local layer (a scene, the web console) keeps the output going.
The node's own setting can also be "stop output". ArtAddress AcFail*
commands set the port's own value when it has one, otherwise the node's,
and each port's ArtPollReply reports its own failsafe in Status3 (stop
output reports hold, as Art-Net has no code for it).

Delta output (`delta_output`, ArtAddress AcStyleDelta 0xA0 / AcStyleConst
0xB0, GoodOutputB bit 6 clear) still looks at the levels at the port's
refresh rate but sends a frame only when they changed, and the same frame
again every 800 ms (`DMX_DELTA_KEEPALIVE_MS`, as sACN repeats unchanged
data) so fixtures don't see a loss. It follows content, not packets:
consoles often resend unchanged data at full rate. Scenes, fades and test
patterns change the levels too, so they need nothing special. Tester mode
always sends continuously and has no loss behaviour of its own.

ArtPollReply's VersInfo is the major and minor number of `PROJECT_VER`
(1.2.0 gives 0x0102); it used to be fixed at 1.0.

## Other Art-Net nodes

`artnet_receiver` parses other nodes' ArtPollReply
(`artnet_parse_poll_reply()`), before the filter that drops the node's own
packets: a reply from (or naming) this node's IP address with a different
MAC address is an IP address clash. `source_monitor` keeps one row per node
and BindIndex for 30 s. Art-Net 4 nodes answer the poller directly, so
`GET /api/network/nodes` also queues an ArtPoll (broadcast from the receive
task, at most every 5 s, `artnet_receiver_poll_network()`); the node only
polls while someone has the Overview page open:

```json
{"listening": true, "polled": true,
 "nodes": [{"ip": "192.168.1.60", "mac": "24:6f:28:aa:bb:01", "short_name": "Stage left",
            "long_name": "Stage left rack", "firmware": 260, "oem": 11200, "esta": 32752,
            "style": 0, "bind_index": 1, "dhcp": true, "conflict": false, "replies": 12, "age_ms": 1200,
            "ports": [{"output": true, "input": false, "protocol": 0, "in": 0, "out": 0,
                       "in_data": false, "out_data": true}]}]}
```

The page groups the rows by IP and MAC address and the signal check adds the
clashes: another device on this node's address, two other devices sharing
an address, and two inputs (this node's or others') sending the same
Art-Net universe.

## Signal check

`GET /api/network/sources` (signed in) is the "why is nothing lighting
up?" data behind the Overview page's Signal check card:

```json
{"listening": true,
 "interfaces": [{"name": "wifi", "ip": "192.168.1.47", "netmask": "255.255.255.0", "rssi": -58, "ssid": "Venue", "channel": 6}],
 "ports": [{"port": 1, "output": true, "mode": 1, "protocol_mode": 4, "merge_mode": 0,
            "artnet": 0, "artnet_backup": -1, "sacn": 1, "live_sources": 1, "contributing": 1, "rules": false,
            "local_inputs": [], "local_only": false, "input": false}],
 "sources": [{"protocol": "sacn", "ip": "192.168.1.31", "name": "Desk", "universe": 1,
              "priority": 100, "pap": false, "preview": false, "slots": 512, "sequenced": true,
              "packets": 5120, "lost": 3, "lost_recent": 0, "late": 0, "rate": 44.0,
              "max_gap_ms": 31, "age_ms": 12, "ports": [1], "filtered": []}],
 "advertised": [{"ip": "192.168.1.31", "name": "Desk", "universes": [1, 2], "count": 2, "age_ms": 4100}]}
```

`source_monitor` keeps one row per protocol, sender (sACN: CID) and
universe, whether a port uses it or not. Art-Net arrives by broadcast, so
the node hears every universe; sACN multicast arrives only for the
universes the node joins, so `sacn_receiver` also joins the universe
discovery group (239.255.250.214) and `advertised` lists what consoles say
they send. `lost` counts missing sequence numbers, `lost_recent` and
`max_gap_ms` cover the last 10-20 s, a 2 s pause restarts the sequence check,
and rows are dropped after a minute of silence. `ports` / `filtered`
follow the routing rules: `filtered` is a port on that universe whose
protocol setting drops this protocol. The hints themselves are worked out in
the browser (`sourceHints()` in `main.js`).

## Input ports: Send to

An input port (DMX in, sent to the network) has `send_to` in its settings:

```json
"port2": {"mode": 2, "send_to": "list", "send_list": ["10.0.0.20", "10.0.0.21"]}
```

- `"all"` (default): Art-Net broadcast and sACN multicast, as before.
- `"nodes"`: Art-Net unicast to every node whose ArtPollReply has an output
  on the port's universe (`srcmon_output_nodes()`, looked up once a second;
  the main loop keeps polling while a port uses this). Broadcast while no
  node is known. sACN stays multicast.
- `"list"`: Art-Net and sACN unicast to up to 4 addresses
  (`CONFIG_SEND_LIST_MAX`). Not multicast, not 0.0.0.0 or 255.255.255.255,
  and at least one.

Wi-Fi access points hold broadcast and multicast frames until the next
DTIM beacon and never resend a lost one, so on Wi-Fi unicast is the
reliable choice. Changing the destination ends a running sACN stream first
(stream-terminated packets to the old destinations).

## Settings files and the start-up notice

`config_save()` writes `config.json` through a temporary file and two
renames (`storage_replace_file()`), keeping the version it replaces as
`config.bak`. LittleFS renames are atomic, so a power cut leaves a whole
file. At start-up a `config.json` that can't be read or parsed is renamed
to `config.bad` and `config.bak` is loaded instead; if that fails too the
node starts with factory settings. A storage partition that would not mount
is formatted (`storage_was_formatted()`), except on a blank board.

Either way `config_boot_notice()` returns a sentence for the user. It is
`settings_notice` in `GET /api/system/info` and the USB `info` command, the
web UI shows it as a banner, and `POST /api/system/notice/clear` ("Got it")
hides it until the next restart. A factory reset deletes `config.bak` and
`config.bad`.

All settings changes go through `config_lock()` (recursive). Saves are
numbered under the lock and written in order under a separate mutex, so a
slower writer can't overwrite a newer file with older settings. Slow work
(PBKDF2, restarting ports, writing scenes) runs outside the lock, and
`app_apply_config()` works from `config_snapshot()`.

## Firmware upload check

Every image carries `"\x01GLBOARD\x02" BOARD_ID "\x03"` (`k_board_tag` in
`web_server.c`). `POST /api/system/firmware-upload` scans the upload for it
as it streams in (`fw_tag.c`) and stops as soon as it finds another board's
ID. An image without the marker (firmware from before the check, or not
this firmware) is refused at the end. Nothing is written to the boot
partition in either case. Install those over USB with the browser flasher.

## Memory and task stacks

`GET /api/diag/health` (System page, Memory card):

```json
{"internal_free": 59403, "internal_min_free": 38919, "internal_largest": 24576, "internal_low": 16384,
 "psram_free": 8348740, "psram_min_free": 8348436, "stack_low": 512,
 "stacks": [{"task": "reset_btn", "free": 684}, {"task": "IDLE1", "free": 696}]}
```

Internal RAM is what Wi-Fi, lwIP and drivers need (PSRAM can't stand in),
and `stacks` lists the tasks with the least stack left over since start-up,
in bytes (`uxTaskGetSystemState()`, so `CONFIG_FREERTOS_USE_TRACE_FACILITY`
is on). The main loop logs the same every 30 s, marked "LOW" below 16 KB of
internal RAM or 512 bytes of stack: as a warning while internal RAM is low
right now or when a lowest figure falls further, otherwise as information.
On esp-node-2rdm, received packets and lwIP buffers go in PSRAM
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). It isn't free: the option rules
out dynamic Wi-Fi transmit buffers, and 16 static ones (1.6 KB each) are
allocated at start-up. All told, 16.5 KB less internal RAM is free when idle.
Measured on the prototype on 2026-11-23, same source both ways, four restarts
each, about 1,900 Art-Net packets/s sent:

| | free when idle | lowest, flood alone | lowest, flood + one web client | smallest largest block seen | failed requests |
|---|---|---|---|---|---|
| without the option | 56.8 KB | 20.6 KB | 5.3 KB | 20.5 KB | 2 |
| with it, as shipped | 40.3 KB | 34.9 KB | 23.7 KB | 29.7 KB | 0 |

(KB = 1,000 bytes.) With RDM on, a background discovery pass runs on each
output port every 30 s, the first about 33 s after start-up, and briefly
takes about 17 KB of internal RAM with nothing connected (6.5 KB for the
task, the rest for its device lists). A low-water mark taken before the first
pass looks much better than one taken after, so compare like with like. That
pass takes an idle node to 23.8 KB, and the largest free block was seen at
12.8 KB during one. Without the option the mark fell to 15.3 KB over the same
period, further than a pass explains.

The table is not the worst case either. With the option and two or three web
clients polling during a flood, the same build fell to 17.4 KB, and to
16.9 KB in a 12 minute soak: about 0.5 KB above the 16 KB warning line. A
flood can also hold up `/main.js` for seconds (7.2 s once, one timeout at
10 s). The build without the option was not measured with several clients.

The option also changes Kconfig defaults: 16 RX buffers
and a BA window of 16 (about 10 KB more internal RAM), and no limit on
out-of-order TCP packets. The board's `sdkconfig.defaults` pins the values
that were measured (10, 6 and 4), so a clean build gets the same. ESP-IDF
recommends a window of 16 for throughput with this option; measure again
before changing it. Boards without PSRAM don't use any of this.

Don't build a large response with cJSON: it costs about 40 bytes of internal
RAM per number, PSRAM or not. 512 levels took 22 KB, so a levels poll during
a flood could use all of it. The levels, tester channel and log responses use
`web_stream.h`, which needs one 1 KB buffer.

## Task watchdog

The task watchdog (10 s, panic) watches the DMX tasks, the network
supervisor, the Art-Net and sACN receive tasks, the RDM bridge and the main
loop. Each wakes at least every second; a task stuck for 10 s restarts the
node and leaves a crash report. The web server is not watched: a stuck web
page is better than a restart that blacks out the DMX outputs.

## Boot-loop guard (safe mode)

A crash within a minute of starting, three times in a row, makes the next
start a safe one (`main/app_boot_guard.c`): the network, the web UI and
Art-Net work as usual, but every DMX port stays off. The count lives in RTC
memory (`RTC_NOINIT_ATTR`), which a crash, watchdog or brown-out reset keeps
and power-on clears; it goes back to zero once the node has run for a
minute. `GET /api/system/info` has the reason as `safe_mode`, the web UI
shows it as a banner, and `POST /api/system/safe-mode/leave` ("Start the
DMX ports") starts the ports. The Art-Net node report says so too. A
restart without a crash starts normally. New firmware that crashes before
it is marked valid (30 s) is rolled back by the bootloader first.

## DMX input frames

The next DMX frame starts overwriting esp_dmx's receive buffer one break
after the last one ended, which can be well under a millisecond, and a
stray break right after a frame empties it before the input task wakes
(`dmx_receive()` then reports nothing). The receive interrupt therefore
copies each finished level frame (`rx_frame` in `service.h`, marked
WIFIDMX), and the input task forwards any new copy (`dmx_read_received()`)
whatever `dmx_receive()` reported, so a frame is never a mix of two and
never lost that way. An input task more than a frame late uses the newest
frame and counts the ones it never saw as `dropped_late` ("Frames skipped
for a newer one" in the tester).

Two more changes to esp_dmx's receive side, both marked WIFIDMX in
`hal/uart.c`: upstream took every short packet's length as the size to
expect from then on, so a single packet cut short (a cable pulled
mid-frame) made every later frame end early until the port restarted. A
shorter size is now only taken when the same length comes twice in a row
(level frames only: text and SIP packets are short anyway), and the size
grows back when a longer frame arrives, which is then handed over whole.
A short frame ends at the next one's break, so `dmx_receive()` no longer
marks that next frame stale (the interrupt ignored it, losing the frame
after every short one), and a break whose framing error is flagged before
the break itself still ends a short frame (`rx_err_head`). `test_dmx_rx.c`
covers all of it on the host; none of it has seen a real short-frame source
yet. Slots a short frame
leaves out keep their last level (as fixtures do) instead of dropping to
zero; after a signal loss they start from zero again.

## esp_dmx: send, RDM and other fixes

`components/esp_dmx` is esp_dmx 4.1.0 with fixes, each marked WIFIDMX.
Updating it from upstream loses them, so run the host tests after any update.

- **Timing:** the mark after break is counted from the real end of the
  break, and a controller leaves 176 us of quiet after any reply or garbled
  reply. Waits for a frame being sent are 30 ms (upstream's 23 ms could end
  before a full frame had), and an RDM exchange may take 50 ms. Both end on
  the interrupt, long before that.
- **Locks and waits:** every way out gives the driver's lock back, a stale
  task notification can't end a wait, and a send that never finishes is reset.
- **Responder:** a request can't assert. Wrong lengths, sub-devices and PIDs
  are NACKed, non-requests ignored, SET values checked, and what RDM sets is
  saved every 2 s (by the input task: the driver only saved when DMX stopped)
  and when the driver is removed.
- **Controller:** a reply only counts if its class, PID, UIDs and length
  answer the request, reads are bounded by the caller's buffer, and ACK_TIMER
  is in tenths of a second. A string needs room for its null: 32 characters
  need a buffer, and a `size`, of 33.

`test_dmx_rdm.c` runs the whole driver on a simulated clock and wire. It has
one task and exact timing, so races between cores rest on reading the code.

In `dmx_handler`:

- Drivers are installed from a task on the DMX core, because ESP-IDF puts an
  interrupt on the core that asks for it. They can be removed from any core.
- Background discovery un-mutes everything first, then mutes what it knows:
  a muted fixture that had dropped off the list could otherwise never be
  found again. A fixture leaves the list after two missed passes.
- The list holds 128 fixtures with PSRAM and 64 without (`DMX_MAX_DEVICES`,
  at most 200). It only takes RAM for what was found, but a discovery needs
  160 bytes per possible fixture while it runs.
- The Art-Net RDM relay only passes on requests, and only the answer from
  the fixture that was asked.
- Discovery reads each fixture's details up to 3 times when a reply is
  lost or garbled (`ask_again()`), as the bridge does for GETs.
- Setting an address (`dmx_handler_rdm_set_address()`): a fixture that
  answers ACK_TIMER has taken it, so the handler waits and reads the
  address back. A broadcast gets no answer by design and counts as done
  once it was sent.
- `dmx_handler_stop_port()` waits for its tasks as long as they take (the
  task watchdog catches one that is stuck), so a port is never restarted
  under a task that is still running.
- A responder refuses a start address whose footprint would run past slot
  512 (`dmx_start_address_fits()` in esp_dmx's `device.c`), the same rule
  the boot check uses to reset one.

`rdm_bridge` (RDM over Art-Net) queues requests in a ring buffer of slim
messages: 8 KiB in PSRAM where there is PSRAM, else 2 KiB of internal RAM,
which holds about 30 typical requests. A request that waited more than 3 s
is dropped (the console has given up), and a GET whose reply was lost or
garbled is sent again, up to 3 tries, except QUEUED_MESSAGE and
STATUS_MESSAGE (the fixture hands those over only once). SETs are never
repeated. `rdm_retries`, `rdm_expired` and `rdm_queue_full` count them.
`POST /api/rdm/discover` starts discovery on every RDM port and answers
202 at once (`{"started":[1]}`); the web UI follows `rdm_discovering` in
`/api/ports/status` and then reads the device list.

`tools/artnet_rdm_test.py` tests RDM over Art-Net against a running node:
it reads the device list with ArtTodRequest, then sends each device RDM
requests in ArtRdm and checks the answers, including a burst sent all at
once. `--ghostlight` adds the checks for a Ghostlight port in RDM
responder mode, `--write` changes the label, address and identify and puts
each back. Standard library only; it needs UDP port 6454 on your computer.

## W5500 Ethernet

- **Reset pulse:** where the board wires the W5500's RSTn
  (`BOARD_ETH_RST_GPIO`: ghostlight-rev1, esp32-devkitc), the driver holds it
  low for 1 ms and waits 3 ms before using the chip (`w5500_reset_hw()` in
  `network_manager.c`, replacing ESP-IDF 5.2's 100 us). The datasheet asks
  for at least 500 us low and up to 1 ms for the PLL to lock after release.
  The prototype has no reset line.
- **A chip gone deaf:** a W5500 can stop receiving while its link stays up
  (a glitch resets it and its socket and MAC address are gone). The netif
  input is wrapped to count frames (`eth_input()`), and the network
  supervisor calls `network_ethernet_watch()` 4 times a second. With the MAC
  filter off every frame on the segment counts, so after 60 s of nothing
  with the link up the chip is reset and set up again
  (`eth_restart_chip()`): driver, netif, sockets and mDNS stay, the address
  comes back as after a cable replug. A try that brings nothing back doubles
  the wait, up to 16 minutes, so a network that really is silent costs a
  short break now and then. `restarts` in the `ethernet` object of
  `GET /api/network/status` counts them. The logic is `eth_watch.c`
  (host-tested); the re-setup itself has not been seen on hardware.

## Captive portal

When the node's own access point starts, `network_manager` starts a small
DNS server (`captive_dns.c`, UDP 53). The AP's DHCP already hands out the
node as the DNS server. For clients in the AP's subnet it answers every A
query with the node's address (TTL 10 s) and other types with an empty
answer; the web server's 404 handler then redirects a request for any other
host (`captive.apple.com/hotspot-detect.html`,
`connectivitycheck.gstatic.com/generate_204`, ...) to `http://<AP IP>/`, so
phones open the setup page by themselves. `network.ap_captive` (default on,
"Open this page when a phone or laptop joins" in the UI) turns it off: names
then get NXDOMAIN and unknown pages a plain 404. Clients on Ethernet or the
venue Wi-Fi are never answered or redirected.

## USB setup

With `CONFIG_NODE_SERIAL_SETUP` (on by default) the node takes commands on
the port it is flashed through: the console UART (boards with a USB-serial
chip) and, on the ESP32-S3, the chip's own USB port. The browser flasher
(**Set up over USB** on
[defeatedwarrior.github.io/flasher.html](https://defeatedwarrior.github.io/flasher.html))
uses it to set up a node straight after installing it; it also gets a node
with a lost address or admin password back. One command per line, JSON after
`#GL `, one reply line per command after `#GL> `:

```
#GL {"id":1,"cmd":"info"}
#GL> {"ok":true,"firmware_version":"1.2.0","board":"Ghostlight Node rev1",...,"id":1}
#GL {"id":2,"cmd":"set","config":{"node_info":{"short_name":"Stage Left"}}}
#GL> {"ok":true,"restart_needed":false,"id":2}
```

Commands: `info`, `get` (the settings without passwords), `set` (the same
document `POST /api/config` takes, checked by the same code), `password`
(admin password, or `{"no_password":true}` to run the web UI without one),
`scan`, `connect` (a saved Wi-Fi network) and `restart`. An empty
`network.ap_password` makes the node's own access point an open network.
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
   `BOARD_*` names listed at the top of `components/board/include/board.h`,
   including `BOARD_ID`, which must be the `boards/<your-board>` folder name
   (web firmware uploads are checked against it).
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
   `.github/workflows/build.yml`. The release workflow and the browser
   flasher find the new folder on their own. The flasher lists the board with
   the first comment line of its `sdkconfig.defaults`, so make that line say
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
| `test_artnet.c` | Art-Net parsing: ArtDmx, ArtPoll/ArtPollReply, ArtAddress, ArtSync, ArtTod*, ArtRdm, ArtIpProg, other nodes' replies and IP clashes, sending ArtPoll, bad lengths and IDs |
| `test_sacn.c` | E1.31 parsing: data, per-address priority, sync, universe discovery, priorities, preview / stream-terminated, bad headers |
| `test_merge.c` | merge engine: HTP, LTP, last source, backup, disable, priorities, timeouts, failsafe, sync, another port's DMX input, AcCancelMerge |
| `test_config.c` | `config_to_json` / `config_from_json` round trip, loading the file, `config_sanitize`, channel rules, DMX from another port, "Send to" lists, the largest settings file, the previous copy (`config.bak`): damaged files, a power cut between the two renames, the start-up notice |
| `test_rules.c` | channel rules: patch, limit and park, their order, bad rules, per-port copies |
| `test_srcmon.c` | the signal check's sender table: sequence loss and wrap-around, rate, gaps, ageing, discovery lists, the other-node table and the nodes that output a universe |
| `test_dns.c` | captive portal DNS answers: A / other types / off, EDNS, malformed queries |
| `test_webstream.c` | the chunked JSON writer for large responses: the same text as cJSON, chunk edges, a failed send |
| `test_dmx_rx.c` | esp_dmx's receive interrupt and `dmx_receive()`: short, cut, growing and mixed-length frames, stray breaks, other start codes, RDM between frames; each with both orders of break and framing error, and a task that wakes late |
| `test_dmx_rdm.c` | the whole esp_dmx driver: RDM responder against any request, controller matching, waits and gaps, break / MAB, lock release, install and delete |
| `test_dmx_handler.c` | `dmx_handler.c` with a fake driver: RDM discovery with lost replies and pulled cables, the list limit, the raw relay, ACK_TIMER, setting addresses (ACK_TIMER, broadcast), stopping a slow task, saving, the install core, the output task's stop output and delta output |
| `test_rdm_bridge.c` | the RDM over Art-Net queue: bursts, a full queue, old requests, GET retries and what is never retried |
| `test_eth_watch.c` | when a deaf W5500 is set up again: traffic, no link, the back-off, recovery, clock wrap |
| `test_fwtag.c` | finding the board marker in a firmware upload: in one piece, split across every chunk boundary, false starts, bad IDs |

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
- `test_dmx_rx.c` has its own stubs (`stubs_dmx/`): a UART that is only a
  FIFO and interrupt flags. It proves the driver's logic, not its timing on
  a chip.
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

It then creates a GitHub Release for the tag with all these files, a
`SHA256SUMS.txt`, and `flasher.json` (written by
`tools/ci/release.py flasher`): the version and, for each board, its chip
family (from `CONFIG_IDF_TARGET`), its description and its file names.

The browser flasher is not in this repository. It is
[defeatedwarrior.github.io/flasher.html](https://defeatedwarrior.github.io/flasher.html),
in the [DefeatedWarrior.github.io](https://github.com/DefeatedWarrior/DefeatedWarrior.github.io)
repository. A workflow there (`.github/workflows/ghostlight-firmware.yml`)
checks for a new release every six hours, downloads its `flasher.json` and
full images, writes one [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
manifest per board, and commits them next to the page. Browsers can't load
GitHub release downloads from another site, so the images have to be copied
there. To put a release on the page straight away, run that workflow by hand
(**Actions → Ghostlight Node firmware → Run workflow**). The page only lists
full releases, not pre-releases, unless you give that workflow the tag.

To try the release workflow without releasing anything, run it from the
Actions tab here (**Release → Run workflow**). It builds every board and
keeps everything it would have attached as a workflow artifact named
`release-dry-run`. It creates no release.
