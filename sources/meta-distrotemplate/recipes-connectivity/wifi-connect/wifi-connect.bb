DESCRIPTION = "Initscripts for connecting wifi" 
LICENSE = "MIT" 
 
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302" 
 
SRC_URI = "file://wifi-connect.sh \
           file://wifi-connect.service" 
 
INITSCRIPT_NAME = "wifi-connect.sh" 

RDEPENDS:${PN} += "bash"
 
inherit systemd
 
S = "${WORKDIR}"

SYSTEMD_SERVICE:${PN} = "wifi-connect.service"
SYSTEMD_AUTO_ENABLE = "enable" 

do_install () {
    install -d ${D}${bindir} 
    install -d ${D}${systemd_unitdir}/system 
    install -m 0644 ${WORKDIR}/wifi-connect.service ${D}${systemd_unitdir}/system

    sed \
        -e "s|@@WIFI_NAME@@|${WIFI_NAME}|g" \
        -e "s|@@WIFI_PASSWD@@|${WIFI_PASSWD}|g" \
        ${WORKDIR}/${INITSCRIPT_NAME} \
        > ${D}${bindir}/${INITSCRIPT_NAME}

    sed -i \
    -e 's|@@BINDIR@@|${bindir}|g' \
    ${D}${systemd_unitdir}/system/wifi-connect.service

    chmod 755 ${D}${bindir}/${INITSCRIPT_NAME}
}

FILES:${PN} += "${systemd_unitdir}/system/wifi-connect.service"