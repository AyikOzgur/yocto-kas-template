DESCRIPTION = "Vulkan YUYV to RGB letterbox preprocessing service"
LICENSE = "CLOSED"

SRC_URI = "git://git@github.com/AyikOzgur/preprocessing-service.git;protocol=ssh;branch=main \
           file://preprocessing-service.service"

# Follow latest main while testing, pin to a commit for releases.
SRCREV = "${AUTOREV}"
PV = "1.0+git"

S = "${WORKDIR}/git"

DEPENDS = "vulkan-loader vulkan-headers glslang-native"
# panvk vulkan driver from mesa.
RDEPENDS:${PN} += "mesa-vulkan-drivers"

inherit cmake systemd

# OpenCV window is for desktop testing only.
EXTRA_OECMAKE = "-DWITH_OPENCV=OFF -DSHADER_PATH=${datadir}/preprocessing-service/yuyv_to_rgb.spv"

SYSTEMD_SERVICE:${PN} = "preprocessing-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append () {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/preprocessing-service.service ${D}${systemd_unitdir}/system

    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    ${D}${systemd_unitdir}/system/preprocessing-service.service
}

FILES:${PN} += "${systemd_unitdir}/system/preprocessing-service.service"
