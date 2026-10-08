DESCRIPTION = "Overlay service"
LICENSE = "CLOSED"

SRC_URI = "git://git@github.com/AyikOzgur/overlay-service.git;protocol=ssh;branch=main \
           file://overlay-service.service"

# Follow latest main while testing, pin to a commit for releases.
SRCREV = "${AUTOREV}"
PV = "1.0+git"

S = "${WORKDIR}/git"

DEPENDS = "vulkan-loader vulkan-headers glslang-native"
# panvk vulkan driver from mesa.
RDEPENDS:${PN} += "mesa-vulkan-drivers"


inherit cmake systemd

SYSTEMD_SERVICE:${PN} = "overlay-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append () {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/overlay-service.service ${D}${systemd_unitdir}/system

    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    ${D}${systemd_unitdir}/system/overlay-service.service
}

FILES:${PN} += "${systemd_unitdir}/system/overlay-service.service"
