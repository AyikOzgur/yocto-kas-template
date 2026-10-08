DESCRIPTION = "Test streamer, hardware encodes overlay-service frames (JPEG, or H.264/H.265 with MPP) and sends RTP"
LICENSE = "CLOSED"

# Socket code shared with inference-service and overlay-service.
FILESEXTRAPATHS:prepend := "${THISDIR}/../pipeline-common:"

SRC_URI = "file://main.cpp \
           file://VideoEncoder.h \
           file://V4l2JpegEncoder.h \
           file://V4l2JpegEncoder.cpp \
           file://MppEncoder.h \
           file://MppEncoder.cpp \
           file://RtpSender.h \
           file://RtpSender.cpp \
           file://RtpJpegSender.h \
           file://RtpJpegSender.cpp \
           file://RtpH26xSender.h \
           file://RtpH26xSender.cpp \
           file://streamer-service.service \
           file://FrameHeader.h \
           file://SocketClient.h \
           file://SocketClient.cpp \
"

S = "${WORKDIR}"

# Defaults, can be overridden from local.conf / kas, or at runtime with
# /var/streamer-service.env (see the unit file).
STREAMER_DEST ?= "192.168.1.7:5004"
# jpeg (V4L2 Hantro, mainline kernel) or h264 / h265 (MPP, vendor kernel).
STREAMER_CODEC ?= "jpeg"
STREAMER_QUALITY ?= "75"
STREAMER_BITRATE ?= "2000"

# H.264/H.265 through MPP, only with the vendor kernel (/dev/mpp_service).
MPP_SOURCES = ""
MPP_SOURCES:radxa-zero-3w-vendor = "${S}/MppEncoder.cpp"
MPP_FLAGS = ""
MPP_FLAGS:radxa-zero-3w-vendor = "-DWITH_MPP"
MPP_LIBS = ""
MPP_LIBS:radxa-zero-3w-vendor = "-lrockchip_mpp"
DEPENDS:append:radxa-zero-3w-vendor = " rockchip-mpp"
# Built differently per machine.
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit systemd

do_compile() {
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 ${MPP_FLAGS} \
        ${S}/main.cpp ${S}/V4l2JpegEncoder.cpp ${S}/RtpSender.cpp ${S}/RtpJpegSender.cpp \
        ${S}/RtpH26xSender.cpp ${S}/SocketClient.cpp ${MPP_SOURCES} \
        -o ${B}/streamer-service -lpthread ${MPP_LIBS}
}

do_install() {
    install -D -m 0755 ${B}/streamer-service ${D}${bindir}/streamer-service

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/streamer-service.service ${D}${systemd_unitdir}/system
    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@STREAMER_DEST@@|${STREAMER_DEST}|g' \
    -e 's|@@STREAMER_CODEC@@|${STREAMER_CODEC}|g' \
    -e 's|@@STREAMER_QUALITY@@|${STREAMER_QUALITY}|g' \
    -e 's|@@STREAMER_BITRATE@@|${STREAMER_BITRATE}|g' \
    ${D}${systemd_unitdir}/system/streamer-service.service
}

SYSTEMD_SERVICE:${PN} = "streamer-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${systemd_unitdir}/system/streamer-service.service"

# Hantro VEPU, the RK3566 hardware JPEG encoder on the mainline kernel.
RRECOMMENDS:${PN} = "kernel-module-hantro-vpu"
RRECOMMENDS:${PN}:radxa-zero-3w-vendor = ""
