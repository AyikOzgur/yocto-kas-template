inherit core-image
CORE_IMAGE_EXTRA_INSTALL += "htop v4l-utils python3 update-status"

IMAGE_FEATURES += "read-only-rootfs"

SWUPDATE_IMAGES = "my-core-image fitImage"
IMAGE_FSTYPES += "ext4.gz verity"
inherit swupdate-image
SWUPDATE_IMAGES_FSTYPES[my-core-image] = ".ext4.gz"

# Include the dm-verity image generation class
IMAGE_CLASSES += "image_types_verity"
# Define target image parameters
#DM_VERITY_IMAGE = "my-core-image"
#DM_VERITY_IMAGE_TYPE = "ext4"