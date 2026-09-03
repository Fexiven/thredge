# thredge

An ESP32-C5-based OpenThread Border Router — minimal footprint, maximum
capability. Community firmware that turns a single ESP32-C5 into an OpenThread Border
Router. The C5 has both dual-band Wi-Fi and an on-chip 802.15.4 radio, so no
external RCP module is needed — Wi-Fi runs on 5 GHz while Thread gets the
2.4 GHz 802.15.4 radio to itself.

Built on Espressif's [esp-thread-br](https://github.com/espressif/esp-thread-br)
(`basic_thread_border_router`) with:

- **Native 802.15.4 radio** configuration for the ESP32-C5 (no RCP, no RCP update)
- **Captive-portal Wi-Fi setup** — with no stored credentials the device
  broadcasts `ESP-ThreadBR-XXXX`; join it and a setup page opens at
  `http://192.168.4.1` (this is upstream's `OPENTHREAD_BR_SOFTAP_SETUP`)
- **Its own web UI** on port 80 -- Status, Wi-Fi, Thread and Update -- built
  on the border router's REST API rather than Espressif's stock frontend, so
  the pages are this project's to change
- **Runtime Wi-Fi settings**: view the current connection and scan both bands,
  then switch networks without reflashing
- **Thread network management**: join an existing mesh by pasting its active
  dataset (the credentials Home Assistant exports), or form a new one with
  generated credentials
- **OTA updates** on the **Update** tab: one-click install from GitHub
  releases or manual `.bin` upload, with automatic rollback if a new build
  fails to boot
- **Factory reset**: hold the BOOT button (GPIO28) for 5 s to erase Wi-Fi
  credentials and return to the setup AP
- **Web flasher**: a GitHub Pages site so users install over USB from the
  browser — no toolchain needed

## For users

1. Open the [flasher page](https://fexiven.github.io/thredge/) in Chrome or
   Edge, plug the board in via USB, click **Install**.
2. Join the `ESP-ThreadBR-XXXX` Wi-Fi network with your phone and enter your
   home Wi-Fi on the page that opens (pick your 5 GHz network if you have one).
3. That's it. Manage the router at `http://esp-otbr.local` -- Thread status,
   Wi-Fi settings and firmware updates are all there.

To join an existing Thread mesh (e.g. Home Assistant's), paste your network's
active dataset in the web interface instead of letting the device form a new
network.

## For developers

### Setup

```bash
git clone --recurse-submodules https://github.com/Fexiven/thredge.git
cd thredge
./scripts/apply-patches.sh
```

`scripts/apply-patches.sh` applies `patches/` to `third_party/esp-thread-br`.
It is idempotent, and CI runs it before every build. The submodule tracks
upstream main (Dependabot bumps it monthly), so the patches live here rather
than in a fork; if one stops applying, the script fails loudly and the patch
needs rebasing.

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

The Web GUI's SPIFFS image (`web_storage`) is built automatically.

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

- **OTBR Firmware** — mDNS hostname, factory-reset GPIO/hold time
- **OTBR OTA** — the release manifest URL
  (**change `OTBR_OTA_MANIFEST_URL` to your GitHub Pages URL before release**)
- Upstream options under **ESP Thread Border Router Example** and
  **Component config → OpenThread**

### Layout

```
main/                       app entry, radio config, Kconfig
components/otbr_web/        the web UI (web_ui/) and the Wi-Fi REST API
components/otbr_ota/        OTA REST API + rollback confirmation
patches/                    patches applied to the submodule
scripts/apply-patches.sh    applies them; run after clone, CI runs it too
third_party/esp-thread-br/  Espressif BR SDK (git submodule)
web/                        flasher page + manifest templates (GitHub Pages)
partitions.csv              dual-OTA 8 MB layout — do not change after release
.github/workflows/          tag-triggered build/release/Pages pipeline
```

### Notes

- **Partition table is frozen.** OTA can never change it; altering it strands
  existing devices.
- Rollback: new OTA images boot "pending verify" and are confirmed only once
  the device gets an IP (`components/otbr_ota`). A build that never gets that
  far reverts on the next reset.
- 4 MB modules: shrink both OTA slots to fit (with less headroom) and set
  `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` — but pick one layout before shipping.
- Wi-Fi credentials and the Thread dataset live in NVS and survive OTA.

### Web UI

The UI lives in `components/otbr_web/web_ui/` and is built into the
`web_storage` SPIFFS partition. Thread data comes from the border router's own
REST API (`/node`, `/get_properties`, `/node/dataset/active`, `/topology`,
...), so none of that logic is duplicated; this project adds only what upstream
lacks:

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/wifi/status` | GET | current association, signal, band, IP, MAC |
| `/api/wifi/scan` | POST | start a scan (returns immediately) |
| `/api/wifi/scan` | GET | scan progress and the last results |
| `/api/ota/version` | GET | running firmware |
| `/api/ota/check` | GET | compare against the release manifest |
| `/api/ota/install` | POST | install the release from the manifest |
| `/api/ota/update` | POST | install an uploaded `.bin` |

Joining a network uses the border router's own endpoints rather than any added
here: `PUT /node/state` with `"disable"`, then `PUT /node/dataset/active` with
the dataset (TLV hex as `text/plain`, or JSON), then `PUT /node/state` with
`"enable"`. The dataset must be written while Thread is stopped.

Editing a page means rebuilding and reflashing the SPIFFS image
(`build/web_storage.bin` at `0x5a0000`); the app partition does not change.

## Known limitations

- **Single-chip trade-off.** Espressif recommends a two-chip border router (a
  Wi-Fi host plus a separate 802.15.4 RCP) because the C5 has one RF path, so
  Wi-Fi and Thread cannot receive simultaneously. This firmware deliberately
  takes the single-chip route for simplicity and cost; expect reduced Thread
  throughput under heavy Wi-Fi load.
- **The stock SoftAP setup page still finds no networks** on first boot.
  Entering the SSID and password by hand works, and once the device is online
  the Wi-Fi tab scans both bands correctly. The same fix has not yet been
  applied to upstream's setup page.
- **Joining by commissioner PSKd is not exposed.** Joining is done by writing
  an active dataset. The device's REST API also supports a `pskdType`
  commissioner join; the UI does not offer it.
- **The border router's own `/join_network` is unused.** It writes only the
  channel, PAN ID and network key into the dataset, leaving out the extended
  PAN ID, so a node joined through it never gets past `detached`. The UI
  writes a complete dataset through `PUT /node/dataset/active` instead.

## License

Apache-2.0, except where a file says otherwise: `main/esp_ot_config.h` is
adapted from an Espressif example and keeps its original CC0-1.0.

`third_party/esp-thread-br` is a submodule rather than vendored source, and is
licensed separately under Apache-2.0.
