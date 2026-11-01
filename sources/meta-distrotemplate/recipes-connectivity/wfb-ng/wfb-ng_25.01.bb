SUMMARY = "WFB-ng, one way video link over Wi-Fi in monitor mode (WiFi broadcast)"
DESCRIPTION = "Only the C tools (wfb_tx, wfb_rx, wfb_keygen) and wfb-video.service, \
which injects streamer-service's MPEG-TS on the USB Wi-Fi card (wfb0)."
HOMEPAGE = "https://github.com/svpcom/wfb-ng"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=84dcc94da3adb52b53ae4fa38fe49e5d"

SRC_URI = "git://github.com/svpcom/wfb-ng.git;protocol=https;branch=release-25.01 \
           file://wfb-video.sh \
           file://wfb-video.service \
           file://10-wfb0.link \
"
SRCREV = "ec936d5153461b4b1c57ca90f732373170ecb37c"

S = "${WORKDIR}/git"

DEPENDS = "libsodium libpcap"

# Defaults, can be overridden from local.conf / kas, or at runtime with
# /var/wfb-video.env (see the unit file). The ground station needs the same
# channel and passphrase (tools/wfb-ground.sh).
# 20 MHz wide. 5.8 GHz: the RTL8812EU board barely radiates on 2.4 GHz, and it keeps
# clear of the 2.4 GHz RC link.
WFB_CHANNEL ?= "149"
# Driver units, about mBm (1000 ~ 10 dBm). Kept low for bench tests.
WFB_TXPOWER ?= "1000"
# HT MCS index, 1 = QPSK 1/2, 13 Mbit/s on air at 20 MHz.
WFB_MCS ?= "1"
# Keys for both ends are derived from it.
WFB_PASSPHRASE ?= "wfb-video"
# Country code for the regulatory domain, e.g. PL. Empty keeps the kernel default.
WFB_REGION ?= ""

inherit systemd

# Version string without git and python at build time.
EXTRA_OEMAKE = "VERSION=${PV} COMMIT=${SRCREV}"

do_compile() {
    oe_runmake wfb_tx wfb_rx wfb_keygen
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/wfb_tx ${S}/wfb_rx ${S}/wfb_keygen ${D}${bindir}
    install -m 0755 ${WORKDIR}/wfb-video.sh ${D}${bindir}

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/wfb-video.service ${D}${systemd_unitdir}/system
    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    -e 's|@@WFB_CHANNEL@@|${WFB_CHANNEL}|g' \
    -e 's|@@WFB_TXPOWER@@|${WFB_TXPOWER}|g' \
    -e 's|@@WFB_MCS@@|${WFB_MCS}|g' \
    -e 's|@@WFB_PASSPHRASE@@|${WFB_PASSPHRASE}|g' \
    -e 's|@@WFB_REGION@@|${WFB_REGION}|g' \
    ${D}${systemd_unitdir}/system/wfb-video.service

    install -D -m 0644 ${WORKDIR}/10-wfb0.link ${D}${systemd_unitdir}/network/10-wfb0.link
}

SYSTEMD_SERVICE:${PN} = "wfb-video.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${systemd_unitdir}/system/wfb-video.service ${systemd_unitdir}/network/10-wfb0.link"

# iw reg set needs the signed regulatory database (CONFIG_CFG80211_REQUIRE_SIGNED_REGDB).
RDEPENDS:${PN} = "iw wireless-regdb-static"
