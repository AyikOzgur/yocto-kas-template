# Original BSP layer has outdated commit. Override it with proper one.
SRCREV:radxa-zero-3 = "66715ef294b0d436e13bbb9e855fe17e296de7a8"
SRCREV:radxa-zero-3:rk-u-boot-env = "66715ef294b0d436e13bbb9e855fe17e296de7a8"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:rk-u-boot-env = " file://bootcount.cfg"
# meta-rockchip overrides SRC_URI for radxa-zero-3, which drops += additions.
SRC_URI:append = " ${@oe.utils.conditional('UBOOT_SIGN_ENABLE', '1', 'file://secureboot.cfg', '', d)}"

# Re-embed the public key when the signing key changes or first appears.
do_uboot_assemble_fitimage[file-checksums] += "${UBOOT_SIGN_KEYDIR}/${UBOOT_SIGN_KEYNAME}.crt:${@os.path.exists('${UBOOT_SIGN_KEYDIR}/${UBOOT_SIGN_KEYNAME}.crt')}"

# SPL loads u-boot.itb, a FIT that binman packs from U-Boot, TF-A and the board
# DTB. The stock concat_dtb() overwrites it with a raw u-boot-nodtb.bin +
# u-boot.dtb blob that SPL can't load, so instead rebuild with the public key
# added to the DTB that binman packs.
concat_dtb:rockchip() {
	# mkimage silently skips signing when the key is missing.
	if [ ! -f "${UBOOT_SIGN_KEYDIR}/${UBOOT_SIGN_KEYNAME}.key" ] || \
	   [ ! -f "${UBOOT_SIGN_KEYDIR}/${UBOOT_SIGN_KEYNAME}.crt" ]; then
		bbfatal "Signing key ${UBOOT_SIGN_KEYDIR}/${UBOOT_SIGN_KEYNAME}.key/.crt not found"
	fi

	dt=$(sed -n 's/^CONFIG_DEFAULT_DEVICE_TREE="\(.*\)"$/\1/p' .config)

	cp arch/arm/dts/$dt.dtb ${UBOOT_DTB_SIGNED}
	${UBOOT_MKIMAGE_SIGN} \
		-f auto-conf \
		-k "${UBOOT_SIGN_KEYDIR}" \
		-o "${FIT_HASH_ALG},${FIT_SIGN_ALG}" \
		-g "${UBOOT_SIGN_KEYNAME}" \
		-K ${UBOOT_DTB_SIGNED} \
		-d /dev/null \
		-r ${B}/unused.itb \
		${UBOOT_MKIMAGE_SIGN_ARGS}
	${UBOOT_FIT_CHECK_SIGN} -k ${UBOOT_DTB_SIGNED} -f ${B}/unused.itb

	# binman picks up <dt>.dtb from the build root before arch/arm/dts.
	cp ${UBOOT_DTB_SIGNED} $dt.dtb
	unset LDFLAGS CFLAGS CPPFLAGS
	oe_runmake -C ${S} O=${B} EXT_DTB=${B}/${UBOOT_DTB_SIGNED} ${UBOOT_MAKE_TARGET}
}