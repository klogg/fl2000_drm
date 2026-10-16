#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise DRM bind error unwinding and registration ownership with sanitizers."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, structure

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "fl2000_drm.c").read_text()
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define FL20000_MAX_WIDTH 4000
#define FL20000_MAX_HEIGHT 4000
#define FL2000_USBIF_AVCONTROL 0
#define DRM_BRIDGE_ATTACH_NO_CONNECTOR 1
#define DRM_FORMAT_XRGB8888 1
#define WRITE_ONCE(p,v) ((p)=(v))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p)?(int)(intptr_t)(p):0)
struct device { void *data; };
struct usb_device { struct device dev; };
struct usb_interface { int unused; };
struct drm_plane { int unused; };
struct drm_crtc { int unused; };
struct drm_encoder { int unused; };
struct drm_connector { bool detached; };
struct drm_mode_config { const void *funcs; int min_width,max_width,min_height,max_height; };
struct drm_device { struct drm_mode_config mode_config; bool registered,unplugged; };
struct fl2000_device { struct drm_device *drm; };
struct fl2000_bridge { void *bridge; struct drm_connector **connector; };
#define to_usb_device(d) ((struct usb_device *)(d))
#define dev_get_drvdata(d) ((struct fl2000_device *)(d)->data)
static int fl2000_drm_driver,fl2000_mode_config_funcs;
"""
mocks = r"""
static int failure,step,unregistered,unplugged,shutdowns,stops,poll_fini,encoder_cleanup,unbound,released;
static bool bound,attached,polling;
static struct drm_connector *bridge_connector;
static int operation(void) { return ++step==failure?-EIO:0; }
static void *__drm_dev_alloc(struct device *d,void *driver,size_t size,size_t offset)
{ (void)d;(void)driver;assert(!offset);return operation()?ERR_PTR(-EIO):calloc(1,size); }
static int drmm_mode_config_init(struct drm_device *d) { (void)d;return operation(); }
static struct usb_interface *usb_ifnum_to_if(struct usb_device *u,int n) { (void)u;assert(!n);return NULL; }
static struct device *usb_intf_get_dma_device(struct usb_interface *i) { (void)i;return NULL; }
static void drm_dev_set_dma_dev(struct drm_device *d,struct device *dma) { (void)d;(void)dma; }
static void put_device(struct device *d) { (void)d; }
static int fl2000_pipeline_init(struct fl2000_drm_if *d) { (void)d;return operation(); }
static int component_bind_all(struct device *d,struct fl2000_bridge *b)
{ (void)d;int ret=operation();if(!ret){bound=true;b->bridge=b;b->connector=&bridge_connector;}return ret; }
static int drm_bridge_attach(struct drm_encoder *e,void *b,void *prev,int flags)
{ (void)e;(void)b;assert(bound && !prev && flags==1);int ret=operation();if(!ret)attached=true;return ret; }
static struct drm_connector *fl2000_bridge_connector_init(struct drm_device *d,struct drm_encoder *e,void *b)
{ (void)d;(void)e;(void)b;return operation()?ERR_PTR(-EIO):calloc(1,sizeof(struct drm_connector)); }
static int drm_connector_attach_encoder(struct drm_connector *c,struct drm_encoder *e)
{ (void)c;(void)e;return operation(); }
static void drm_mode_config_reset(struct drm_device *d) { (void)d; }
static void drm_kms_helper_poll_init(struct drm_device *d) { (void)d;assert(!polling);polling=true; }
static int fl2000_enable_interrupts(struct usb_device *u) { (void)u;return operation(); }
static int drm_dev_register(struct drm_device *d,int flags)
{ assert(!flags && !d->registered);int ret=operation();if(!ret)d->registered=true;return ret; }
static void drm_client_setup_with_fourcc(struct drm_device *d,int format) { assert(d->registered && format==1); }
static int fl2000_enable_interrupt(struct usb_device *u) { (void)u;return operation(); }
static void drm_dev_unregister(struct drm_device *d)
{ assert(d->registered);d->registered=false;unregistered++; }
static void drm_dev_unplug(struct drm_device *d)
{ d->unplugged=true;unplugged++;drm_dev_unregister(d); }
static void drm_atomic_helper_shutdown(struct drm_device *d)
{ assert(d->unplugged && !d->registered);shutdowns++; }
static void fl2000_streaming_disable(struct usb_device *u) { (void)u;stops++; }
static void drm_kms_helper_poll_fini(struct drm_device *d)
{ (void)d;assert(polling);polling=false;poll_fini++; }
static void drm_encoder_cleanup(struct drm_encoder *e) { (void)e;attached=false;encoder_cleanup++; }
static void fl2000_bridge_connector_detach(struct drm_connector *c)
{ assert(!attached);if(c){assert(!c->detached);c->detached=true;} }
static void component_unbind_all(struct device *d,struct fl2000_bridge *b)
{ (void)d;(void)b;assert(bound && !attached);bound=false;bridge_connector=NULL;unbound++; }
static void drm_dev_put(struct drm_device *d)
{
 struct fl2000_drm_if *output=(void *)d;
 assert(!d->registered && !bound && !attached && !polling);
 if(output->connector){assert(output->connector->detached);free(output->connector);}
 free(output);released++;
}
"""
checks = r"""
int main(void)
{
 for(failure=1;failure<=10;failure++) {
  struct fl2000_device device={0};struct usb_device usb={.dev={.data=&device}};
  step=unregistered=unplugged=shutdowns=stops=poll_fini=encoder_cleanup=unbound=released=0;
  assert(fl2000_drm_bind(&usb.dev)==-EIO);
  assert(step==failure && !device.drm && !bound && !attached && !polling);
  assert(released==(failure>1));
  assert(unbound==(failure>4));
  assert(poll_fini==(failure>7));
  assert(unregistered==(failure==10));
  assert(unplugged==(failure==10) && shutdowns==(failure==10) && stops==(failure==10));
 }
 puts("PASS: all DRM bind failure stages, single unregister on interrupt-start failure, preserved errno, balanced teardown");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="fl2000-drm-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text(
        "\n".join([
            prelude,
            structure(source, "fl2000_drm_if"),
            mocks,
            function(source, "fl2000_drm_bind"),
            checks,
        ])
    )
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-no-pie",
            str(c),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
