#!/bin/bash
# Build NVMeVirt in conventional-SSD and ZNS personalities.
set -e
COMMIT=61c90f7758cbd9545b4a4727e89377bf88eab060
[ -d nvmevirt ] || git clone https://github.com/snu-csl/nvmevirt.git
cd nvmevirt && git checkout $COMMIT && cd ..

build_variant() {   # $1 = conv|zns
    local v=$1
    cp -r nvmevirt build_$v
    pushd build_$v > /dev/null
    if [ "$v" = conv ]; then
        sed -i 's/^CONFIG_NVMEVIRT_NVM := y/#CONFIG_NVMEVIRT_NVM := y/' Kbuild
        sed -i 's/^#CONFIG_NVMEVIRT_SSD := y/CONFIG_NVMEVIRT_SSD := y/' Kbuild
    else
        sed -i 's/^CONFIG_NVMEVIRT_NVM := y/#CONFIG_NVMEVIRT_NVM := y/' Kbuild
        sed -i 's/^#CONFIG_NVMEVIRT_ZNS := y/CONFIG_NVMEVIRT_ZNS := y/' Kbuild
    fi
    # paper config: SAMSUNG_970PRO profile already sets OP_AREA_PERCENT (0.07),
    # matching the simulator's 7% OP; ZNS profiles use 0. No patch needed.
    grep -n 'OP_AREA' ssd_config.h || true
    make -j"$(nproc)"
    cp nvmev.ko ../nvmev_$v.ko
    popd > /dev/null
}
build_variant conv
build_variant zns
echo "built: nvmev_conv.ko nvmev_zns.ko"
