DESCRIPTION = "Mavlink router"
LICENSE = "CLOSED"

SRC_URI = "gitsm://git@github.com/mavlink-router/mavlink-router.git;protocol=ssh;branch=master \
           file://mavlink-router.service"

# Follow latest main while testing, pin to a commit for releases.
SRCREV = "${AUTOREV}"
PV = "1.0+git"

S = "${WORKDIR}/git"

# UART device where FC is connected.
UART_DEVICE ?= "/dev/ttyS4"

inherit meson systemd pkgconfig
EXTRA_OEMESON += "-Dsystemdsystemunitdir=${systemd_system_unitdir}"


SYSTEMD_SERVICE:${PN} = "mavlink-router.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_configure:prepend () {
    # Don't build unit tests; meson would otherwise pick up the host's /usr/src/gtest
    sed -i "s|^dep_gtest = dependency('gtest'.*|dep_gtest = dependency('', required : false)|" ${S}/meson.build
}


do_install:append () {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mavlink-router.service ${D}${systemd_unitdir}/system

    sed -i \
    -e 's|@@UART_DEVICE@@|${UART_DEVICE}|g' \
    ${D}${systemd_unitdir}/system/mavlink-router.service
}

FILES:${PN} += "${systemd_unitdir}/system/mavlink-router.service"
