#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise native output detection, DDC and bridge callbacks with sanitizers."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, register_header, structure

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "fl2000_vga.c").read_text()
registers = (ROOT / "fl2000_registers.c").read_text()
table_start = source.index(
    "static const struct drm_bridge_funcs fl2000_vga_bridge_funcs"
)
bridge_funcs = source[table_start : source.index("\n};", table_start) + 3]
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT_U32(n) (UINT32_C(1) << (n))
#define GENMASK_U32(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define __always_unused __attribute__((__unused__))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define I2C_LOCK_ROOT_ADAPTER 1
#define DRM_MODE_CONNECTOR_VGA 1
#define DRM_BRIDGE_OP_DETECT 1
#define DRM_BRIDGE_OP_EDID 2
#define DRM_BRIDGE_OP_HPD 4
#define DRM_BRIDGE_ATTACH_NO_CONNECTOR 1
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define dev_err_ratelimited(...) (++logs)
#define dev_warn_ratelimited(...) (++logs)
struct device { int unused; };
struct usb_device { struct device dev; };
#define to_usb_device(d) ((struct usb_device *)(d))
struct drm_device { bool unplugged; };
struct drm_atomic_commit { int unused; };
struct drm_connector { int unused; };
struct drm_encoder { int unused; };
struct drm_bridge_funcs;
struct drm_bridge { int type,ops; const struct drm_bridge_funcs *funcs; };
struct regmap { u32 status,control; };
struct i2c_adapter { bool locked; };
struct drm_edid { u8 data[128]; };
enum drm_connector_status {
 connector_status_disconnected,connector_status_connected,connector_status_unknown
};
struct drm_bridge_state { int unused; };
struct drm_bridge_funcs {
 struct drm_bridge_state *(*atomic_duplicate_state)(struct drm_bridge *);
 void (*atomic_destroy_state)(struct drm_bridge *,struct drm_bridge_state *);
#ifdef FL2000_DRM_LEGACY_ATOMIC
 struct drm_bridge_state *(*atomic_reset)(struct drm_bridge *);
#else
 struct drm_bridge_state *(*atomic_create_state)(struct drm_bridge *);
#endif
 void (*atomic_disable)(struct drm_bridge *,struct drm_atomic_commit *);
 enum drm_connector_status (*detect)(struct drm_bridge *,struct drm_connector *);
 const struct drm_edid *(*edid_read)(struct drm_bridge *,struct drm_connector *);
};
static struct drm_bridge_state initial_state;
static unsigned int state_creations;
static struct drm_bridge_state *drm_atomic_helper_bridge_create_state(struct drm_bridge *bridge)
{ assert(bridge);state_creations++;return &initial_state; }
#ifdef FL2000_DRM_LEGACY_ATOMIC
#define drm_atomic_helper_bridge_reset drm_atomic_helper_bridge_create_state
#endif
static struct drm_bridge_state *drm_atomic_helper_bridge_duplicate_state(struct drm_bridge *bridge)
{ assert(bridge);return &initial_state; }
static void drm_atomic_helper_bridge_destroy_state(struct drm_bridge *bridge,struct drm_bridge_state *state)
{ assert(bridge && state==&initial_state); }
static struct regmap map;
static struct i2c_adapter adapter;
static int reg_error,reg_reads,reg_writes,logs,accesses;
static struct regmap *dev_get_regmap(struct device *d,const char *n)
{ (void)d;(void)n;return &map; }
static int regmap_read(struct regmap *m,unsigned int reg,unsigned int *value)
{ assert(reg==0x8000);reg_reads++;if(reg_error)return reg_error;*value=m->status;return 0; }
static int regmap_write_bits(struct regmap *m,unsigned int reg,u32 mask,u32 value)
{
 assert(reg==0x8004 && mask==BIT_U32(7) && !value);reg_writes++;
 if(reg_error)return reg_error;
 m->control=(m->control&~mask)|value;return 0;
}
static bool drm_dev_enter(struct drm_device *drm,int *idx)
{ *idx=0;if(drm->unplugged)return false;accesses++;return true; }
static void drm_dev_exit(int idx) { assert(!idx && accesses==1);accesses--; }
static struct i2c_adapter *fl2000_i2c_adapter(struct usb_device *u)
{ (void)u;return &adapter; }
static void i2c_lock_bus(struct i2c_adapter *a,int flags)
{ assert(!a->locked && flags==1);a->locked=true; }
static void i2c_unlock_bus(struct i2c_adapter *a,int flags)
{ assert(a->locked && flags==1);a->locked=false; }
static unsigned int segment,ddc_calls,ddc_fail_at;
static bool restore_failure;
static u8 expected(unsigned int offset) { return offset*37+offset/256; }
static int fl2000_i2c_dword(struct usb_device *u,bool read,unsigned int addr,u8 offset,u32 *data)
{
 (void)u;assert(adapter.locked);
 if(++ddc_calls==ddc_fail_at)return -ETIMEDOUT;
 if(!read) {
  assert(addr==0x30 && *data==offset*0x01010101U);
  if(!offset && restore_failure)return -EREMOTEIO;
  segment=offset;return 0;
 }
 assert(addr==0x50 && !(offset&3));
 *data=0;for(unsigned int i=0;i<4;i++)*data|=(u32)expected(segment*256+offset+i)<<(i*8);
 return 0;
}
"""
mocks = r"""
static struct drm_edid cached;
static bool edid_failure;
static const struct drm_edid *drm_edid_read_custom(struct drm_connector *connector,
 int (*read)(void *,u8 *,unsigned int,size_t),void *ctx)
{
 assert(connector && accesses==1);
 if(edid_failure)return NULL;
 return read(ctx,cached.data,0,128)?NULL:&cached;
}
static unsigned int alloc_stage,alloc_fail;
static const struct drm_bridge_funcs fl2000_vga_bridge_funcs;
static struct fl2000_vga *allocated;
static void *allocate_bridge(size_t size)
{
 if(++alloc_stage==alloc_fail)return ERR_PTR(-ENOMEM);
 allocated=calloc(1,size);assert(allocated);return allocated;
}
static void *allocate_vga(const struct drm_bridge_funcs *funcs)
{
 struct fl2000_vga *vga=allocate_bridge(sizeof(*vga));
 if(!IS_ERR(vga))vga->bridge.funcs=funcs;
 return vga;
}
#define devm_drm_bridge_alloc(d,t,m,f) ((void)(d),(t *)allocate_vga(f))
static int devm_drm_bridge_add(struct device *d,struct drm_bridge *bridge)
{ (void)d;assert(bridge==&allocated->bridge);return ++alloc_stage==alloc_fail?-EIO:0; }
static int drm_bridge_attach(struct drm_encoder *encoder,struct drm_bridge *bridge,
 struct drm_bridge *previous,int flags)
{
 assert(encoder && bridge==&allocated->bridge && !previous && flags==1);
 if(++alloc_stage==alloc_fail)return -EINVAL;
#ifdef FL2000_DRM_LEGACY_ATOMIC
 assert(bridge->funcs->atomic_reset);
 assert(bridge->funcs->atomic_reset(bridge)==&initial_state);
#else
 assert(bridge->funcs->atomic_create_state);
 assert(bridge->funcs->atomic_create_state(bridge)==&initial_state);
#endif
 return 0;
}
"""
checks = r"""
int main(void)
{
 struct usb_device usb={0};struct drm_device drm={0};struct drm_connector connector={0};
 struct fl2000_vga vga={.usb_dev=&usb,.drm=&drm};
 bool native=true;
 /* The hardware output pin, not monitor presence, selects HDMI versus VGA. */
 const u32 status[]={0,FL2000_VGA_STATUS_MONITOR_STATUS,FL2000_VGA_STATUS_HDMI_STATUS,
  FL2000_VGA_STATUS_HDMI_STATUS|FL2000_VGA_STATUS_MONITOR_STATUS};
 for(unsigned int i=0;i<4;i++) {
  map.status=status[i];assert(!fl2000_detect_output(&usb,&native));
  assert(native==(i<2));
 }
 reg_error=-EIO;native=true;
 assert(fl2000_detect_output(&usb,&native)==-EIO && native);
 assert(fl2000_vga_detect(&vga.bridge,&connector)==connector_status_unknown && !accesses);
 reg_error=0;map.status=FL2000_VGA_STATUS_MONITOR_STATUS;
 assert(fl2000_vga_detect(&vga.bridge,&connector)==connector_status_connected);
 map.status=0;assert(fl2000_vga_detect(&vga.bridge,&connector)==connector_status_disconnected);
 drm.unplugged=true;int reads=reg_reads;
 assert(fl2000_vga_detect(&vga.bridge,&connector)==connector_status_disconnected);
 assert(!fl2000_vga_edid_read(&vga.bridge,&connector) && reg_reads==reads && !accesses);
 drm.unplugged=false;
 assert(fl2000_vga_edid_read(&vga.bridge,&connector)==&cached && !accesses);
 edid_failure=true;assert(!fl2000_vga_edid_read(&vga.bridge,&connector) && !accesses);
 edid_failure=false;
 map.control=UINT32_MAX;fl2000_vga_disable(&vga.bridge,NULL);
 assert(map.control==(UINT32_MAX&~FL2000_VGA_CTRL_PXCLK_DAC_OUTPUT_EN) && !accesses);
 reg_error=-EIO;int writes=reg_writes;
 fl2000_vga_disable(&vga.bridge,NULL);assert(reg_writes==writes+1 && !accesses);
 drm.unplugged=true;fl2000_vga_disable(&vga.bridge,NULL);
 assert(reg_writes==writes+1);drm.unplugged=false;reg_error=0;
 /* Odd lengths must not overwrite the caller's tail, including high segments. */
 for(unsigned int block=0;block<6;block++)for(size_t len=1;len<=128;len++) {
  u8 bytes[130];memset(bytes,0xa5,sizeof(bytes));
  assert(!fl2000_vga_read_edid_block(&vga,bytes+1,block,len));
  for(size_t i=0;i<len;i++)assert(bytes[i+1]==expected(block*128+i));
  assert(bytes[0]==0xa5 && bytes[len+1]==0xa5 && !segment && !adapter.locked);
 }
 u8 bytes[128];
 assert(fl2000_vga_read_edid_block(&vga,NULL,0,128)==-EINVAL);
 assert(fl2000_vga_read_edid_block(&vga,bytes,0,0)==-EINVAL);
 assert(fl2000_vga_read_edid_block(&vga,bytes,0,129)==-EINVAL);
 assert(fl2000_vga_read_edid_block(&vga,bytes,512,128)==-EINVAL);
 /* Fail segment selection, each data read, and restoration independently. */
 for(unsigned int fail=1;fail<=34;fail++) {
  ddc_calls=0;ddc_fail_at=fail;segment=0;
  assert(fl2000_vga_read_edid_block(&vga,bytes,2,128)==-ETIMEDOUT && !adapter.locked);
  if(fail!=34)assert(!segment);
 }
 ddc_fail_at=0;segment=0;restore_failure=true;
 assert(fl2000_vga_read_edid_block(&vga,bytes,2,128)==-EREMOTEIO && !adapter.locked);
 assert(vga.ddc_segment_dirty);
 restore_failure=false;
 assert(!fl2000_vga_read_edid_block(&vga,bytes,0,128) && !segment && !vga.ddc_segment_dirty);
 for(unsigned int i=0;i<128;i++)assert(bytes[i]==expected(i));
 /* Preserve the original read error if resetting the segment also fails. */
 ddc_calls=0;ddc_fail_at=2;restore_failure=true;
 assert(fl2000_vga_read_edid_block(&vga,bytes,2,128)==-ETIMEDOUT && !adapter.locked);
 restore_failure=false;ddc_fail_at=0;
 assert(!fl2000_vga_read_edid_block(&vga,bytes,0,128) && !vga.ddc_segment_dirty);
 struct drm_encoder encoder={0};
 for(alloc_fail=1;alloc_fail<=4;alloc_fail++) {
  alloc_stage=0;allocated=NULL;
  const int errors[]={0,-ENOMEM,-EIO,-EINVAL,0};
  assert(fl2000_vga_attach(&usb.dev,&drm,&encoder)==errors[alloc_fail]);
  if(allocated) {
   assert(allocated->drm==&drm && allocated->usb_dev==&usb && allocated->bridge.type==1);
   free(allocated);
  }
 }
 assert(!accesses && logs && state_creations==1);
 puts("PASS: output pin detection, native monitor status, EDID segments/tails/failures, unplug guards and VGA bridge attachment");
}
"""

with tempfile.TemporaryDirectory(prefix="fl2000-vga-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text(
        "\n".join(
            [
                prelude,
                register_header((ROOT / "fl2000_registers.h").read_text()),
                structure(source, "fl2000_vga"),
                mocks,
                function(registers, "fl2000_detect_output"),
                function(source, "fl2000_vga_read_edid_block"),
                function(source, "fl2000_vga_detect"),
                function(source, "fl2000_vga_edid_read"),
                function(source, "fl2000_vga_disable"),
                bridge_funcs,
                function(source, "fl2000_vga_attach"),
                checks,
            ]
        )
    )
    for legacy in (False, True):
        subprocess.run(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer",
                "-no-pie",
                *(["-DFL2000_DRM_LEGACY_ATOMIC"] if legacy else []),
                str(c),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
