# thredge

An ESP32-C5-based OpenThread Border Router — minimal footprint, maximum
capability. Community firmware that turns a single ESP32-C5 into an OpenThread Border
Router. The C5 has dual-band Wi-Fi and an on-chip 802.15.4 radio, so no external
RCP module is needed.

Built on Espressif's [esp-thread-br](https://github.com/espressif/esp-thread-br)
(`basic_thread_border_router`) with:

- **Native 802.15.4 radio** configuration for the ESP32-C5 (no RCP or RCP update)
- **Captive-portal Wi-Fi setup** — with no stored credentials the device
  broadcasts `ESP-ThreadBR-XXXX`; join it and configure Wi-Fi at `http://192.168.4.1`
- **A project-specific web UI** on port 80 for status, Wi-Fi, Thread, topology,
  and firmware updates, using Espressif's border-router APIs for Thread data
- **Runtime Wi-Fi settings**: view the current connection and select a Wi-Fi
  band or enter new credentials without reflashing
- **Thread network management**: join an existing mesh using its active dataset
  or a discovered network's key, or form a new network
- **OTA updates** from GitHub releases or manual `.bin` upload, with automatic
  rollback if a new build fails to boot
- **Factory reset**: hold the BOOT button (GPIO28) for 5 s to erase Wi-Fi
  credentials and return to setup mode
- **Web flasher**: a GitHub Pages site so users can install over USB from a
  browser without installing the ESP-IDF toolchain

## For users

1. Open the [flasher page](https://fexiven.github.io/thredge/) in Chrome or
   Edge, plug the board in via USB, and click **Install**.
2. Join the `ESP-ThreadBR-XXXX` Wi-Fi network and enter your home Wi-Fi details
   on the setup page.
3. Manage the router at `http://esp-otbr.local` (or its IP address).

To join an existing Thread mesh (for example, Home Assistant's), use the
Thread page to paste its active dataset or discover the network and supply its
network key. The active dataset contains sensitive network credentials; handle
it like a password.

## For developers

### Setup

```bash
git clone --recurse-submodules https://github.com/Fexiven/thredge.git
cd thredge
./scripts/apply-patches.sh
```

`scripts/apply-patches.sh` applies `patches/` to `third_party/esp-thread-br`.
It is idempotent, and CI runs it before every build. The submodule tracks
upstream main (Dependabot updates it); if a patch stops applying, the script
fails and the patch needs rebasing.

Requires ESP-IDF **v5.5.2 or newer**. CI builds with v6.0.3.

### Build

With a local ESP-IDF:

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32c5 build
```

Or in a container, with no toolchain on the host:

```bash
podman run --rm -v "$PWD":/project:z -w /project docker.io/espressif/idf:v6.0.3 \
  bash -c "git config --global --add safe.directory '*' && idf.py set-target esp32c5 build"
```

### Flash

Easiest is the [flasher page](https://fexiven.github.io/thredge/). Over USB
from a host, note that the ESP32-C5 needs **esptool 4.9 or newer**:

```bash
idf.py merge-bin -o otbr-merged.bin
uvx --from "esptool==4.12.0" esptool.py --chip esp32c5 -p /dev/cu.usbmodem101 \
  write_flash 0x0 build/otbr-merged.bin
```

To update the app without erasing stored Wi-Fi credentials, flash only the app
partition: `write_flash 0x20000 build/esp32c5_otbr.bin`.

Boards using the chip's native USB (such as Seeed XIAO ESP32-C5) appear as
`/dev/cu.usbmodem*`; boards with a USB-UART bridge (ESP32-C5-DevKitC-1) appear
as `/dev/cu.usbserial-*` and need `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` instead of
the USB Serial/JTAG console used by the default configuration.

### Configuration

Use `idf.py menuconfig` to configure:

- **OTBR Firmware** — mDNS hostname and factory-reset GPIO/hold time
- **OTBR OTA** — the release manifest URL
- Upstream options under **ESP Thread Border Router Example** and
  **Component config → OpenThread**

### Layout

```
main/                       app entry, radio configuration, Kconfig
components/otbr_web/        embedded web UI and Wi-Fi REST endpoints
components/otbr_ota/        OTA REST API and rollback confirmation
patches/                    patches applied to the Espressif submodule
scripts/apply-patches.sh    applies the upstream patches
third_party/esp-thread-br/  Espressif BR SDK (git submodule)
web/                        flasher page and manifest templates for GitHub Pages
partitions.csv              dual-OTA 8 MB layout — do not change after release
.github/workflows/          build, release, and Pages pipeline
```

### Notes

- **The partition table is frozen for released devices.** OTA can replace an
  app image, but cannot update the partition table. Changing the layout can
  strand devices that already use a released layout.
- New OTA images boot in a pending-verify state. The firmware confirms the
  image after it gets an IP address; otherwise the bootloader can roll back on
  the next reset.
- The checked-in partition table is for 8 MB flash. A 4 MB board needs a
  different layout and smaller OTA slots; choose and test that layout before
  shipping.
- Wi-Fi credentials and the Thread dataset live in NVS and survive OTA.

### Web UI and APIs

The pages in `components/otbr_web/web_ui/` are embedded in the application by
`components/otbr_web/CMakeLists.txt`. There is **no SPIFFS web-UI partition** in
this project. A UI change therefore requires rebuilding and updating the app
firmware; the UI and code roll back together. The UI is small, so this avoids
an additional partition, separate flashing steps, and the risk of a UI being
incompatible with the running firmware.

Thread information and operations use the HTTP server provided by Espressif's
`esp-thread-br`; the project adds Wi-Fi and OTA routes and serves its own pages.
The server listens on **HTTP port 80**. You can check it from a computer on the
same LAN (replace the hostname with the router's IP if mDNS does not resolve):

```bash
curl http://esp-otbr.local/node
curl http://esp-otbr.local/get_properties
curl http://esp-otbr.local/available_network
```

`/available_network` scans for **Thread** networks; it is not a Wi-Fi scan.
Current project-specific routes are:

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/wifi/status` | GET | Current Wi-Fi association, signal, band, IP, and MAC |
| `/api/wifi/config` | POST | Save Wi-Fi credentials and reconnect |
| `/api/wifi/band` | GET, POST | Read or change the Wi-Fi band preference |
| `/api/ota/version` | GET | Running firmware version |
| `/api/ota/check` | GET | Compare the running version with the release manifest |
| `/api/ota/install` | POST | Install the release from the manifest |
| `/api/ota/update` | POST | Install an uploaded `.bin` image |

The upstream server also exposes Thread management and diagnostics endpoints,
including `/node`, `/node/state`, `/node/dataset/active`, `/get_properties`,
`/available_network`, and `/topology`. The API is enabled as part of the border
router web server; if `/node` responds, you are reaching it. Paths under that
server use the same host and port as the UI.

**Home Assistant note:** an HTTP API endpoint responding does not by itself
mean Home Assistant will automatically discover or manage this device through
its OTBR integration. `thredge` does not provide a Home Assistant integration.
The documented workflow is to use Home Assistant's Thread credentials to join
the same Thread mesh. Whether Home Assistant can use this particular device as
its managed border router depends on the integration and discovery support in
the Home Assistant version in use.

**Security:** the web UI and APIs currently use unauthenticated HTTP. They can
read Thread credentials and change Wi-Fi/Thread settings or install firmware.
Do not expose port 80 to the internet or an untrusted network. Authentication
and transport security are not currently implemented.

## Known limitations

- **Single-chip trade-off.** The ESP32-C5 uses a shared RF path, so Wi-Fi and
  Thread cannot receive simultaneously. Expect reduced Thread performance under
  heavy Wi-Fi load compared with a two-chip design using a separate RCP.
- **First-boot Wi-Fi setup:** the upstream SoftAP setup page's network scan is
  known not to find networks reliably. Entering the SSID and password manually
  works. The current thredge Wi-Fi page does not provide a Wi-Fi scan feature.
- **Commissioner PSKd joining is not exposed by the thredge UI.** The UI can
  join using an active dataset or a discovered network's network key.
- **The upstream `/join_network` route is not used by the UI.** The UI writes a
  complete dataset through `PUT /node/dataset/active`, because joining requires
  details such as the extended PAN ID as well as channel, PAN ID, and key.

## License

Apache-2.0, except where a file says otherwise: `main/esp_ot_config.h` is
adapted from an Espressif example and keeps its original CC0-1.0.

`third_party/esp-thread-br` is a submodule and is licensed separately under
Apache-2.0.
