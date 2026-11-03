DESCRIPTION = "V4L2 capture service, shares dma-buf frames over unix socket"
LICENSE = "CLOSED"

SRC_URI = "git://git@github.com/AyikOzgur/camera-service.git;protocol=ssh;branch=radxa-camera-support \
           file://camera-service.service \
           file://camera-setup.sh \
           file://90-rkisp-camera.rules \
           file://cameras/csi-imx462.env \
           file://cameras/usb-grabber.env \
"

# Follow the branch while testing, pin to a commit for releases.
SRCREV = "${AUTOREV}"
PV = "1.0+git"

S = "${WORKDIR}/git"

# Default camera, one of the presets in files/cameras/ (installed to /etc/camera/).
# Can be overridden from local.conf / kas, or at runtime (see the unit file).
CAMERA ?= "usb-grabber"

inherit cmake systemd

SYSTEMD_SERVICE:${PN} = "camera-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append () {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/camera-service.service ${D}${systemd_unitdir}/system
    sed -i -e 's|@@BINDIR@@|${bindir}|g' ${D}${systemd_unitdir}/system/camera-service.service

    install -d ${D}${sysconfdir}/camera
    install -m 0644 ${WORKDIR}/cameras/*.env ${D}${sysconfdir}/camera/
    if [ ! -e ${D}${sysconfdir}/camera/${CAMERA}.env ]; then
        bbfatal "CAMERA = \"${CAMERA}\" has no preset in files/cameras/"
    fi
    ln -sf ${CAMERA}.env ${D}${sysconfdir}/camera/default.env

    install -m 0755 ${WORKDIR}/camera-setup.sh ${D}${bindir}
    install -D -m 0644 ${WORKDIR}/90-rkisp-camera.rules ${D}${sysconfdir}/udev/rules.d/90-rkisp-camera.rules
}

FILES:${PN} += "${systemd_unitdir}/system/camera-service.service"
