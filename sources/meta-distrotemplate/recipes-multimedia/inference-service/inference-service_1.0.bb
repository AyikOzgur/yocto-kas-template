DESCRIPTION = "NPU inference test service, runs YOLO on preprocessing-service frames"
LICENSE = "CLOSED"

# RK3566 models from Q-engineering YoloV5-NPU / YoloV8-NPU (BSD-3-Clause).
# The service runs yolov8n; yolov5s is kept for comparison.
SRC_URI = "file://inference-service.cpp \
           file://inference-service.service \
           https://raw.githubusercontent.com/Qengineering/YoloV5-NPU/main/rk3566/yolov5s.rknn;name=yolov5s \
           https://raw.githubusercontent.com/Qengineering/YoloV8-NPU/main/rk3566/yolov8n.rknn;name=yolov8n \
"
SRC_URI[yolov5s.sha256sum] = "084997571ff4d3b1cc98567c442f45ea94d377cc4829cdbed1b20af893f4efc7"
SRC_URI[yolov8n.sha256sum] = "953dae6fcfd21d10f864d8c552f20c3f91ce51f795b0d604c87bc17d8c9430f1"

S = "${WORKDIR}"

DEPENDS = "librknnrt"

inherit systemd

do_compile() {
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 \
        ${S}/inference-service.cpp -o ${B}/inference-service -lrknnrt
}

do_install() {
    install -D -m 0755 ${B}/inference-service ${D}${bindir}/inference-service
    install -d ${D}${datadir}/inference-service
    install -m 0644 ${S}/yolov5s.rknn ${S}/yolov8n.rknn ${D}${datadir}/inference-service

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/inference-service.service ${D}${systemd_unitdir}/system
    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@DATADIR@@|${datadir}|g' \
    ${D}${systemd_unitdir}/system/inference-service.service
}

SYSTEMD_SERVICE:${PN} = "inference-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${datadir}/inference-service ${systemd_unitdir}/system/inference-service.service"

# librknnrt is radxa-zero-3w only.
COMPATIBLE_MACHINE = "radxa-zero-3w"
