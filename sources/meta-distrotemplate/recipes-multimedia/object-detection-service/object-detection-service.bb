DESCRIPTION = "Object detection service that supports multiple backends"
LICENSE = "CLOSED"


SRC_URI = " \
    git://git@github.com/AyikOzgur/object-detection-service.git;protocol=ssh;branch=main \
    https://raw.githubusercontent.com/Qengineering/YoloV5-NPU/main/rk3566/yolov5s.rknn;name=yolov5s \
    https://raw.githubusercontent.com/Qengineering/YoloV8-NPU/main/rk3566/yolov8n.rknn;name=yolov8n \
    file://object-detection-service.service \
"

SRCREV = "a6406c7ca48463ddf729b4f211a6806297aca3fb"

PV = "1.0+git"

SRC_URI[yolov5s.sha256sum] = "084997571ff4d3b1cc98567c442f45ea94d377cc4829cdbed1b20af893f4efc7"
SRC_URI[yolov8n.sha256sum] = "953dae6fcfd21d10f864d8c552f20c3f91ce51f795b0d604c87bc17d8c9430f1"

S = "${WORKDIR}/git"
DEPENDS = "librknnrt"

# Default detection confidence threshold, can be overridden from local.conf / kas,
# or at runtime with /var/inference-service.env (see the unit file).
INFERENCE_CONF ?= "0.45"

inherit cmake systemd 

SYSTEMD_SERVICE:${PN} = "object-detection-service.service"
SYSTEMD_AUTO_ENABLE = "enable"


do_install:append() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/object-detection-service.service ${D}${systemd_unitdir}/system

    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@DATADIR@@|${datadir}|g' \
    -e 's|@@INFERENCE_CONF@@|${INFERENCE_CONF}|g' \
    ${D}${systemd_unitdir}/system/object-detection-service.service

    install -d ${D}${datadir}/object-detection-service
    install -m 0644 ${WORKDIR}/yolov5s.rknn ${WORKDIR}/yolov8n.rknn ${D}${datadir}/object-detection-service
}

FILES:${PN} += "${systemd_unitdir}/system/object-detection-service.service"

# librknnrt is radxa-zero-3w only.
COMPATIBLE_MACHINE = "radxa-zero-3w"
