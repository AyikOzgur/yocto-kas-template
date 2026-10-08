SUMMARY = "Radxa vendor kernel (Rockchip BSP 6.1) for the Radxa ZERO 3W"
DESCRIPTION = "Rockchip BSP kernel as shipped by Radxa. Used for the hardware \
H.264/H.265 encoder (mpp_service/rkvenc) and the built-in RKNPU driver. The GPU \
runs on panfrost instead of the Mali kbase driver, so Mesa panvk keeps working."
SECTION = "kernel"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

# Branch linux-6.1-stan-rkr5.1. A tarball instead of a git clone: the repository
# history is huge and we only need one commit.
RADXA_KERNEL_COMMIT = "f87fca6cefcb6229c7f81399dd351cf658940bfa"
SRC_URI = " \
    https://github.com/radxa/kernel/archive/${RADXA_KERNEL_COMMIT}.tar.gz;downloadfilename=radxa-kernel-${RADXA_KERNEL_COMMIT}.tar.gz \
    file://panfrost.cfg \
    file://cma-heap.cfg \
    file://radxa-zero-3w-vendor.dtsi \
"
SRC_URI[sha256sum] = "5f4dd2214520b73b864ff486e2f78b528f2078d47285de50a4fac28ab81806d3"

FILESEXTRAPATHS:prepend := "${THISDIR}/files/vendor:"

LINUX_VERSION = "6.1"
PV = "${LINUX_VERSION}+git"
KERNEL_VERSION_SANITY_SKIP = "1"

# Start from Radxa's in-tree defconfig, the .cfg files in SRC_URI are merged on top.
KBUILD_DEFCONFIG = "rockchip_linux_defconfig"
KCONFIG_MODE = "alldefconfig"
# meta-rockchip trims mainline configs with its own kernel metadata, which this tree
# doesn't use (and the vendor defconfig is Rockchip-only anyway).
KERNEL_FEATURES:remove = "bsp/rockchip/remove-non-rockchip-arch-arm64.scc"

inherit kernel
inherit kernel-yocto
require recipes-kernel/linux/linux-yocto.inc

# kernel.bbclass builds from S = STAGING_KERNEL_DIR and only moves a git checkout
# there. Move the extracted tarball instead; kernel-yocto then turns it into a git tree.
TARBALL_DIR = "${WORKDIR}/kernel-${RADXA_KERNEL_COMMIT}"
do_kernel_checkout:prepend() {
    if [ -d ${TARBALL_DIR} ]; then
        rm -rf ${S}
        mv ${TARBALL_DIR} ${S}
    fi
}

DTS_FILE = "${S}/arch/arm64/boot/dts/rockchip/rk3566-radxa-zero-3w-aic8800ds2.dts"

do_configure:append() {
    # Append our board fragment to the vendor device tree, once.
    if ! grep -q "BEGIN radxa-zero-3w-vendor.dtsi" ${DTS_FILE}; then
        echo "/* BEGIN radxa-zero-3w-vendor.dtsi */" >> ${DTS_FILE}
        cat ${WORKDIR}/radxa-zero-3w-vendor.dtsi >> ${DTS_FILE}
    fi
}

# Same A/B boot layout as the mainline kernel.
do_deploy:append() {
    if [ -e ${DEPLOYDIR}/fitImage ]; then
        install -m 0644 ${DEPLOYDIR}/fitImage ${DEPLOYDIR}/fitImageA
        install -m 0644 ${DEPLOYDIR}/fitImage ${DEPLOYDIR}/fitImageB
    fi
}

COMPATIBLE_MACHINE = "radxa-zero-3w-vendor"
