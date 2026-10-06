SUMMARY = "Rockchip RKNPU kernel driver"
DESCRIPTION = "Out-of-tree port of Rockchip's RKNPU driver (v0.9.8) to mainline Linux. \
Provides the DRM device used by librknnrt on RK3566."
LICENSE = "GPL-2.0-only"
# Repository has no LICENSE file; use the SPDX header of the main source.
LIC_FILES_CHKSUM = "file://src/rknpu_drv.c;beginline=1;endline=1;md5=50d2ba0afecd20f74c12a4bdbcfcfe61"

SRC_URI = "git://github.com/w568w/rknpu-module.git;branch=main;protocol=https \
           file://0001-Support-building-against-Linux-6.11.patch \
           file://0002-Map-cacheable-buffers-cacheable-without-IOMMU.patch"
SRCREV = "a8792fe6b633f90cf2c6808267cac327537ab4cf"

PV = "0.9.8+git"
S = "${WORKDIR}/git"

inherit module

do_compile() {
    # Strip TMPDIR from __FILE__ (buildpaths QA).
    oe_runmake -C ${STAGING_KERNEL_DIR} M=${S} \
        KERNEL_VERSION=${KERNEL_VERSION} \
        KCFLAGS="-ffile-prefix-map=${S}/=" \
        modules
}

do_install() {
    install -d ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra
    install -m 0644 ${S}/rknpu.ko ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra
}

FILES:${PN} = "${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra/rknpu.ko"

RPROVIDES:${PN} += "kernel-module-rknpu-${KERNEL_VERSION}"

# DWARF references the kernel tree in work-shared; only affects the -dbg package.
INSANE_SKIP:${PN}-dbg += "buildpaths"

COMPATIBLE_MACHINE = "radxa-zero-3w"
