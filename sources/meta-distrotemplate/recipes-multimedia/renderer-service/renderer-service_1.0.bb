DESCRIPTION = "Test renderer, draws inference-service detections on camera frames (Vulkan)"
LICENSE = "CLOSED"

# Socket and message code shared with inference-service and streamer-service.
FILESEXTRAPATHS:prepend := "${THISDIR}/../pipeline-common:"

SRC_URI = "file://main.cpp \
           file://Renderer.h \
           file://Renderer.cpp \
           file://vulkan-utils.h \
           file://vulkan-utils.cpp \
           file://DmaHeap.h \
           file://DmaHeap.cpp \
           file://overlay_yuyv.comp \
           file://renderer-service.service \
           file://FrameHeader.h \
           file://Detections.h \
           file://SocketClient.h \
           file://SocketClient.cpp \
           file://SocketServer.h \
           file://SocketServer.cpp \
"

S = "${WORKDIR}"

DEPENDS = "vulkan-loader vulkan-headers glslang-native"
# panvk vulkan driver from mesa.
RDEPENDS:${PN} += "mesa-vulkan-drivers"

inherit systemd

SHADER_DIR = "${datadir}/renderer-service"

do_compile() {
    glslangValidator -V --target-env vulkan1.0 ${S}/overlay_yuyv.comp -o ${B}/overlay_yuyv.spv
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 \
        -DSHADER_PATH='"${SHADER_DIR}/overlay_yuyv.spv"' \
        ${S}/main.cpp ${S}/Renderer.cpp ${S}/vulkan-utils.cpp ${S}/DmaHeap.cpp \
        ${S}/SocketClient.cpp ${S}/SocketServer.cpp \
        -o ${B}/renderer-service -lvulkan -lpthread
}

do_install() {
    install -D -m 0755 ${B}/renderer-service ${D}${bindir}/renderer-service
    install -D -m 0644 ${B}/overlay_yuyv.spv ${D}${SHADER_DIR}/overlay_yuyv.spv

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/renderer-service.service ${D}${systemd_unitdir}/system
    sed -i -e 's|@@BINDIR@@|${bindir}|g' ${D}${systemd_unitdir}/system/renderer-service.service
}

SYSTEMD_SERVICE:${PN} = "renderer-service.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${SHADER_DIR} ${systemd_unitdir}/system/renderer-service.service"
