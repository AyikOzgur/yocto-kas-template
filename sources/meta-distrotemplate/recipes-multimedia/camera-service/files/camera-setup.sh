#!/bin/sh
# Runs before camera-service starts (the sensor controls below are locked while streaming).
#   camera-setup.sh <rotation>     rotation: 0 or 180 degrees
ROTATION=${1:-0}

# 180 degrees = horizontal + vertical flip in the sensor, free and adds no latency.
# Cameras without flip controls (most USB grabbers) are left alone.
if command -v v4l2-ctl >/dev/null; then
    if [ "$ROTATION" = 180 ]; then flip=1; else flip=0; fi
    for dev in /dev/v4l-subdev*; do
        [ -e "$dev" ] || continue
        v4l2-ctl -d "$dev" --list-ctrls 2>/dev/null | grep -q horizontal_flip || continue
        v4l2-ctl -d "$dev" --set-ctrl "horizontal_flip=$flip,vertical_flip=$flip"
    done
fi

# Rockchip ISP (vendor kernel): YUV output in limited (TV) range, like the USB grabber.
# The overlay colors, the inference preprocessing and the encoder all expect it, the ISP
# defaults to full range.
if command -v media-ctl >/dev/null; then
    PAD='"rkisp-isp-subdev":2'
    for dev in /dev/media*; do
        [ -e "$dev" ] || continue
        fmt=$(media-ctl -d "$dev" --get-v4l2 "$PAD" 2>/dev/null | sed -n 's/.*fmt:\([^ ]*\).*/\1/p')
        [ -n "$fmt" ] || continue
        media-ctl -d "$dev" --set-v4l2 "$PAD[fmt:$fmt quantization:lim-range]"
    done
fi
exit 0
