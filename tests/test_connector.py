#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise connector callback guards and bridge reference ownership with sanitizers."""

import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = re.sub(
    r"^#include.*$", "", (ROOT / "fl2000_connector.c").read_text(), flags=re.MULTILINE
)
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define __always_unused __attribute__((__unused__))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define kzalloc_obj(value) calloc(1,sizeof(value))
#define kfree(p) free(p)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define DRM_CONNECTOR_POLL_HPD 1
#define DRM_MODE_CONNECTOR_HDMIA 1
#define ENOMEM 12
struct drm_device { bool unplugged; };
struct drm_encoder;
struct drm_display_info { int unused; };
struct drm_display_mode { int unused; };
struct drm_edid { int unused; };
struct drm_connector;
struct drm_bridge;
enum drm_connector_status { connector_status_connected, connector_status_disconnected };
enum drm_mode_status { MODE_OK, MODE_ERROR };
struct drm_bridge_funcs {
 enum drm_connector_status (*detect)(struct drm_bridge *,struct drm_connector *);
 const struct drm_edid *(*edid_read)(struct drm_bridge *,struct drm_connector *);
 enum drm_mode_status (*mode_valid)(struct drm_bridge *,const struct drm_display_info *,const struct drm_display_mode *);
};
struct drm_bridge { const struct drm_bridge_funcs *funcs; unsigned int refs; };
struct drm_connector_funcs {
 void (*reset)(void);
 enum drm_connector_status (*detect)(struct drm_connector *,bool);
 void (*fill_modes)(void);
 void (*destroy)(struct drm_connector *);
 void (*atomic_duplicate_state)(void);
 void (*atomic_destroy_state)(void);
};
struct drm_connector_helper_funcs {
 int (*get_modes)(struct drm_connector *);
 enum drm_mode_status (*mode_valid)(struct drm_connector *,const struct drm_display_mode *);
};
struct drm_connector {
 struct drm_device *dev;
 struct drm_display_info display_info;
 const struct drm_connector_funcs *funcs;
 const struct drm_connector_helper_funcs *helper;
 unsigned int polled;
};
static int entered,calls,init_error,cleanups,put_count,updates,mode_error;
static bool drm_dev_enter(struct drm_device *d,int *idx)
{ *idx=1;if(d->unplugged)return false;entered++;return true; }
static void drm_dev_exit(int idx) { assert(idx==1 && entered==1);entered--; }
static struct drm_bridge *drm_bridge_get(struct drm_bridge *b) { b->refs++;return b; }
static void drm_bridge_put(struct drm_bridge *b)
{ if(b){assert(b->refs);b->refs--;put_count++;} }
static void drm_connector_cleanup(struct drm_connector *c) { (void)c;cleanups++; }
static int drm_connector_init(struct drm_device *d,struct drm_connector *c,const struct drm_connector_funcs *f,int type)
{ assert(type==DRM_MODE_CONNECTOR_HDMIA);c->dev=d;c->funcs=f;return init_error; }
static void drm_connector_helper_add(struct drm_connector *c,const struct drm_connector_helper_funcs *h)
{ c->helper=h; }
static int drm_edid_connector_update(struct drm_connector *c,const struct drm_edid *e)
{ (void)c;(void)e;assert(entered);updates++;return mode_error; }
static int drm_edid_connector_add_modes(struct drm_connector *c) { (void)c;return 3; }
static void drm_edid_free(const struct drm_edid *e) { free((void *)e); }
static void drm_atomic_helper_connector_reset(void) {}
static void drm_helper_probe_single_connector_modes(void) {}
static void drm_atomic_helper_connector_duplicate_state(void) {}
static void drm_atomic_helper_connector_destroy_state(void) {}
static enum drm_connector_status detect(struct drm_bridge *b,struct drm_connector *c)
{ (void)b;(void)c;assert(entered);calls++;return connector_status_connected; }
static const struct drm_edid *edid(struct drm_bridge *b,struct drm_connector *c)
{ (void)b;(void)c;assert(entered);calls++;return calloc(1,sizeof(struct drm_edid)); }
static enum drm_mode_status valid(struct drm_bridge *b,const struct drm_display_info *i,const struct drm_display_mode *m)
{ (void)b;(void)i;(void)m;assert(entered);calls++;return MODE_OK; }
"""
checks = r"""
int main(void)
{
 struct drm_device dev={0};
 struct drm_display_mode mode={0};
 struct drm_bridge_funcs *funcs=malloc(sizeof(*funcs));
 *funcs=(struct drm_bridge_funcs){.detect=detect,.edid_read=edid,.mode_valid=valid};
 struct drm_bridge *bridge=calloc(1,sizeof(*bridge));bridge->funcs=funcs;bridge->refs=1;
 init_error=-ENOMEM;
 assert(IS_ERR(fl2000_bridge_connector_init(&dev,NULL,bridge)));
 assert(bridge->refs==1 && put_count==1);
 init_error=0;
 struct drm_connector *c=fl2000_bridge_connector_init(&dev,NULL,bridge);
 assert(!IS_ERR(c) && bridge->refs==2);
 assert(c->funcs->detect(c,false)==connector_status_connected);
 assert(c->helper->get_modes(c)==3);
 assert(c->helper->mode_valid(c,&mode)==MODE_OK && calls==3 && !entered);
 mode_error=-ENOMEM;
 assert(c->helper->get_modes(c)==0 && !entered);mode_error=0;
 /* Unplug blocks forwarding before the provider is detached. */
 dev.unplugged=true;int before=calls;
 assert(c->funcs->detect(c,false)==connector_status_disconnected);
 assert(c->helper->get_modes(c)==0 && c->helper->mode_valid(c,&mode)==MODE_ERROR);
 assert(calls==before && !entered);
 fl2000_bridge_connector_detach(c);assert(bridge->refs==1);
 fl2000_bridge_connector_detach(c);assert(bridge->refs==1);
 fl2000_bridge_connector_detach(NULL);
 /* Model provider removal and its callback table becoming inaccessible. */
 drm_bridge_put(bridge);assert(!bridge->refs);free(funcs);free(bridge);
 assert(c->funcs->detect(c,false)==connector_status_disconnected);
 assert(c->helper->get_modes(c)==0 && c->helper->mode_valid(c,&mode)==MODE_ERROR);
 c->funcs->destroy(c);assert(cleanups==1);
 /* Bind failure: release the reference before provider unbind, idempotently. */
 struct drm_bridge live={.refs=1};dev.unplugged=false;
 c=fl2000_bridge_connector_init(&dev,NULL,&live);
 fl2000_bridge_connector_detach(c);assert(live.refs==1);
 assert(c->funcs->detect(c,false)==connector_status_disconnected);
 assert(c->helper->get_modes(c)==0 && c->helper->mode_valid(c,&mode)==MODE_ERROR);
 c->funcs->destroy(c);assert(live.refs==1 && cleanups==2 && !entered);
 c=fl2000_bridge_connector_init(&dev,NULL,&live);
 c->funcs->destroy(c);assert(live.refs==1 && cleanups==3);
 printf("PASS: guarded connector callbacks, init failure, early detach, provider removal, delayed destruction, idempotent cleanup\n");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="fl2000-connector-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text("\n".join([prelude, source, checks]))
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
