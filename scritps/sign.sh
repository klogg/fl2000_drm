#!/bin/sh

KVER=$(uname -r)
SIGN_CMD=/usr/src/linux-headers-$KVER/scripts/sign-file
KEY_DIR=/var/lib/shim-signed/mok

for module in "fl2000.ko" "it66121.ko"
do
    $SIGN_CMD sha256 $KEY_DIR/MOK.priv $KEY_DIR/MOK.der $module 2>&1
done

exit 0