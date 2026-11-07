# thredge

An ESP32-C5-based OpenThread Border Router — minimal footprint, maximum
capability. Community firmware that turns a single ESP32-C5 into an OpenThread Border
Router. The C5 has both dual-band Wi-Fi and an on-chip 802.15.4 radio, so no
external RCP module is needed — Wi-Fi runs on 5 GHz while Thread gets the
2.4 GHz 802.15.4 radio to itself.

Built on Espressif's [esp-thread-br](https://github.com/espressif/esp-thread-br)
(`basic_thread_border_router`), used **unpatched** as a git submodule, with:

- **Native 802.15.4 radio** configuration for the ESP32-C5 (no RCP, no RCP update)
- **Captive-portal Wi-Fi setup**: with no stored credentials the device
  broadcasts `ESP-ThreadBR-XXXX`; join it and a setup page opens at
  `http://192.168.4.1` (upstream's `OPENTHREAD_BR_SOFTAP_SETUP`)
- **Thread REST API** on port 80: Espressif's ot-br-posix-compatible API
  (`/node`, `/node/dataset/active`, `/node/state`, ...). There is deliberately
  no web UI beyond that
- **OTA updates** from GitHub releases, either **automatic** (default) or
  **manual**, switchable at runtime, plus `.bin` upload. A small update server
  on port **8080** has one plain status page and a JSON API. New images are
  verified before they are written and roll back automatically if they do not
  come up
- **Factory reset**: hold the BOOT button (GPIO28) for 5 s to erase Wi-Fi
  credentials and return to the setup AP
- **Web flasher**: a GitHub Pages site so users install over USB from the
  browser, with no toolchain needed

## For users

1. Open the [flasher page](https://fexiven.github.io/thredge/) in Chrome or
   Edge, plug the board in via USB, click **Install**.
2. Join the `ESP-ThreadBR-XXXX` Wi-Fi network with your phone and enter your
   home Wi-Fi on the page that opens (pick your 5 GHz network if you have one).
3. That's it. The border router forms a Thread network and keeps itself up to
   date. `http://esp-otbr.local:8080` shows the firmware version and lets you
   check for updates, switch between automatic and manual updates, or upload
   a firmware file.

### Joining an existing Thread network

To extend an existing mesh (e.g. Home Assistant's) instead of forming a new
one, write its active dataset (the TLV hex HA shows under Settings → Devices
→ Thread) while Thread is stopped:

```bash
curl -X PUT -H 'Content-Type: application/json' -d '"disable"' http://esp-otbr.local/node/state
```

```bash
curl -X PUT -H 'Content-Type: text/plain' -d '<dataset hex>' http://esp-otbr.local/node/dataset/active
```

```bash
curl -X PUT -H 'Content-Type: application/json' -d '"enable"' http://esp-otbr.local/node/state
```

Or over the USB console: `dataset set active <hex>`, then `ifconfig up` and
`thread start`.

### Updates

| Mode | Behaviour |
| --- | --- |
| `auto` (default) | Checks the release manifest 5 min after boot and then every 24 h; installs a release only if it is **newer** than the running one and has not rolled back on this device before. |
| `manual` | Same checks, but only reports `update_available`. You start the install yourself. |

Switch with the page at `:8080`, or:

```bash
curl -X POST -H 'X-Thredge: 1' -d manual http://esp-otbr.local:8080/api/ota/mode
```

Safety rules that apply to every install:

- Downloads and uploads are checked for the right project name **before**
  anything is erased, and validated (checksum + SHA-256) before the boot
  partition is switched.
- A new image boots "pending verify". It is confirmed only after it has had an
  IP address for 60 s. If it crashes or resets before then, the bootloader
  goes back to the previous image. If it never comes online, the device
  reboots after 15 min to trigger that rollback.
- Nothing is installed while the running image is still pending verify,
  because that would overwrite the slot it would roll back to.
- Wi-Fi credentials and the Thread dataset live in NVS and survive updates.

## For developers

### Setup

```bash
git clone --recurse-submodules https://github.com/Fexiven/thredge.git
cd thredge
```

`third_party/esp-thread-br` is used exactly as upstream ships it, and
Dependabot bumps it monthly. Each bump PR is built by CI, so a breaking
upstream change fails before it can be merged. The one thing upstream does
that does not fit this firmware (packing its web UI into a `web_storage`
SPIFFS partition this layout does not have) is turned off from
`main/project_include.cmake` rather than by a patch.

Requires ESP-IDF **v5.5.2 or newer**. Earlier 5.5.x releases lack
`examples/openthread/ot_common_components/ot_examples_common`, which the
border-router component depends on, and dependency solving fails. CI builds
with v6.0.3.

### Build

With a local ESP-IDF:

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c5 build
```

Or in a container, with no toolchain on the host:

```bash
podman run --rm -v "$PWD":/project:z -w /project docker.io/espressif/idf:v6.0.3 \
  bash -c "git config --global --add safe.directory '*' && idf.py set-target esp32c5 build"
```

### Flash

Easiest is the [flasher page](https://fexiven.github.io/thredge/). Over USB
from a host, note that the ESP32-C5 needs **esptool 4.9 or newer** — older
versions detect the chip and then fail on the flash stub:

```bash
idf.py merge-bin -o otbr-merged.bin   # in the container
uvx --from "esptool==4.12.0" esptool.py --chip esp32c5 -p /dev/cu.usbmodem101 \
  write_flash 0x0 build/otbr-merged.bin
```

To update the app without erasing stored Wi-Fi credentials, flash only the app
partition: `write_flash 0x20000 build/esp32c5_otbr.bin`.

Boards using the chip's native USB (Seeed XIAO ESP32-C5 and similar) appear as
`/dev/cu.usbmodem*`; boards with a USB-UART bridge (ESP32-C5-DevKitC-1) appear
as `/dev/cu.usbserial-*` and need `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` instead
of the USB Serial/JTAG console this repo defaults to.

### Configuration

`idf.py menuconfig`:

- **OTBR Firmware**: mDNS hostname, factory-reset GPIO/hold time
- **OTBR OTA**: manifest URL, update server port, default update mode, check
  interval, confirmation delay and deadline
- Upstream options under **ESP Thread Border Router Example** and
  **Component config → OpenThread**

### Layout

```
main/                       app entry, radio config, Kconfig,
                            project_include.cmake (skips upstream's SPIFFS image)
components/otbr_ota/        update worker, update server (:8080), rollback
third_party/esp-thread-br/  Espressif BR SDK (git submodule, unpatched)
web/                        flasher page + manifest templates (GitHub Pages)
partitions.csv              dual-OTA 8 MB layout, frozen since 2026.09.0
.github/workflows/          build on PRs, build + release + Pages on tags
```

### Update API (port 8080)

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/` | GET | status page |
| `/api/ota/status` | GET | version, partition, mode, state, progress, latest release, last error |
| `/api/ota/check` | POST | check the manifest now |
| `/api/ota/install` | POST | install the manifest's release (also allows going back to it) |
| `/api/ota/upload` | POST | install a `.bin` sent as the raw request body |
| `/api/ota/mode` | POST | body `auto` or `manual` |

Every POST needs an `X-Thredge` header (any value). Browsers will not send a
custom header cross-origin without a CORS preflight, which this server never
approves, so a random web page cannot start an update. There is no
authentication beyond that: anyone on your LAN can reach the device, just as
they can reach the Thread REST API.

```bash
curl -X POST -H 'X-Thredge: 1' --data-binary @build/esp32c5_otbr.bin \
  http://esp-otbr.local:8080/api/ota/upload
```

### Releasing

Push a calendar-version tag (`2026.11.0`; a leading `v` is stripped). CI
stamps `version.txt`, builds, attaches `esp32c5_otbr.bin` and
`otbr-merged.bin` to a GitHub release and publishes the flasher and
`ota.json` to Pages. Devices in auto mode pick it up within a day. Versions
must keep increasing numerically: auto-update never installs a version that
compares as older or equal.

### Notes

- **Partition table is frozen.** OTA can never change it; altering it strands
  existing devices.
- Devices on 2026.09.x update to this firmware as usual (their Update tab);
  from then on they update themselves.

## Known limitations

- **Single-chip trade-off.** Espressif recommends a two-chip border router (a
  Wi-Fi host plus a separate 802.15.4 RCP) because the C5 has one RF path, so
  Wi-Fi and Thread cannot receive simultaneously. This firmware deliberately
  takes the single-chip route for simplicity and cost; expect reduced Thread
  throughput under heavy Wi-Fi load.
- **The stock SoftAP setup page may find no networks** on first boot (seen
  with earlier upstream versions). Entering the SSID and password by hand works.
- **No web UI for Thread.** By design: the REST API on port 80 (or the USB
  console) is the interface. `http://esp-otbr.local/` itself has nothing to
  serve.
- **Joining by commissioner PSKd is not documented here.** The REST API
  supports a commissioner join; this README only covers writing a dataset.

## License

Apache-2.0, except where a file says otherwise: `main/esp_ot_config.h` is
adapted from an Espressif example and keeps its original CC0-1.0.

`third_party/esp-thread-br` is a submodule rather than vendored source, and is
licensed separately under Apache-2.0.
