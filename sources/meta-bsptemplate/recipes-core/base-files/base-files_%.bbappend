# rockchip_ab.wks writes the root slots as raw dm-verity images, so wic
# cannot add the mounts for its other partitions to them.
do_install:append() {
	cat >> ${D}${sysconfdir}/fstab <<EOF
/dev/mmcblk1p9       /boot                vfat       defaults              0  0
/dev/mmcblk1p12      /var                 ext4       defaults              0  0
EOF
}
