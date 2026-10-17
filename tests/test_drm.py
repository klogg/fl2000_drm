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
#define __always_unused __attribute__((__unused__))
#define FL20000_MAX_WIDTH 4000
#define FL20000_MAX_HEIGHT 4000
#define FL2000_USBIF_AVCONTROL 0
#define DRM_BRIDGE_ATTACH_NO_CONNECTOR 1
#define DRM_FORMAT_XRGB8888 1
#define WRITE_ONCE(p,v) ((p)=(v))
#define READ_ONCE(p) (p)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p)?(int)(intptr_t)(p):0)
struct device { void *data; };
struct usb_device { struct device dev; };
struct usb_interface { int unused; };
struct drm_plane { int unused; };
struct drm_crtc { int unused; };
struct drm_encoder { void *bridge; };
enum drm_connector_status { connector_status_disconnected, connector_status_connected, connector_status_unknown };
struct drm_connector { enum drm_connector_status status; };
struct drm_mode_config { const void *funcs; int min_width,max_width,min_height,max_height; };
struct drm_device { struct drm_mode_config mode_config; bool registered,unplugged; };
struct fl2000_device { struct drm_device *drm; struct module *bridge_owner; };
struct module { int refs; };

#define to_usb_device(d) ((struct usb_device *)(d))
#define dev_get_drvdata(d) ((struct fl2000_device *)(d)->data)
static int fl2000_drm_driver,fl2000_mode_config_funcs;
"""
mocks = r"""
static int failure,step,unregistered,unplugged,shutdowns,stops,poll_fini,encoder_cleanup,unbound,released;
static bool bound,attached,polling;
static struct module provider;
static bool reject_provider;
static void (*release_owner)(struct drm_device *,void *);
static void *owner_data;
static bool try_module_get(struct module *m) { if(reject_provider)return false;m->refs++;return true; }
static void module_put(struct module *m) { assert(m->refs);m->refs--; }
static int hpd_calls,hotplug_events;
static enum drm_connector_status detected=connector_status_connected;
static bool drm_dev_enter(struct drm_device *d,int *idx) { *idx=0;return !d->unplugged; }
static void drm_dev_exit(int idx) { assert(!idx); }
static enum drm_connector_status drm_bridge_detect(void *b,struct drm_connector *c)
{ assert(b && c);hpd_calls++;return detected; }
static void drm_bridge_hpd_notify(void *b,enum drm_connector_status status)
{ assert(b && status==detected);hotplug_events++; }

static int operation(void) { return ++step==failure?-EIO:0; }
static void *__drm_dev_alloc(struct device *d,void *driver,size_t size,size_t offset)
{ (void)d;(void)driver;assert(!offset);return operation()?ERR_PTR(-EIO):calloc(1,size); }
static int drmm_mode_config_init(struct drm_device *d) { (void)d;return operation(); }
static struct usb_interface *usb_ifnum_to_if(struct usb_device *u,int n) { (void)u;assert(!n);return NULL; }
static struct device *usb_intf_get_dma_device(struct usb_interface *i) { (void)i;return NULL; }
static void drm_dev_set_dma_dev(struct drm_device *d,struct device *dma) { (void)d;(void)dma; }
static void put_device(struct device *d) { (void)d; }
static int fl2000_pipeline_init(struct fl2000_drm_if *d) { (void)d;return operation(); }
static int component_bind_all(struct device *d,struct drm_encoder *e)
{ (void)d;int ret=operation();if(!ret){ret=operation();if(!ret){bound=true;attached=true;e->bridge=e;}}return ret; }
static struct drm_bridge *drm_bridge_chain_get_first_bridge(struct drm_encoder *e) { return e->bridge; }
static void drm_bridge_put(struct drm_bridge *b) { assert(b); }
static struct drm_connector *drm_bridge_connector_init(struct drm_device *d,struct drm_encoder *e)
{ (void)d;(void)e;return operation()?ERR_PTR(-EIO):calloc(1,sizeof(struct drm_connector)); }
static int drm_connector_attach_encoder(struct drm_connector *c,struct drm_encoder *e)
{ (void)c;(void)e;return operation(); }
static int drmm_add_action_or_reset(struct drm_device *d,void (*action)(struct drm_device *,void *),void *owner)
{ int r=operation();if(r)action(d,owner);else { release_owner=action;owner_data=owner; } return r; }
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
static void component_unbind_all(struct device *d,struct drm_encoder *b)
{ (void)d;(void)b;assert(bound && !attached);bound=false;unbound++; }
static void drm_dev_put(struct drm_device *d)
{
 struct fl2000_drm_if *output=(void *)d;
 assert(!d->registered && !bound && !attached && !polling);
 if(output->connector){assert(provider.refs);free(output->connector);}
 if(release_owner){release_owner(d,owner_data);release_owner=NULL;}
 assert(!provider.refs);
 free(output);released++;
}
"""
checks = r"""
int main(void)
{
 for(failure=1;failure<=11;failure++) {
  struct fl2000_device device={.bridge_owner=&provider};struct usb_device usb={.dev={.data=&device}};
  step=unregistered=unplugged=shutdowns=stops=poll_fini=encoder_cleanup=unbound=released=0;
  assert(fl2000_drm_bind(&usb.dev)==-EIO);
  assert(step==failure && !device.drm && !bound && !attached && !polling);
  assert(released==(failure>1));
  assert(unbound==(failure>6));
  assert(poll_fini==(failure>8));
  assert(unregistered==(failure==11));
  assert(unplugged==(failure==11) && shutdowns==(failure==11) && stops==(failure==11));
 }
 failure=0;step=0;reject_provider=true;
 struct fl2000_device failed_device={.bridge_owner=&provider};struct usb_device failed_usb={.dev={.data=&failed_device}};
 assert(fl2000_drm_bind(&failed_usb.dev)==-ENODEV && !provider.refs && !bound);
 reject_provider=false;
 struct fl2000_drm_if output={0};
 struct fl2000_device device={.bridge_owner=&provider};struct usb_device usb={.dev={.data=&device}};
 assert(fl2000_drm_hotplug(&usb)==-ENODEV);
 struct drm_connector connector={0};output.connector=&connector;
 device.drm=&output.drm;output.encoder.bridge=&output;
 assert(!fl2000_drm_hotplug(&usb) && hpd_calls==1 && hotplug_events==1);
 connector.status=detected;
 assert(!fl2000_drm_hotplug(&usb) && hpd_calls==2 && hotplug_events==1);
 detected=connector_status_unknown;
 assert(fl2000_drm_hotplug(&usb)==-EIO && hotplug_events==1);
 output.drm.unplugged=true;
 assert(fl2000_drm_hotplug(&usb)==-ENODEV && hpd_calls==3);
 puts("PASS: standard encoder attachment, all DRM bind failure stages, single unregister on interrupt-start failure, preserved errno, balanced teardown");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="fl2000-drm-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text(
        "\n".join(
            [
                prelude,
                structure(source, "fl2000_drm_if"),
                mocks,
                function(source, "fl2000_bridge_module_put"),
                function(source, "fl2000_drm_bind"),
                function(source, "fl2000_drm_hotplug"),
                checks,
            ]
        )
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
