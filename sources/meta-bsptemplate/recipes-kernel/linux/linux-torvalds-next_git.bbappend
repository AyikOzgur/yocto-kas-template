# Original BSP layer has outdated commit. Override it with proper one.
SRCREV = "8e7c1c539395e34648d859c20b0f9478eb5901cc"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://radxa-zero-3w-wifi.dtsi \
            file://radxa-zero-3w-npu.dtsi \
            file://dmabuf-heaps.cfg"

do_configure:append() {
    # Append the Wi-Fi and NPU fragments to the main device tree, once.
    dts=${S}/arch/arm64/boot/dts/rockchip/rk3566-radxa-zero-3w.dts
    for frag in radxa-zero-3w-wifi.dtsi radxa-zero-3w-npu.dtsi; do
        if ! grep -q "BEGIN ${frag}" $dts; then
            echo "/* BEGIN ${frag} */" >> $dts
            cat ${WORKDIR}/${frag} >> $dts
        fi
    done
}

do_deploy:append() {
    if [ -e ${DEPLOYDIR}/fitImage ]; then
        install -m 0644 ${DEPLOYDIR}/fitImage ${DEPLOYDIR}/fitImageA
        install -m 0644 ${DEPLOYDIR}/fitImage ${DEPLOYDIR}/fitImageB
    fi
}