SUMMARY = "Aicsemi aic8800 Wi-Fi driver and firmware"
DESCRIPTION = "Aic8800 Wi-Fi driver and firmware for Radxa Zero 3W"
LICENSE = "GPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464"

SRC_URI = "git://github.com/radxa-pkg/aic8800.git;branch=main;protocol=https"
SRCREV = "2bf2dc64bedaf3f0fcbcc206125afa5da8b3835b"

S = "${WORKDIR}/git"

inherit module
DEPENDS += "quilt-native"

DRIVER_SRC_DIR = "${S}/src/SDIO/driver_fw/driver/aic8800"

do_patch() {
    cd ${S}
    if [ "$(quilt top 2>/dev/null)" == "No patches applied" ] || [ -z "$(quilt top 2>/dev/null)" ]; then
        bbnote "Applying quilt patches..."
        export QUILT_PATCHES=debian/patches
        export QUILT_SERIES_FILE=debian/patches/series
        quilt push -a
    else
        bbnote "Source already patched. Skipping quilt push."
    fi
}

do_compile() {
    oe_runmake -C ${STAGING_KERNEL_DIR} M=${DRIVER_SRC_DIR} \
        KERNEL_VERSION=${KERNEL_VERSION} \
        modules
}

do_install() {
    install -d ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless
    install -m 0644 ${DRIVER_SRC_DIR}/aic8800_bsp/*.ko ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless
    install -m 0644 ${DRIVER_SRC_DIR}/aic8800_fdrv/*.ko ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless
    install -m 0644 ${DRIVER_SRC_DIR}/aic8800_btlpm/*.ko ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless

    install -d ${D}${nonarch_base_libdir}/firmware/aic8800_fw/SDIO/aic8800D80
    install -d ${D}${nonarch_base_libdir}/firmware/aic8800_fw/SDIO/aic8800
    install -m 0644 ${S}/src/SDIO/driver_fw/fw/aic8800D80/* ${D}${nonarch_base_libdir}/firmware/aic8800_fw/SDIO/aic8800D80/
    install -m 0644 ${S}/src/SDIO/driver_fw/fw/aic8800/* ${D}${nonarch_base_libdir}/firmware/aic8800_fw/SDIO/aic8800/
}

PACKAGES =+ "${PN}-firmware"

FILES:${PN} = "${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless/*.ko"
FILES:${PN}-firmware = "${nonarch_base_libdir}/firmware/aic8800_fw"

RRECOMMENDS:${PN} = "${PN}-firmware"

RPROVIDES:${PN} += "kernel-module-aic8800-bsp-${KERNEL_VERSION}"
RPROVIDES:${PN} += "kernel-module-aic8800-btlpm-${KERNEL_VERSION}"
RPROVIDES:${PN} += "kernel-module-aic8800-fdrv-${KERNEL_VERSION}"

COMPATIBLE_MACHINE = "radxa-zero-3w"
