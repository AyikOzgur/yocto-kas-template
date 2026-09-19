
HW_REVISION = "0.0.1"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://enable_hash_verify.cfg"

do_install:append() {

    install -d ${D}${sysconfdir}
    echo "${MACHINE} ${HW_REVISION}" > ${D}${sysconfdir}/hwrevision
}

FILES:${PN} += " \
    ${sysconfdir}/hwrevision \
"