DESCRIPTION = "Test streamer, hardware JPEG encodes renderer-service frames and sends RTP/JPEG"
LICENSE = "CLOSED"

# Socket code shared with inference-service and renderer-service.
FILESEXTRAPATHS:prepend := "${THISDIR}/../pipeline-common:"

SRC_URI = "file://main.cpp \
           file://V4l2JpegEncoder.h \
           file://V4l2JpegEncoder.cpp \
           file://RtpJpegSender.h \
           file://RtpJpegSender.cpp \
           file://streamer-service.service \
           file://FrameHeader.h \
           file://SocketClient.h \
           file://SocketClient.cpp \
"

S = "${WORKDIR}"

# Default RTP destination and JPEG quality, can be overridden from local.conf / kas,
# or at runtime with /var/streamer-service.env (see the unit file).
STREAMER_DEST ?= "192.168.1.100:5004"
STREAMER_QUALITY ?= "75"

inherit systemd

do_compile() {
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 \
        ${S}/main.cpp ${S}/V4l2JpegEncoder.cpp ${S}/RtpJpegSender.cpp ${S}/SocketClient.cpp \
        -o ${B}/streamer-service -lpthread
}

do_install() {
    install -D -m 0755 ${B}/streamer-service ${D}${bindir}/streamer-service

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/streamer-service.service ${D}${systemd_unitdir}/system
    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@STREAMER_DEST@@|${STREAMER_DEST}|g' \
    -e 's|@@STREAMER_QUALITY@@|${STREAMER_QUALITY}|g' \
    ${D}${systemd_unitdir}/system/streamer-service.service
}

SYSTEMD_SERVICE:${PN} = "streamer-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${systemd_unitdir}/system/streamer-service.service"

# Hantro VEPU, the RK3566 hardware JPEG encoder.
RRECOMMENDS:${PN} = "kernel-module-hantro-vpu"
