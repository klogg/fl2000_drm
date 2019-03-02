#!/bin/sh

modprobe drm
modprobe drm_kms_helper

insmod fl2000.ko
insmod it66121.ko

