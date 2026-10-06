inherit core-image
CORE_IMAGE_EXTRA_INSTALL += "htop v4l-utils python3 update-status"
CORE_IMAGE_EXTRA_INSTALL += "camera-service preprocessing-service inference-service"

IMAGE_FEATURES += "read-only-rootfs"

SWUPDATE_IMAGES = "my-core-image fitImage"
IMAGE_FSTYPES += "ext4.gz"
inherit swupdate-image
SWUPDATE_IMAGES_FSTYPES[my-core-image] = ".ext4.gz"