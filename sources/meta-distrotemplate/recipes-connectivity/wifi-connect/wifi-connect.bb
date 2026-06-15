DESCRIPTION = "Initscripts for connecting wifi" 
LICENSE = "MIT" 
 
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302" 
 
SRC_URI = "file://wifi-connect.sh" 
 
INITSCRIPT_NAME = "wifi-connect.sh" 
INITSCRIPT_PARAMS = "start 50 S ." 

RDEPENDS:${PN} += "bash"
 
inherit update-rc.d 
 
S = "${WORKDIR}" 
 
do_install () { 
    install -d ${D}${sysconfdir}/init.d/ 
    install -c -m 755 ${WORKDIR}/${INITSCRIPT_NAME} ${D}${sysconfdir}/init.d/${INITSCRIPT_NAME} 
} 