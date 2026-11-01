#!/bin/sh
# Video over WiFi broadcast: puts the USB card (wfb0) in monitor mode on a fixed
# channel and injects every UDP datagram streamer-service sends to port 5600.
# One way, no association, no retransmissions; FEC (8 data + 4 parity packets)
# repairs losses instead.
set -e

IFACE=wfb0
KEYS=/run/wfb

# drone.key + gs.key, the ground station derives the same pair from the passphrase.
mkdir -p $KEYS
cd $KEYS
wfb_keygen "$WFB_PASSPHRASE"

if [ -n "$WFB_REGION" ]; then
    iw reg set "$WFB_REGION"
fi
# Monitor mode once per plug-in only: taking the interface down again (service
# restart) leaves the 8812eu driver dropping every injected packet until reload.
if ! iw dev $IFACE info | grep -q "type monitor"; then
    ip link set $IFACE down
    iw dev $IFACE set type monitor
fi
ip link set $IFACE up
# The first channel set after plug-in only reaches the driver, the radio stays on its
# default (nothing on air). Tune to another channel first, then to the real one.
if [ "$WFB_CHANNEL" = 1 ]; then OTHER=6; else OTHER=1; fi
iw dev $IFACE set channel $OTHER HT20
iw dev $IFACE set channel "$WFB_CHANNEL" HT20
iw dev $IFACE set txpower fixed "$WFB_TXPOWER"

# 20 MHz, long guard interval, no STBC / LDPC: any single antenna receiver can
# decode it. -T 2: close a FEC block 2 ms after the last packet of
# a frame, so a loss at the end of a frame is repaired without waiting for the next one.
exec wfb_tx -K $KEYS/drone.key -u 5600 -p 0 -B 20 -G long -S 0 -L 0 -M "$WFB_MCS" \
    -k 8 -n 12 -T 2 $IFACE
