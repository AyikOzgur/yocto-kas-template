DESCRIPTION = "ISP tuning: black level, color matrix, gamma, auto white balance and auto exposure for the Rockchip ISP (RK356x, vendor kernel), without rkaiq"
LICENSE = "CLOSED"

SRC_URI = "file://main.cpp \
           file://isp-tuning-service.service \
"

S = "${WORKDIR}"

# The Rockchip ISP uapi headers come from the vendor kernel, not from linux-libc-headers.
COMPATIBLE_MACHINE = "radxa-zero-3w-vendor"
PACKAGE_ARCH = "${MACHINE_ARCH}"
do_compile[depends] += "virtual/kernel:do_shared_workdir"

inherit systemd

do_compile() {
    # Only the Rockchip headers, the generic linux/ ones stay those of the libc.
    install -d ${B}/include/linux
    for h in rk-isp2-config.h rk-isp21-config.h rk-camera-module.h rk-video-format.h; do
        install -m 0644 ${STAGING_KERNEL_DIR}/include/uapi/linux/$h ${B}/include/linux/
    done
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 -I${B}/include ${S}/main.cpp -o ${B}/isp-tuning-service
}

do_install() {
    install -D -m 0755 ${B}/isp-tuning-service ${D}${bindir}/isp-tuning-service

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/isp-tuning-service.service ${D}${systemd_unitdir}/system
    sed -i -e 's|@@BINDIR@@|${bindir}|g' ${D}${systemd_unitdir}/system/isp-tuning-service.service
}

SYSTEMD_SERVICE:${PN} = "isp-tuning-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${systemd_unitdir}/system/isp-tuning-service.service"
