SUMMARY = "Rockchip RKNN runtime (RKNPU2)"
DESCRIPTION = "Prebuilt librknnrt and its C API headers from rknn-toolkit2. \
Runs .rknn models on the NPU through the rknpu kernel driver."
HOMEPAGE = "https://github.com/airockchip/rknn-toolkit2"
LICENSE = "Proprietary"
LIC_FILES_CHKSUM = "file://LICENSE;md5=a2399e2538b7ebeac89b5b1cc648459a"

# Fetch only the runtime files; the full toolkit repository is ~2 GB.
RKNN_BASE = "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/v${PV}"
RKNN_API = "${RKNN_BASE}/rknpu2/runtime/Linux/librknn_api"

SRC_URI = " \
    ${RKNN_BASE}/LICENSE;subdir=rknnrt;name=license \
    ${RKNN_API}/aarch64/librknnrt.so;subdir=rknnrt;name=lib \
    ${RKNN_API}/include/rknn_api.h;subdir=rknnrt;name=api \
    ${RKNN_API}/include/rknn_custom_op.h;subdir=rknnrt;name=custom-op \
    ${RKNN_API}/include/rknn_matmul_api.h;subdir=rknnrt;name=matmul \
"
SRC_URI[license.sha256sum] = "d846f57d942c7dfdca7b8b54f9e8bb39e1e226790dc4f5ee205d6fd678961720"
SRC_URI[lib.sha256sum] = "d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8"
SRC_URI[api.sha256sum] = "c48e11a6f41b451a5fd1e4ad774ea60252d3d94f78bee9b21ea3d21b21deba9a"
SRC_URI[custom-op.sha256sum] = "af5983da0ca244ca31dc3162aa683322b0285531196c7a770f29cd2e3b8ccaa6"
SRC_URI[matmul.sha256sum] = "aaadd9a7118de30a06b222996b6731db77095d00f5931a7a98c83a67f14a4d42"

S = "${WORKDIR}/rknnrt"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${libdir} ${D}${includedir}
    install -m 0755 ${S}/librknnrt.so ${D}${libdir}
    install -m 0644 ${S}/rknn_api.h ${S}/rknn_custom_op.h ${S}/rknn_matmul_api.h ${D}${includedir}
}

# The soname is the unversioned librknnrt.so, so it belongs in the main
# package rather than -dev.
FILES_SOLIBSDEV = ""
FILES:${PN} = "${libdir}/librknnrt.so"

# Needs the rknpu driver to do anything.
RRECOMMENDS:${PN} = "rknpu-driver"

# Prebuilt by Rockchip: stripped and linked without GNU_HASH.
INSANE_SKIP:${PN} = "already-stripped ldflags"

COMPATIBLE_MACHINE = "radxa-zero-3w"
