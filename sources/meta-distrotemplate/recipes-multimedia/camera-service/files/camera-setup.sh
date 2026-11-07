#!/bin/sh
# Camera setup from the camera preset (environment), by camera-setup.service before anything
# opens the camera: isp-tuning-service reads the sensor mode, the sensor controls below are
# locked while streaming.
#   CAMERA_ROTATION      0 or 180 degrees
#   CAMERA_SENSOR_SIZE   sensor mode for the Rockchip ISP, e.g. 1280x720; empty: keep the current one
ROTATION=${CAMERA_ROTATION:-0}
SENSOR_SIZE=${CAMERA_SENSOR_SIZE:-}

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

# Rockchip ISP (vendor kernel). The ISP only takes the sensor format when it powers up, and
# isp-tuning-service can keep it powered: set the sensor mode, then the ISP input to the
# sensor format (its Bayer order follows the flips) and the ISP output to the same size.
# YUV output in limited (TV) range, like the USB grabber: the overlay colors, the inference
# preprocessing and the encoder all expect it, the ISP defaults to full range.
command -v media-ctl >/dev/null || exit 0
ISP='"rkisp-isp-subdev"'
# Format of a pad on media device $dev, e.g. SRGGB10_1X10/1280x720, without the frame interval
# media-ctl adds for sensors (@10000/600000): the ISP has none, setting one fails.
fmt() { media-ctl -d "$dev" --get-v4l2 "$1" 2>/dev/null | sed -n 's/.*fmt:\([^] @]*\).*/\1/p'; }
for dev in /dev/media*; do
    [ -e "$dev" ] || continue
    media-ctl -d "$dev" --get-v4l2 "$ISP:0" >/dev/null 2>&1 || continue

    # Entity name of the sensor, e.g. "m00_b_imx219 2-0010".
    sensor=$(media-ctl -d "$dev" -p 2>/dev/null | awk '
        /^- entity [0-9]+: / { name = $0; sub(/^- entity [0-9]+: /, "", name); sub(/ \([0-9]+ pads?,.*$/, "", name) }
        /subtype Sensor/ { print name; exit }')
    [ -n "$sensor" ] || continue
    SENSOR="\"$sensor\":0"

    if [ -n "$SENSOR_SIZE" ]; then
        # On the sensor's own node: media-ctl would also pass the format on to the CSI D-PHY,
        # which has no format to set. The driver picks its closest mode and its Bayer order.
        node=$(media-ctl -d "$dev" -e "$sensor")
        v4l2-ctl -d "$node" --set-subdev-fmt "pad=0,width=${SENSOR_SIZE%x*},height=${SENSOR_SIZE#*x}"
    fi
    sensor_fmt=$(fmt "$SENSOR")
    size=${sensor_fmt#*/}
    echo "Sensor $sensor: $sensor_fmt"
    media-ctl -d "$dev" --set-v4l2 "$ISP:0[fmt:$sensor_fmt crop:(0,0)/$size]"
    media-ctl -d "$dev" --set-v4l2 "$ISP:2[fmt:YUYV8_2X8/$size crop:(0,0)/$size quantization:lim-range]"
done
exit 0
