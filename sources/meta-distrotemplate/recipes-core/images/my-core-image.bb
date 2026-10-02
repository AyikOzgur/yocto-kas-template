inherit core-image
CORE_IMAGE_EXTRA_INSTALL += "htop v4l-utils python3 update-status"

IMAGE_FEATURES += "read-only-rootfs"

VERITY_SALT = "ec5b3a46a036dc0a78c192edbb8b9b98e242b108f379d53c2b3d18541a4c58da"
inherit verity-ab

SWUPDATE_IMAGES = "my-core-image fitImage"
inherit swupdate-image
SWUPDATE_IMAGES_FSTYPES[my-core-image] = ".ext4.verity.gz"
