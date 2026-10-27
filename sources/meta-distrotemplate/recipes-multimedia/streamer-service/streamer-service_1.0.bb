DESCRIPTION = "Streamer, hardware encodes overlay-service frames (H.264/H.265 with MPP) and serves them over SRT"
LICENSE = "CLOSED"

# Socket code shared with inference-service and overlay-service.
FILESEXTRAPATHS:prepend := "${THISDIR}/../pipeline-common:"

SRC_URI = "file://main.cpp \
           file://MppEncoder.h \
           file://MppEncoder.cpp \
           file://TsMuxer.h \
           file://TsMuxer.cpp \
           file://SrtServer.h \
           file://SrtServer.cpp \
           file://BitrateController.h \
           file://streamer-service.service \
           file://FrameHeader.h \
           file://SocketClient.h \
           file://SocketClient.cpp \
"

S = "${WORKDIR}"

# Defaults, can be overridden from local.conf / kas, or at runtime with
# /var/streamer-service.env (see the unit file).
STREAMER_PORT ?= "9000"
# h264 or h265.
STREAMER_CODEC ?= "h264"
# Max bitrate in kbps, lowered at runtime when the link struggles.
STREAMER_BITRATE ?= "2000"
# SRT latency in ms, the receiver's own setting counts too (the larger one is used).
STREAMER_LATENCY ?= "50"

DEPENDS = "rockchip-mpp srt"

# MPP needs the vendor kernel (/dev/mpp_service).
COMPATIBLE_MACHINE = "radxa-zero-3w-vendor"
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit systemd

do_compile() {
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 \
        ${S}/main.cpp ${S}/MppEncoder.cpp ${S}/TsMuxer.cpp ${S}/SrtServer.cpp ${S}/SocketClient.cpp \
        -o ${B}/streamer-service -lpthread -lrockchip_mpp -lsrt
}

do_install() {
    install -D -m 0755 ${B}/streamer-service ${D}${bindir}/streamer-service

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/streamer-service.service ${D}${systemd_unitdir}/system
    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@STREAMER_PORT@@|${STREAMER_PORT}|g' \
    -e 's|@@STREAMER_CODEC@@|${STREAMER_CODEC}|g' \
    -e 's|@@STREAMER_BITRATE@@|${STREAMER_BITRATE}|g' \
    -e 's|@@STREAMER_LATENCY@@|${STREAMER_LATENCY}|g' \
    ${D}${systemd_unitdir}/system/streamer-service.service
}

SYSTEMD_SERVICE:${PN} = "streamer-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${systemd_unitdir}/system/streamer-service.service"
