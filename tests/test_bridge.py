#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise rebased bridge modesetting, reset restoration, and HPD acknowledgment."""

import re
import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, structure

ROOT = Path(__file__).resolve().parents[1]

source = re.sub(
    r"/\*.*?\*/", "", (ROOT / "bridge/it66121_drv.c").read_text(), flags=re.DOTALL
)
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define __always_unused __attribute__((__unused__))
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((1U << ((h)+1))-1) & ~((1U << (l))-1))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define DRM_ERROR(...) ((void)0)
#define dev_err(dev,...) ((void)(dev))
#define DRM_MODE_FLAG_INTERLACE BIT(0)
#define DRM_MODE_FLAG_DBLSCAN BIT(1)
#define HDMI_INFOFRAME_HEADER_SIZE 4
#define HDMI_AVI_INFOFRAME_SIZE 13
#define HDMI_INFOFRAME_SIZE(t) 17
struct mutex { int unused; };
struct device { int unused; };
struct i2c_client;
struct i2c_adapter;
struct drm_display_mode { unsigned int clock, flags; };
struct drm_display_info { bool is_hdmi; };
struct drm_connector { struct drm_display_info display_info; };
struct drm_crtc_state { struct drm_display_mode mode, adjusted_mode; };
struct drm_encoder { void *crtc; };
struct drm_bridge { struct drm_encoder *encoder; void *dev; };
struct drm_atomic_state { struct drm_connector *connector; struct drm_crtc_state *crtc; };
struct hdmi_avi_infoframe { int unused; };
enum drm_connector_status { connector_status_connected, connector_status_disconnected, connector_status_unknown };
enum drm_mode_status { MODE_OK, MODE_BAD, MODE_CLOCK_HIGH, MODE_CLOCK_LOW };
struct regmap { u8 values[512]; };
static int fault_reg=-1, writes, notifications;
static bool unplugged;
static enum drm_connector_status notified_status;
static int regmap_read(struct regmap *map,unsigned int r,unsigned int *v)
{
 assert(r<512);
 if ((int)r==fault_reg) return -EIO;
 *v=map->values[r];return 0;
}
static int regmap_write(struct regmap *map,unsigned int r,unsigned int v)
{
 assert(r<512);
 if ((int)r==fault_reg) return -EIO;
 writes++;map->values[r]=v;
 if(r==0x0c && (v&1))map->values[6]&=~1;
 if(r==0x0e && (v&1))map->values[0x0e]&=~0x80;
 return 0;
}
static int regmap_write_bits(struct regmap *map,unsigned int r,unsigned int mask,unsigned int v)
{
 return regmap_write(map,r,(map->values[r]&~mask)|(v&mask));
}
static int regmap_bulk_write(struct regmap *map,unsigned int r,const u8 *v,unsigned int n)
{
 for(unsigned int i=0;i<n;i++){int ret=regmap_write(map,r+i,v[i]);if(ret)return ret;}return 0;
}
static bool drm_dev_enter(void *dev,int *idx){(void)dev;*idx=0;return !unplugged;}
static void drm_dev_exit(int idx){(void)idx;}
static void drm_bridge_hpd_notify(struct drm_bridge *b,enum drm_connector_status s)
{(void)b;notifications++;notified_status=s;}
static struct drm_connector *drm_atomic_get_new_connector_for_encoder(struct drm_atomic_state *s,struct drm_encoder *e)
{(void)e;return s->connector;}
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(struct drm_atomic_state *s,void *crtc)
{(void)crtc;return s->crtc;}
static int drm_hdmi_avi_infoframe_from_display_mode(struct hdmi_avi_infoframe *f,struct drm_connector *c,const struct drm_display_mode *m)
{(void)f;(void)m;assert(c && c->display_info.is_hdmi);return 0;}
static int hdmi_avi_infoframe_pack(struct hdmi_avi_infoframe *f,u8 *buf,unsigned int n)
{(void)f;assert(n==17);for(unsigned int i=0;i<n;i++)buf[i]=i+17;return n;}
"""
ctx = structure(source, "it66121_ctx")
checks = r"""
int main(void)
{
 struct regmap map={0};
 struct drm_encoder encoder={.crtc=&map};
 struct drm_connector connector={.display_info={.is_hdmi=true}};
 struct it66121_ctx ctx={.regmap=&map,.bridge={.encoder=&encoder},.connector=&connector,.bus_width=24,.id=ID_IT66121};
 struct drm_crtc_state crtc={.adjusted_mode={.clock=32000}};
 struct drm_atomic_state state={.connector=&connector,.crtc=&crtc};
 /* Simulate the encoder reset immediately before atomic bridge enable. */
 memset(map.values,0xff,sizeof(map.values));
 it66121_bridge_enable(&ctx.bridge,&state);
 assert(map.values[IT66121_INPUT_MODE_REG]==1);
 assert(map.values[IT66121_INPUT_CSC_REG]==0);
 assert(map.values[IT66121_HDMI_MODE_REG]==1);
 assert(map.values[IT66121_AVIINFO_DB1_REG]==21);
 assert(map.values[IT66121_AVIINFO_DB1_REG+12]==33);
 assert(map.values[IT66121_AVIINFO_CSUM_REG]==20);
 assert(map.values[IT66121_AVI_INFO_PKT_REG]==3);
 assert(!(map.values[IT66121_CLK_BANK_REG]&IT66121_CLK_BANK_PWROFF_TXCLK));
 assert(map.values[IT66121_INT_MASK1_REG]==0xfe);
 assert(!(map.values[IT66121_INT_REG]&(IT66121_INT_ACTIVE_HIGH|IT66121_INT_OPEN_DRAIN)));
 assert(!(map.values[IT66121_AV_MUTE_REG]&IT66121_AV_MUTE_ON));
 assert(map.values[IT66121_AFE_DRV_REG]==0);
 assert(!(map.values[IT66121_SW_RST_REG]&(IT66121_SW_RST_REF|IT66121_SW_RST_VID)));
 it66121_bridge_disable(&ctx.bridge,&state);
 assert(map.values[IT66121_AV_MUTE_REG]&IT66121_AV_MUTE_ON);
 assert(ctx.connector==&connector);
 /* DVI keeps parallel-bus/AFE setup while stopping AVI packets. */
 connector.display_info.is_hdmi=false;
 memset(map.values,0xff,sizeof(map.values));
 it66121_bridge_enable(&ctx.bridge,&state);
 assert(map.values[IT66121_HDMI_MODE_REG]==0);
 assert(map.values[IT66121_AVI_INFO_PKT_REG]==0);
 assert(map.values[IT66121_INPUT_MODE_REG]==1);
 assert(map.values[IT66121_INT_MASK1_REG]==0xfe);
 /* Polling must leave a latched IRQ untouched. */
 map.values[IT66121_SYS_STATUS_REG]=0xc0;
 map.values[IT66121_INT_STATUS1_REG]=1;
 writes=0;
 assert(it66121_bridge_detect(&ctx.bridge,&connector)==connector_status_connected);
 assert(writes==0 && map.values[IT66121_INT_STATUS1_REG]==1);
 fault_reg=IT66121_SYS_STATUS_REG;
 assert(it66121_bridge_detect(&ctx.bridge,&connector)==connector_status_unknown);
 fault_reg=-1;unplugged=true;
 assert(it66121_bridge_detect(&ctx.bridge,&connector)==connector_status_disconnected);
 unplugged=false;
 /* The interrupt path owns acknowledgment and only notifies real HPD events. */
 it66121_bridge_interrupt(&ctx.bridge);
 assert(notifications==1 && notified_status==connector_status_connected);
 assert(!(map.values[IT66121_INT_STATUS1_REG]&1));
 assert(!(map.values[IT66121_SYS_STATUS_REG]&0x80));
 writes=0;it66121_bridge_interrupt(&ctx.bridge);
 assert(notifications==1 && writes==0);
 map.values[IT66121_SYS_STATUS_REG]=0x80;
 map.values[IT66121_INT_STATUS1_REG]=1;
 it66121_bridge_interrupt(&ctx.bridge);
 assert(notifications==2 && notified_status==connector_status_disconnected);
 /* Mode limits and error propagation retain existing behavior. */
 struct drm_display_mode mode={.clock=25000};
 assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_OK);
 mode.clock=148500;assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_OK);
 mode.clock=148501;assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_CLOCK_HIGH);
 mode.clock=24999;assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_CLOCK_LOW);
 mode.clock=32000;mode.flags=DRM_MODE_FLAG_INTERLACE;
 assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_BAD);
 fault_reg=IT66121_INPUT_MODE_REG;assert(it66121_configure_input(&ctx)==-EIO);
 fault_reg=IT66121_INT_MASK1_REG;assert(it66121_restore_interrupts(&ctx)==-EIO);
 puts("PASS: bridge reset restoration, HDMI/DVI, input timing, polling without acknowledgment, HPD IRQ handling, mode limits, I/O errors");
}
"""
names = (
    "it66121_fire_afe",
    "it66121_configure_input",
    "it66121_configure_afe",
    "it66121_is_hpd_detect",
    "it66121_set_mute",
    "it66121_restore_interrupts",
    "it66121_configure_hdmi",
    "it66121_bridge_mode_set",
    "it66121_bridge_enable",
    "it66121_bridge_disable",
    "it66121_bridge_mode_valid",
    "it66121_bridge_detect",
    "it66121_bridge_interrupt",
)
parts = [
    prelude,
    '#include "bridge/it66121_registers.h"',
    "enum chip_id { ID_IT6610, ID_IT66121, ID_IT66122 };",
    ctx,
]
parts += [function(source, name) for name in names]
with tempfile.TemporaryDirectory(prefix="fl2000-bridge-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text("\n\n".join(parts + [checks]))
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-no-pie",
            "-I",
            str(ROOT),
            str(c),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
