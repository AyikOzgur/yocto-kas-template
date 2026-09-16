DESCRIPTION = "Update status checking and controlling service" 
LICENSE = "MIT" 
 
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302" 
 
SRC_URI = "file://update-status.py \
           file://update-status.service" 
 

RDEPENDS:${PN} += "python3-core"
 
inherit systemd
 
S = "${WORKDIR}"

SYSTEMD_SERVICE:${PN} = "update-status.service"
SYSTEMD_AUTO_ENABLE = "enable" 

do_install () {
    install -d ${D}${bindir} 
    install -d ${D}${systemd_unitdir}/system 
    install -m 0644 ${WORKDIR}/update-status.service ${D}${systemd_unitdir}/system

    install -m 755 ${WORKDIR}/update-status.py ${D}${bindir}/update-status.py

    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    ${D}${systemd_unitdir}/system/update-status.service
}

FILES:${PN} += "${systemd_unitdir}/system/update-status.service"