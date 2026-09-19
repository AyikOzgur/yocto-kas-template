#!/bin/sh
echo 1 > /sys/module/aic8800_fdrv/parameters/aicwf_dbg_level
ip link set wlan0 up
wpa_passphrase "@@WIFI_NAME@@" "@@WIFI_PASSWD@@" > /etc/wpa_supplicant.conf

wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf

sleep 5

udhcpc -i wlan0