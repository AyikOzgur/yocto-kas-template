DESCRIPTION = "V4L2 capture service, shares dma-buf frames over unix socket"
LICENSE = "CLOSED"

SRC_URI = "git://git@github.com/AyikOzgur/camera-service.git;protocol=ssh;branch=main \
           file://camera-service.service"

# Follow latest main while testing, pin to a commit for releases.
SRCREV = "${AUTOREV}"
PV = "1.0+git"

S = "${WORKDIR}/git"

# Capture device, can be overridden from local.conf / kas.
CAMERA_VIDEO_DEVICE ?= "/dev/video2"

inherit cmake systemd

SYSTEMD_SERVICE:${PN} = "camera-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append () {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/camera-service.service ${D}${systemd_unitdir}/system

    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@VIDEO_DEVICE@@|${CAMERA_VIDEO_DEVICE}|g' \
    ${D}${systemd_unitdir}/system/camera-service.service
}

FILES:${PN} += "${systemd_unitdir}/system/camera-service.service"
