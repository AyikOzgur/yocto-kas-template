#!/bin/bash
# Ground station for the wfb-ng link: puts a USB Wi-Fi adapter in monitor mode on the drone's
# channel, receives the video with wfb_rx and plays it with low latency: RTP (H.264 / H.265,
# streamer-service's default) or MPEG-TS (older images), the first packet tells. MAVLink goes
# both ways on the same adapter: QGroundControl connects by itself (UDP 14550), the adapter
# transmits its commands (only an adapter that can inject, e.g. RTL8812AU / RTL8812EU).
#
#   tools/wfb-ground.sh [interface]      default: the first USB Wi-Fi adapter
#
# Must match the drone (wfb-ng recipe / /var/wfb-video.env on the board):
#   WFB_CHANNEL     default 149 (5745 MHz)
#   WFB_PASSPHRASE  default wfb-video
# Transmit power of this adapter (MAVLink commands), in mBm like on the drone:
#   WFB_TXPOWER     default 1000 (10 dBm)
# Country code of the regulatory domain, e.g. PL. Needed to transmit (MAVLink commands) while
# the PC is not connected to any Wi-Fi network, see below:
#   WFB_REGION      default: unchanged
# Player:
#   PLAYER=ffplay   (default) or gst, or none to only receive on udp://127.0.0.1:5600
#                   (e.g. for QGroundControl or your own player)
#
# Needs sudo (monitor mode, raw capture), iw, python3, and wfb_rx / wfb_tx / wfb_keygen from
# wfb-ng:
#   sudo apt install libpcap-dev libsodium-dev
#   git clone -b release-25.01 https://github.com/svpcom/wfb-ng.git && cd wfb-ng
#   make wfb_rx wfb_tx wfb_keygen && sudo install -m 0755 wfb_rx wfb_tx wfb_keygen /usr/local/bin
# An RTL8812EU adapter (like the drone's) gets its driver built and loaded here, once per
# kernel, which needs git, a compiler and the kernel headers:
#   sudo apt install git build-essential linux-headers-$(uname -r)
# Fedora packages and the whole setup: docs/wifi-broadcast.md
set -euo pipefail

CHANNEL=${WFB_CHANNEL:-149}
PASSPHRASE=${WFB_PASSPHRASE:-wfb-video}
TXPOWER=${WFB_TXPOWER:-1000}
REGION=${WFB_REGION:-}
PLAYER=${PLAYER:-ffplay}
PORT=5600
# MAVLink: where QGroundControl listens, and the local ports between the relay and wfb-ng.
GCS_PORT=14550
MAVLINK_RX_PORT=14551
MAVLINK_TX_PORT=14552
MAVLINK_LOG=/tmp/wfb-ground-mavlink.log

# svpcom's RTL8812EU driver at the commit of the drone image (rtl8812eu-driver recipe).
RTL8812EU_GIT=https://github.com/svpcom/rtl8812eu.git
RTL8812EU_BRANCH=v5.15.0.1
RTL8812EU_COMMIT=48e6e449e089fa954e4e15079bd864039e2960da

for tool in iw python3 wfb_rx wfb_tx wfb_keygen; do
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
PIDS=()

cleanup() {
    [ ${#PIDS[@]} -gt 0 ] && sudo kill "${PIDS[@]}" 2>/dev/null || true
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
# Ctrl+C does not reach the receivers (background jobs of a script ignore it), the cleanup
# stops them with sudo: keep its authentication valid, or it asks for the password at the
# end, in the middle of their output.
(while sudo -n -v 2>/dev/null; do sleep 60; done) &
PIDS+=($!)
# Receivers of a run that could not clean up (e.g. its terminal was closed).
sudo pkill -f "wfb_[rt]x .* $IFACE\$" || true
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
# The kernel takes the country from the Wi-Fi network the PC is connected to. Without one it
# falls back to the world domain, where the 5.8 GHz channels are receive only: the video and
# the telemetry still arrive, but what this adapter should transmit is dropped without an
# error (MAVLink commands never reach the drone).
if [ -n "$REGION" ]; then
    sudo iw reg set "$REGION"
    sleep 1
fi
sudo iw dev "$IFACE" set channel "$CHANNEL" HT20
sudo iw dev "$IFACE" set txpower fixed "$TXPOWER" 2>/dev/null || true
PHY=$(basename "$(readlink -f "/sys/class/net/$IFACE/phy80211")")
FREQ=$(iw dev "$IFACE" info | sed -n 's/.*channel [0-9]* (\([0-9]*\) MHz).*/\1/p')
if iw phy "$PHY" channels | sed -n "/\* $FREQ MHz/,/Channel widths/p" | grep -qi "no ir"; then
    echo "Warning: channel $CHANNEL is receive only in the current regulatory domain" \
        "($(iw reg get | grep -m1 country | cut -d: -f1)): MAVLink commands will not be" \
        "transmitted. Set your country, e.g. WFB_REGION=PL $0" >&2
fi
# The adapter comes back in managed mode if it drops off USB meanwhile (loose cable, weak power).
if ! iw dev "$IFACE" info 2>/dev/null | grep -q 'type monitor'; then
    echo "$IFACE is not in monitor mode (did it drop off USB? see journalctl -k | grep 'USB disconnect')." >&2
    exit 1
fi
echo "Receiving on $IFACE, channel $CHANNEL -> udp://127.0.0.1:$PORT"

# Prints link statistics every second (packets, FEC recovered, lost, RSSI).
sudo wfb_rx -K "$KEYS/gs.key" -p 0 -c 127.0.0.1 -u $PORT "$IFACE" &
RX_PID=$!
PIDS+=("$RX_PID")

# MAVLink, the other end of wfb-mavlink-down / -up.service on the drone: telemetry from radio
# port 16 (0x10), commands on radio port 144 (0x90), every packet twice (FEC 1+1) at MCS 1.
# Their statistics go to $MAVLINK_LOG (written as the user, on purpose; both append, or one
# would overwrite the lines of the other).
: >"$MAVLINK_LOG"
# shellcheck disable=SC2024
sudo wfb_rx -K "$KEYS/gs.key" -p 16 -c 127.0.0.1 -u $MAVLINK_RX_PORT "$IFACE" >>"$MAVLINK_LOG" 2>&1 &
PIDS+=($!)
# shellcheck disable=SC2024
sudo wfb_tx -K "$KEYS/gs.key" -u $MAVLINK_TX_PORT -p 144 -B 20 -G long -S 0 -L 0 -M 1 -k 1 -n 2 "$IFACE" \
    >>"$MAVLINK_LOG" 2>&1 &
PIDS+=($!)
# QGroundControl answers to the address its telemetry comes from, which has to be one socket:
# telemetry from wfb_rx is passed on from it, and what comes back on it goes to wfb_tx.
python3 -c "
import os, select, socket
parent = os.getppid()
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(('127.0.0.1', $MAVLINK_RX_PORT))
gcs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
while True:
    if os.getppid() != parent:  # the script is gone
        break
    for s in select.select([rx, gcs], [], [], 1)[0]:
        data = s.recv(65535)
        gcs.sendto(data, ('127.0.0.1', $GCS_PORT if s is rx else $MAVLINK_TX_PORT))
" &
PIDS+=($!)
echo "MAVLink on udp://127.0.0.1:$GCS_PORT (QGroundControl), statistics in $MAVLINK_LOG"

# What the drone sends, from its first video packet: MPEG-TS (sync byte 0x47) or RTP
# (version 2) with payload type 96 (H.264) or 97 (H.265), streamer-service --udp-format.
VIDEO=ts
if [ "$PLAYER" = ffplay ] || [ "$PLAYER" = gst ]; then
    echo "Waiting for the first video packet..."
    VIDEO=$(python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', $PORT))
d = s.recv(65535)
if d[0] != 0x47 and d[0] >> 6 == 2:
    print('h265' if d[1] & 0x7f == 97 else 'h264')
else:
    print('ts')
")
    echo "Video: $([ "$VIDEO" = ts ] && echo MPEG-TS || echo "RTP $VIDEO")"
fi
RTP_PT=96
RTP_CODEC=H264
[ "$VIDEO" = h265 ] && RTP_PT=97 && RTP_CODEC=H265
# ffmpeg needs an SDP file to know what a dynamic RTP payload type carries.
printf 'v=0\no=- 0 0 IN IP4 127.0.0.1\ns=wfb-ng\nc=IN IP4 127.0.0.1\nt=0 0\nm=video %s RTP/AVP %s\na=rtpmap:%s %s/90000\n' \
    $PORT $RTP_PT $RTP_PT $RTP_CODEC >"$KEYS/video.sdp"

case "$PLAYER" in
ffplay)
    # No probing and no buffering; setpts=0 shows every frame as soon as it is decoded
    # instead of scheduling it by its timestamp.
    # A hidden (minimized) window is not drawn, so the player stops taking frames. Without
    # limits it keeps everything that arrives meanwhile and replays minutes of old video
    # when the window comes back. -noinfbuf stops its reading after about a second of queued
    # video, fifo_size=0 turns off ffmpeg's own 5 MB UDP buffer: the rest is dropped by the
    # kernel (socket buffer of buffer_size bytes, about a second), and the picture is live
    # again within about two seconds.
    PLAY_ARGS=(-hide_banner -loglevel warning -fflags nobuffer -flags low_delay -framedrop -noinfbuf
        -probesize 32 -analyzeduration 0 -vf setpts=0 -window_title "wfb-ng channel $CHANNEL")
    if [ "$VIDEO" = ts ]; then
        ffplay "${PLAY_ARGS[@]}" "udp://127.0.0.1:$PORT?fifo_size=0&buffer_size=262144"
    else
        # RTP: no reordering queue, a late packet is lost anyway.
        ffplay "${PLAY_ARGS[@]}" -protocol_whitelist file,udp,rtp -reorder_queue_size 0 \
            -buffer_size 262144 "$KEYS/video.sdp"
    fi
    ;;
gst)
    if [ "$VIDEO" = ts ]; then
        # tsdemux adds 700 ms by default.
        gst-launch-1.0 -q udpsrc port=$PORT ! tsdemux latency=0 ! decodebin ! videoconvert ! \
            autovideosink sync=false
    else
        gst-launch-1.0 -q udpsrc port=$PORT \
            caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=$RTP_CODEC,payload=$RTP_PT" ! \
            "rtp${VIDEO}depay" ! decodebin ! videoconvert ! autovideosink sync=false
    fi
    ;;
*)
    wait "$RX_PID"
    ;;
esac
