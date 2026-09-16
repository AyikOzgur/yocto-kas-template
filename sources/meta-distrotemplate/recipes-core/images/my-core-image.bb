inherit core-image
CORE_IMAGE_EXTRA_INSTALL += "htop v4l-utils python3 update-status"

IMAGE_FSTYPES += "ext4.gz"
inherit swupdate-image
SWUPDATE_IMAGES_FSTYPES[my-core-image] = ".ext4.gz"