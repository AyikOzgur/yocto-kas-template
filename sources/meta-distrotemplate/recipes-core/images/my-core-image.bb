inherit core-image
CORE_IMAGE_EXTRA_INSTALL += "htop v4l-utils python3 update-status"
CORE_IMAGE_EXTRA_INSTALL += "camera-service preprocessing-service object-detection-service"
CORE_IMAGE_EXTRA_INSTALL += "overlay-service"
# Hardware H.264/H.265 encoding through MPP needs the vendor kernel.
CORE_IMAGE_EXTRA_INSTALL:append:radxa-zero-3w-vendor = " streamer-service"
# Video over WiFi broadcast on an RTL8812EU USB card (wfb0).
CORE_IMAGE_EXTRA_INSTALL:append:radxa-zero-3w-vendor = " rtl8812eu-driver wfb-ng"
# media-ctl: Rockchip ISP / camera sensor setup (separate package of v4l-utils).
CORE_IMAGE_EXTRA_INSTALL:append:radxa-zero-3w-vendor = " media-ctl"
CORE_IMAGE_EXTRA_INSTALL += "mavlink-router"

IMAGE_FEATURES += "read-only-rootfs"

SWUPDATE_IMAGES = "my-core-image fitImage"
IMAGE_FSTYPES += "ext4.gz"
inherit swupdate-image
SWUPDATE_IMAGES_FSTYPES[my-core-image] = ".ext4.gz"