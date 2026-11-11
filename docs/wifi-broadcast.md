# Video and MAVLink over WiFi broadcast

How to get the drone's video and a two-way MAVLink link on a ground PC over wfb-ng
(WiFi broadcast): no access point, no association, one fixed 5.8 GHz channel.

```
drone                                                    ground PC
streamer-service ─ video ───▶┐                 ┌─▶ wfb_rx ─▶ ffplay
flight controller ─ UART ─ mavlink-router      │
                   telemetry ▶├─ RTL8812EU ~~~ Wi-Fi adapter ─▶ QGroundControl (UDP 14550)
                   commands  ◀┘                └◀─ commands
```

The IP path stays available next to it: QGroundControl can also connect to the board's
normal Wi-Fi address on TCP 5760, and the video is served there as SRT on port 9000.

## 1. Hardware

| Where | What |
|---|---|
| Drone | Radxa Zero 3W, RTL8812EU module (BL-M8812EU2) on its USB port, **both antennas attached** |
| Drone | Flight controller on UART4 (header pin 16 RX, pin 18 TX), MAVLink at 115200 baud |
| Ground | Linux PC with a 5 GHz Wi-Fi adapter that supports monitor mode and injection |

Ground adapters that are known to work:

- **RTL8812EU / RTL8812AU on USB**: the recommended choice. The PC's own Wi-Fi stays online.
- **Built-in MediaTek MT7925**: works, but the PC is offline while the script runs, unless a
  second adapter keeps the network connection.

Never power an RTL8812EU without antennas: transmitting without a load can destroy it.
A bare module needs a USB-A port (a USB-C port may not supply power to it).

## 2. Drone

Build the vendor image and flash it:

```sh
kas build boards/radxa-zero-3w-vendor.yml
```

The image is in `build/tmp-glibc/deploy/images/radxa-zero-3w-vendor/` (`.wic` for a full
flash, `.swu` for an update). Nothing has to be configured on the board. After boot, check:

```sh
systemctl is-active wfb-video wfb-mavlink-down wfb-mavlink-up mavlink-router   # 4x active
journalctl -u wfb-video -n 2 -o cat          # PKT line with non zero counters = on air
```

## 3. Ground PC, once

Tools and libraries, Ubuntu / Debian:

```sh
sudo apt install iw python3 ffmpeg git build-essential libpcap-dev libsodium-dev \
    linux-headers-$(uname -r)
```

Fedora:

```sh
sudo dnf install iw python3 git gcc gcc-c++ make libpcap-devel libsodium-devel \
    kernel-devel-$(uname -r)
# Fedora's own ffmpeg (ffmpeg-free) has no H.264 decoder, the player needs the full one
# from RPM Fusion:
sudo dnf install \
    https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
sudo dnf swap ffmpeg-free ffmpeg --allowerasing
```

If `kernel-devel-$(uname -r)` is not found, the running kernel is older than the
repository's: `sudo dnf upgrade`, reboot, and install it again.

wfb-ng, the same release as on the drone:

```sh
git clone -b release-25.01 https://github.com/svpcom/wfb-ng.git && cd wfb-ng
make wfb_rx wfb_tx wfb_keygen
sudo install -m 0755 wfb_rx wfb_tx wfb_keygen /usr/local/bin
```

QGroundControl, if MAVLink is wanted. Its default settings are fine.

An RTL8812EU adapter needs no manual driver setup: `tools/wfb-ground.sh` builds the driver
on first use (about 15 s, again after a kernel update) and loads it. With Secure Boot on
(`mokutil --sb-state`, common on Fedora installs), the unsigned driver is refused: turn
Secure Boot off in the firmware setup, or sign the module with an enrolled key (MOK).
Adapters with a driver in the kernel, like the MT7925, are not affected.

## 4. Run

```sh
tools/wfb-ground.sh                # the first USB Wi-Fi adapter
tools/wfb-ground.sh wlp194s0       # a specific adapter, e.g. the built-in one
```

- The video opens in `ffplay`. `PLAYER=gst` uses GStreamer, `PLAYER=none` only receives
  (MPEG-TS on `udp://127.0.0.1:5600`).
- Start QGroundControl: it connects by itself (UDP 14550).
- Ctrl+C or closing the player ends it and gives the adapter back to the system.

The terminal shows the video link once a second:

```
PKT  654:557180:0:654:37:3:0:235:261132
     │          │ │   │  └ lost (visible glitches)
     │          │ │   └ repaired by FEC
     │          │ └ decrypted
     │          └ decryption errors (wrong passphrase)
     └ packets received
```

MAVLink link statistics are in `/tmp/wfb-ground-mavlink.log`.

## 5. Settings

Channel and passphrase must be the same on both sides.

| Setting | Default | Drone | Ground |
|---|---|---|---|
| Channel | 149 (5745 MHz) | `WFB_CHANNEL` | `WFB_CHANNEL` |
| Passphrase | `wfb-video` | `WFB_PASSPHRASE` | `WFB_PASSPHRASE` |
| Transmit power, mBm | 1000 (10 dBm) | `WFB_TXPOWER` | `WFB_TXPOWER` |
| Country (regulatory domain) | unchanged | `WFB_REGION` | `WFB_REGION` |

- **Drone, at runtime**: one `NAME=value` per line in `/var/wfb-video.env`, then reboot.
- **Drone, in the image**: the same names in `local.conf` / the kas board file.
- **Ground**: environment variables, e.g. `WFB_CHANNEL=165 tools/wfb-ground.sh`.

**Set `WFB_REGION` on the ground PC** (your country code, e.g. `WFB_REGION=PL`) when it is
not connected to any Wi-Fi network, which is the normal case in the field. The kernel takes
the country from the connected network; without one, the 5.8 GHz channels are receive only:
video and telemetry still arrive, but the adapter silently transmits nothing, so
QGroundControl's commands never reach the drone. The script warns when that is the case.

**Set your own passphrase before flying.** Whoever knows it can send commands to the
flight controller. Pick a channel without other networks on it; `nmcli device wifi list`
shows which ones are busy.

## 6. When it does not work

| What you see | Cause | What to do |
|---|---|---|
| `PKT 0:0:0:…` only | Nothing received | Same channel on both sides? Drone transmitting (step 2 check)? Try another channel or adapter |
| Decryption errors count up | Passphrases differ | Set the same `WFB_PASSPHRASE` on both sides |
| Many lost packets at short range | Signal too strong, or a busy channel | Move a few metres apart, lower `WFB_TXPOWER`, change channel |
| `Cannot find device`, `socket error!`, `not in monitor mode` | The adapter dropped off USB | `journalctl -k \| grep 'USB disconnect'`; check its cable and power |
| Video works, QGroundControl does not connect | MAVLink path | Flight controller connected? `/tmp/wfb-ground-mavlink.log` should show packets |
| Telemetry works, commands do not ("vehicle did not respond", parameters do not load) | The ground adapter does not transmit | Set `WFB_REGION` (step 5). On the drone: `journalctl -u wfb-mavlink-up -f` must count packets |
| RTL8812EU driver build fails | Missing headers or compiler | Step 3 packages; log in `~/.cache/wfb-ground/…/build.log` |
| RTL8812EU driver does not load (`Key was rejected` / `Required key not available`) | Secure Boot | See step 3 |
| Packets arrive but the player shows no picture (Fedora) | `ffmpeg-free` without H.264 | The ffmpeg swap of step 3 |

## Where things are

| Part | Location |
|---|---|
| Ground script | `tools/wfb-ground.sh` |
| Radio setup and video on the drone | `sources/meta-distrotemplate/recipes-connectivity/wfb-ng/` (`wfb-video.service`) |
| MAVLink on the radio | same recipe, `wfb-mavlink-down.service` (radio port 16), `wfb-mavlink-up.service` (radio port 144) |
| MAVLink routing | `sources/meta-distrotemplate/recipes-connectivity/mavlink-router/files/main.conf` |
| RTL8812EU driver of the drone | `sources/meta-bsptemplate/recipes-kernel/rtl8812eu-driver/` |

The ground script builds the RTL8812EU driver at the commit of that recipe
(`RTL8812EU_COMMIT` in the script): change both together.
