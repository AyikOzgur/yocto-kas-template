#!/bin/bash
# Ground station for the wfb-ng video link: puts a USB Wi-Fi adapter in monitor mode
# on the drone's channel, receives with wfb_rx and plays the MPEG-TS with low latency.
#
#   tools/wfb-ground.sh [interface]      default: the first USB Wi-Fi adapter
#
# Must match the drone (wfb-ng recipe / /var/wfb-video.env on the board):
#   WFB_CHANNEL     default 149 (5745 MHz)
#   WFB_PASSPHRASE  default wfb-video
# Player:
#   PLAYER=ffplay   (default) or gst, or none to only receive on udp://127.0.0.1:5600
#                   (e.g. for QGroundControl or your own player)
#
# Needs sudo (monitor mode, raw capture), iw, and wfb_rx / wfb_keygen from wfb-ng:
#   sudo apt install libpcap-dev libsodium-dev
#   git clone -b release-25.01 https://github.com/svpcom/wfb-ng.git && cd wfb-ng
#   make wfb_rx wfb_keygen && sudo install -m 0755 wfb_rx wfb_keygen /usr/local/bin
set -euo pipefail

CHANNEL=${WFB_CHANNEL:-149}
PASSPHRASE=${WFB_PASSPHRASE:-wfb-video}
PLAYER=${PLAYER:-ffplay}
PORT=5600

for tool in iw wfb_rx wfb_keygen; do
    if ! command -v $tool >/dev/null; then
        echo "$tool not found, see the top of $0 for how to install it." >&2
        exit 1
    fi
done

IFACE=${1:-}
if [ -z "$IFACE" ]; then
    for dev in /sys/class/net/*; do
        if [ -e "$dev/phy80211" ] && readlink -f "$dev/device" | grep -q /usb; then
            IFACE=$(basename "$dev")
            break
        fi
    done
fi
if [ -z "$IFACE" ]; then
    echo "No USB Wi-Fi adapter found, give the interface name as argument." >&2
    exit 1
fi

KEYS=$(mktemp -d)
NM_MANAGED=no
RX_PID=

cleanup() {
    [ -n "$RX_PID" ] && sudo kill "$RX_PID" 2>/dev/null || true
    sudo ip link set "$IFACE" down
    sudo iw dev "$IFACE" set type managed
    sudo ip link set "$IFACE" up
    [ "$NM_MANAGED" = yes ] && sudo nmcli device set "$IFACE" managed yes || true
    rm -rf "$KEYS"
}
trap cleanup EXIT

# Same passphrase as the drone: gs.key decrypts what drone.key encrypted.
(cd "$KEYS" && wfb_keygen "$PASSPHRASE" 2>/dev/null)

sudo -v
# Keep NetworkManager away from the adapter while it is in monitor mode.
if command -v nmcli >/dev/null && nmcli -t -f DEVICE device | grep -qx "$IFACE"; then
    sudo nmcli device set "$IFACE" managed no
    NM_MANAGED=yes
fi
sudo ip link set "$IFACE" down
sudo iw dev "$IFACE" set type monitor
sudo ip link set "$IFACE" up
sudo iw dev "$IFACE" set channel "$CHANNEL" HT20
echo "Receiving on $IFACE, channel $CHANNEL -> udp://127.0.0.1:$PORT"

# Prints link statistics every second (packets, FEC recovered, lost, RSSI).
sudo wfb_rx -K "$KEYS/gs.key" -p 0 -c 127.0.0.1 -u $PORT "$IFACE" &
RX_PID=$!

case "$PLAYER" in
ffplay)
    # No probing and no buffering; setpts=0 shows every frame as soon as it is decoded
    # instead of scheduling it by its timestamp.
    ffplay -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -framedrop \
        -probesize 32 -analyzeduration 0 -vf setpts=0 -window_title "wfb-ng channel $CHANNEL" "udp://127.0.0.1:$PORT"
    ;;
gst)
    # tsdemux adds 700 ms by default.
    gst-launch-1.0 -q udpsrc port=$PORT ! tsdemux latency=0 ! decodebin ! videoconvert ! \
        autovideosink sync=false
    ;;
*)
    wait "$RX_PID"
    ;;
esac
