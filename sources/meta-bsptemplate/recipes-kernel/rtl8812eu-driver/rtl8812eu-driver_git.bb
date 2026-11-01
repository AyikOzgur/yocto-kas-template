SUMMARY = "Realtek RTL8812EU USB Wi-Fi driver with monitor mode and injection"
DESCRIPTION = "svpcom's RTL8812EU/RTL8822EU driver for wfb-ng (WiFi broadcast). \
The card comes up as wfb0, so it never takes wlan0 from the onboard Wi-Fi \
(udev keeps that name with wfb-ng's 10-wfb0.link)."
HOMEPAGE = "https://github.com/svpcom/rtl8812eu"
LICENSE = "GPL-2.0-only"
# No license file in the repository, use the header of a source file.
LIC_FILES_CHKSUM = "file://os_dep/linux/os_intfs.c;beginline=1;endline=14;md5=9925fbb78837e87efa0522d118989766"

SRC_URI = "git://github.com/svpcom/rtl8812eu.git;protocol=https;branch=v5.15.0.1"
SRCREV = "48e6e449e089fa954e4e15079bd864039e2960da"
PV = "5.15.0.1+git"

S = "${WORKDIR}/git"

inherit module

do_compile() {
    # The driver's own Makefile (it needs its directory as TopDIR), generic arm64
    # platform instead of the default x86 PC one. The vendor kernel builds with
    # CONFIG_WERROR, the Realtek code has warnings (KCFLAGS come last, so this wins).
    oe_runmake -C ${S} \
        CONFIG_PLATFORM_I386_PC=n CONFIG_PLATFORM_ARM64_RPI=y \
        KSRC=${STAGING_KERNEL_DIR} KVER=${KERNEL_VERSION} \
        KCFLAGS=-Wno-error \
        modules
}

do_install() {
    install -D -m 0644 ${S}/8812eu.ko \
        ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless/8812eu.ko

    # Interface name wfb0, regulatory domain from the OS, and tx power fully set by
    # "iw dev wfb0 set txpower fixed <mBm>" (no per rate tables or limits).
    install -d ${D}${sysconfdir}/modprobe.d
    echo "options 8812eu ifname=wfb%d rtw_regd_src=1 rtw_tx_pwr_by_rate=0 rtw_tx_pwr_lmt_enable=0" \
        > ${D}${sysconfdir}/modprobe.d/8812eu.conf
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/modules/${KERNEL_VERSION}/kernel/drivers/net/wireless/8812eu.ko \
    ${sysconfdir}/modprobe.d/8812eu.conf \
"

RPROVIDES:${PN} += "kernel-module-8812eu-${KERNEL_VERSION}"

# The video link (MPP encoder) is vendor kernel only anyway.
COMPATIBLE_MACHINE = "radxa-zero-3w-vendor"
