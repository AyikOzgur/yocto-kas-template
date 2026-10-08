SUMMARY = "Rockchip Media Process Platform (MPP)"
DESCRIPTION = "Userspace library for the Rockchip hardware video codecs. On RK3566 \
it drives the H.264/H.265 encoder (VEPU540) through the vendor kernel's /dev/mpp_service."
HOMEPAGE = "https://github.com/rockchip-linux/mpp"
LICENSE = "Apache-2.0 & MIT"
LIC_FILES_CHKSUM = "file://LICENSES/Apache-2.0;md5=7f43e699e0a26fae98c2938092f008d2 \
                    file://LICENSES/MIT;md5=e8f57dd048e186199433be2c41bd3d6d"

SRC_URI = "git://github.com/rockchip-linux/mpp.git;protocol=https;branch=develop"
SRCREV = "14729dd578e570e5f00fd1dd2113f5429012d64b"
PV = "1.0+git"

S = "${WORKDIR}/git"

DEPENDS = "libdrm"

inherit cmake pkgconfig

# Test tools (mpi_enc_test, mpp_info_test, ...) for bring-up, in their own package.
EXTRA_OECMAKE = "-DBUILD_TEST=ON"

PACKAGES =+ "${PN}-tests"
FILES:${PN}-tests = "${bindir}"

# Needs the vendor kernel's mpp_service driver.
COMPATIBLE_MACHINE = "radxa-zero-3w-vendor"
