#!/bin/sh
set -eu

CONFIGFS=/sys/kernel/config/usb_gadget
GADGET=$CONFIGFS/g1
CONFIG=$GADGET/configs/c.1

host_mac() {
    sha1sum /proc/device-tree/serial-number \
        | fold -1 \
        | sed -n '1{h;d};2{y/1235679abdef/000444888ccc/;H;d};13{g;s/\n//g;p;q};s/^/:/;N;H;d'
}

start() {
    if [ ! -d "$CONFIGFS" ]; then
        modprobe libcomposite
    fi

    if [ -d "$GADGET" ]; then
        if [ -L "$CONFIG/acm.0" ] && [ -L "$CONFIG/ncm.0" ] && [ -n "$(cat "$GADGET/UDC")" ]; then
            ifconfig "$(cat "$GADGET/functions/ncm.0/ifname")" up
            exit 0
        fi
        echo "$GADGET already exists with an incompatible configuration" >&2
        exit 1
    fi

    retries=100
    while [ ! -e /sys/class/udc/49000000.usb ] && [ "$retries" -gt 0 ]; do
        sleep 0.1
        retries=$((retries - 1))
    done
    [ -e /sys/class/udc/49000000.usb ] || {
        echo "USB device controller 49000000.usb is unavailable" >&2
        exit 1
    }

    mkdir "$GADGET"
    echo 0x1d6b > "$GADGET/idVendor"
    echo 0x0104 > "$GADGET/idProduct"
    echo 0x0200 > "$GADGET/bcdUSB"
    echo 0xef > "$GADGET/bDeviceClass"
    echo 0x02 > "$GADGET/bDeviceSubClass"
    echo 0x01 > "$GADGET/bDeviceProtocol"
    echo 0x0100 > "$GADGET/bcdDevice"

    mkdir -p "$GADGET/strings/0x409"
    tr -d '\0' < /proc/device-tree/serial-number > "$GADGET/strings/0x409/serialnumber"
    echo STMicroelectronics > "$GADGET/strings/0x409/manufacturer"
    echo STM32MP > "$GADGET/strings/0x409/product"

    mkdir -p "$CONFIG/strings/0x409"
    echo "Config 1: NCM + ACM" > "$CONFIG/strings/0x409/configuration"
    echo 0xe0 > "$CONFIG/bmAttributes"
    echo 0 > "$CONFIG/MaxPower"

    mkdir -p "$GADGET/os_desc"
    echo 1 > "$GADGET/os_desc/use"
    echo 0xbc > "$GADGET/os_desc/b_vendor_code"
    echo MSFT100 > "$GADGET/os_desc/qw_sign"

    mkdir -p "$GADGET/functions/ncm.0/os_desc/interface.ncm"
    echo WINNCM > "$GADGET/functions/ncm.0/os_desc/interface.ncm/compatible_id"
    host_mac > "$GADGET/functions/ncm.0/host_addr"
    mkdir "$GADGET/functions/acm.0"

    ln -s "$GADGET/functions/ncm.0" "$CONFIG/ncm.0"
    ln -s "$GADGET/functions/acm.0" "$CONFIG/acm.0"
    ln -s "$CONFIG" "$GADGET/os_desc/c.1"
    echo 49000000.usb > "$GADGET/UDC"

    sleep 0.2
    ifconfig "$(cat "$GADGET/functions/ncm.0/ifname")" up
}

case ${1:-start} in
    start) start ;;
    *) echo "Usage: $0 start" >&2; exit 2 ;;
esac
