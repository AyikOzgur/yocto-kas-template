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
# An RTL8812EU adapter (like the drone's) gets its driver built and loaded here, once per
# kernel, which needs git, a compiler and the kernel headers:
#   sudo apt install git build-essential linux-headers-$(uname -r)
set -euo pipefail

CHANNEL=${WFB_CHANNEL:-149}
PASSPHRASE=${WFB_PASSPHRASE:-wfb-video}
PLAYER=${PLAYER:-ffplay}
PORT=5600

# svpcom's RTL8812EU driver at the commit of the drone image (rtl8812eu-driver recipe).
RTL8812EU_GIT=https://github.com/svpcom/rtl8812eu.git
RTL8812EU_BRANCH=v5.15.0.1
RTL8812EU_COMMIT=48e6e449e089fa954e4e15079bd864039e2960da

for tool in iw wfb_rx wfb_keygen; do
    if ! command -v $tool >/dev/null; then
        echo "$tool not found, see the top of $0 for how to install it." >&2
        exit 1
    fi
done

rtl8812eu_plugged() {
    local dev
    for dev in /sys/bus/usb/devices/*; do
        [ "$(cat "$dev/idVendor" 2>/dev/null)" = 0bda ] && [ "$(cat "$dev/idProduct" 2>/dev/null)" = a81a ] &&
            return 0
    done
    return 1
}

# The RTL8812EU has no driver in the mainline kernel: build svpcom's for the running kernel
# (cached in ~/.cache/wfb-ground, rebuilt after a kernel update) and load it with the drone's
# options. An installed one (e.g. DKMS) is used as is.
load_rtl8812eu() {
    rtl8812eu_plugged || return 0
    [ -d /sys/module/8812eu ] && return 0
    local options="rtw_regd_src=1 rtw_tx_pwr_by_rate=0 rtw_tx_pwr_lmt_enable=0"
    if modinfo 8812eu >/dev/null 2>&1; then
        # shellcheck disable=SC2086
        sudo modprobe 8812eu $options
    else
        local kernel dir
        kernel=$(uname -r)
        dir=${XDG_CACHE_HOME:-$HOME/.cache}/wfb-ground/rtl8812eu-${RTL8812EU_COMMIT:0:7}-$kernel
        if [ ! -f "$dir/8812eu.ko" ]; then
            if [ ! -d "/lib/modules/$kernel/build" ]; then
                echo "Kernel headers missing for the RTL8812EU driver, see the top of $0." >&2
                exit 1
            fi
            echo "Building the RTL8812EU driver for kernel $kernel (once, in $dir)"
            rm -rf "$dir"
            git clone -q --single-branch -b $RTL8812EU_BRANCH $RTL8812EU_GIT "$dir"
            git -C "$dir" -c advice.detachedHead=false checkout -q $RTL8812EU_COMMIT
            if ! make -C "$dir" -j"$(nproc)" KSRC="/lib/modules/$kernel/build" KVER="$kernel" \
                KCFLAGS=-Wno-error modules >"$dir/build.log" 2>&1; then
                echo "RTL8812EU driver build failed, see $dir/build.log" >&2
                exit 1
            fi
        fi
        # With Secure Boot on, an unsigned module is refused ("Key was rejected by service").
        # shellcheck disable=SC2086
        sudo insmod "$dir/8812eu.ko" $options
    fi
    # Its interface shows up a moment later.
    local dev
    for _ in $(seq 1 20); do
        for dev in /sys/class/net/*; do
            [ "$(basename "$(readlink -f "$dev/device/driver")")" = rtl88x2eu ] && return 0
        done
        sleep 0.5
    done
}
load_rtl8812eu

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
# A card that was connected to a network can capture only a trickle in monitor mode afterwards
# (MT7925: about 5 instead of 70 frames per second), a fresh driver load clears that. Only for
# installed drivers, a hand loaded (insmod) one could not be loaded again.
DRIVER=$(basename "$(readlink -f "/sys/class/net/$IFACE/device/driver/module")")
if modinfo "$DRIVER" >/dev/null 2>&1 && sudo modprobe -r "$DRIVER" 2>/dev/null; then
    sudo modprobe "$DRIVER"
    for _ in $(seq 1 20); do
        [ -e "/sys/class/net/$IFACE" ] && break
        sleep 0.5
    done
fi
# Keep NetworkManager away from the adapter while it is in monitor mode.
if command -v nmcli >/dev/null && nmcli -t -f DEVICE device | grep -qx "$IFACE"; then
    sudo nmcli device set "$IFACE" managed no
    NM_MANAGED=yes
fi
# NetworkManager may still be busy with a freshly loaded card for a moment.
for _ in 1 2 3 4 5; do
    sudo ip link set "$IFACE" down
    sudo iw dev "$IFACE" set type monitor 2>/dev/null && break
    sleep 1
done
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
