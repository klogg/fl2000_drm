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
#define READ_ONCE(x) (x)
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
#define dev_dbg(dev,...) ((void)(dev))
#define dev_err_ratelimited(dev,...) ((void)(dev))
#define dev_name(dev) ((void)(dev), "test-bridge")
typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQF_ONESHOT 1
#define IRQF_TRIGGER_LOW 2
#define THIS_MODULE ((void *)3)
#define DRM_MODE_FLAG_INTERLACE BIT(0)
#define DRM_MODE_FLAG_DBLSCAN BIT(1)
#define HDMI_INFOFRAME_HEADER_SIZE 4
#define HDMI_AVI_INFOFRAME_SIZE 13
#define HDMI_INFOFRAME_SIZE(t) 17
struct mutex { int unused; };
struct device { int unused; };
struct i2c_client { int irq; };
struct i2c_adapter { struct device dev; };
struct drm_display_mode { unsigned int clock, flags; };
struct drm_display_info { bool is_hdmi; };
struct drm_connector { void *dev; struct drm_display_info display_info; };
struct drm_crtc_state { struct drm_display_mode mode, adjusted_mode; };
struct drm_encoder { void *crtc; };
struct drm_bridge { struct drm_encoder *encoder; void *dev; };
struct drm_atomic_state { struct drm_connector *connector; struct drm_crtc_state *crtc; };
struct hdmi_avi_infoframe { int unused; };
#define HDMI_I2S 0
struct hdmi_codec_daifmt { int fmt; };
struct hdmi_codec_params { int sample_rate,sample_width,channels; };
enum drm_connector_status { connector_status_connected, connector_status_disconnected, connector_status_unknown };
enum drm_mode_status { MODE_OK, MODE_BAD, MODE_CLOCK_HIGH, MODE_CLOCK_LOW };
struct regmap { u8 values[512]; };
static int fault_reg=-1, fault_write_reg=-1, writes, notifications;
static bool unplugged;
static unsigned int cts_samples[255],cts_sample_count;
static bool sample_cts;
static void msleep(unsigned int delay) { assert(delay==30);cts_sample_count++; }
static unsigned int register_writes[512];
static enum drm_connector_status notified_status;
static int regmap_read(struct regmap *map,unsigned int r,unsigned int *v)
{
 assert(r<512);
 if ((int)r==fault_reg) return -EIO;
 if(sample_cts && r>=0x1a0 && r<=0x1a2) {
  assert(cts_sample_count && cts_sample_count<=255);
  unsigned int cts=cts_samples[cts_sample_count-1];
  *v=r==0x1a0?(cts&15)<<4:r==0x1a1?(cts>>4)&255:cts>>12;
 } else *v=map->values[r];
 return 0;
}
static int regmap_write(struct regmap *map,unsigned int r,unsigned int v)
{
 assert(r<512);
 if ((int)r==fault_reg || (int)r==fault_write_reg) return -EIO;
 writes++;register_writes[r]++;map->values[r]=v;
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
static bool drm_dev_enter(void *dev,int *idx){assert(dev==(void *)1);*idx=0;return !unplugged;}
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
assert not re.search(r"\.mode_set\s*=", source), (
    "Mode programming must run only in atomic enable"
)
ctx = structure(source, "it66121_ctx")
irq_mocks = r"""
#define EPROBE_DEFER 517
#define DOMAIN_BUS_ANY 0
#define DRM_BRIDGE_ATTACH_NO_CONNECTOR 1
struct irq_domain { int unused; };
static struct irq_domain domain;
static struct i2c_adapter adapter;
static bool missing_domain,missing_mapping;
static int attach_error;
#define dev_fwnode(dev) (dev)
static struct irq_domain *irq_find_matching_fwnode(struct device *dev,int token)
{ assert(dev==&adapter.dev && token==DOMAIN_BUS_ANY);return missing_domain?NULL:&domain; }
static int irq_find_mapping(struct irq_domain *d,int hwirq)
{ assert(d==&domain && !hwirq);return missing_mapping?0:37; }
static int drm_bridge_attach(struct drm_encoder *encoder,struct drm_bridge *bridge,void *prev,int flags)
{ assert(encoder && bridge && !prev && flags==DRM_BRIDGE_ATTACH_NO_CONNECTOR);return attach_error; }
static struct it66121_ctx *bound_ctx;
static irqreturn_t (*requested_thread)(int,void *);
static int requested_irq,request_error,irq_frees;
static void *dev_get_drvdata(struct device *dev) { (void)dev;return bound_ctx; }
static int request_threaded_irq(int irq,void *primary,irqreturn_t (*thread)(int,void *),int flags,const char *name,void *data)
{
 assert(!primary && flags==(IRQF_ONESHOT|IRQF_TRIGGER_LOW) && name && data==bound_ctx);
 if(request_error)return request_error;
 assert(!requested_irq);requested_irq=irq;requested_thread=thread;return 0;
}
static void free_irq(int irq,void *data)
{ assert(irq==requested_irq && data==bound_ctx);requested_irq=0;requested_thread=NULL;irq_frees++; }
"""
checks = r"""
int main(void)
{
 struct regmap map={0};
 struct drm_encoder encoder={.crtc=&map};
 struct drm_connector connector={.dev=(void *)1,.display_info={.is_hdmi=true}};
 struct it66121_ctx ctx={.regmap=&map,.bridge={.encoder=&encoder},.connector=&connector,.bus_width=24,.id=ID_IT66121,.aligned_i2c=true,.reset_by_controller=true,.component_binding=true,.retry_irq_ack=true,.input_clock_delay=IT66121_INPUT_MODE_PCLK_DELAY1,.hpd_enabled=true};
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
 assert(!ctx.connector);
 /* DVI keeps parallel-bus/AFE setup while stopping AVI packets. */
 connector.display_info.is_hdmi=false;
 memset(map.values,0xff,sizeof(map.values));
 it66121_bridge_enable(&ctx.bridge,&state);
 assert(map.values[IT66121_HDMI_MODE_REG]==0);
 assert(map.values[IT66121_AVI_INFO_PKT_REG]==0);
 assert(map.values[IT66121_INPUT_MODE_REG]==1);
 assert(map.values[IT66121_INT_MASK1_REG]==0xfe);
 /* Preserve every chip, wiring width, output type, and AFE clock branch. */
 for (unsigned int id=ID_IT6610;id<=ID_IT66122;id++) {
  for (unsigned int width=12;width<=24;width+=12) {
   for (unsigned int hdmi=0;hdmi<2;hdmi++) {
    for (unsigned int high=0;high<2;high++) {
     ctx.id=id;ctx.bus_width=width;connector.display_info.is_hdmi=hdmi;
     crtc.adjusted_mode.clock=high?148500:32000;
     memset(map.values,0xff,sizeof(map.values));
     memset(register_writes,0,sizeof(register_writes));
     it66121_bridge_enable(&ctx.bridge,&state);
     assert(register_writes[IT66121_INPUT_MODE_REG]==1);
     assert(register_writes[IT66121_INT_MASK1_REG]==1);
     assert(register_writes[IT66121_INT_REG]==1);
     assert(register_writes[IT66121_AV_MUTE_REG]==1);
     assert(register_writes[IT66121_CLK_BANK_REG]==(id==ID_IT6610?0U:2U));
     assert(map.values[IT66121_INPUT_MODE_REG]==(1U|(width==12?IT66121_INPUT_MODE_DDR:0U)));
     assert(map.values[IT66121_HDMI_MODE_REG]==hdmi);
     assert(map.values[IT66121_INT_MASK1_REG]==0xfe);
     assert(!(map.values[IT66121_AV_MUTE_REG]&IT66121_AV_MUTE_ON));
     assert(!!(map.values[IT66121_AFE_XP_REG]&IT66121_AFE_XP_GAINBIT)==!!high);
     assert(!!(map.values[IT66121_AFE_IP_REG]&IT66121_AFE_IP_GAINBIT)==!!high);
    }
   }
  }
 }
 ctx.id=ID_IT66121;ctx.bus_width=24;connector.display_info.is_hdmi=true;
 crtc.adjusted_mode.clock=32000;
 /* An incomplete atomic state must never touch the bridge. */
 writes=0;state.connector=NULL;it66121_bridge_enable(&ctx.bridge,&state);assert(!writes);
 state.connector=&connector;state.crtc=NULL;it66121_bridge_enable(&ctx.bridge,&state);assert(!writes);
 state.crtc=&crtc;
 /* A programming failure must not continue to unmute a partial configuration. */
 const unsigned int enable_faults[]={IT66121_HDMI_MODE_REG,IT66121_INPUT_MODE_REG,
  IT66121_AFE_DRV_REG,IT66121_INT_REG,IT66121_INT_MASK1_REG};
 for (unsigned int i=0;i<sizeof(enable_faults)/sizeof(enable_faults[0]);i++) {
  memset(map.values,0xff,sizeof(map.values));
  memset(register_writes,0,sizeof(register_writes));fault_write_reg=(int)enable_faults[i];
  it66121_bridge_enable(&ctx.bridge,&state);
  assert(!register_writes[IT66121_AV_MUTE_REG]);
  assert(map.values[IT66121_AV_MUTE_REG]&IT66121_AV_MUTE_ON);
 }
 fault_write_reg=-1;
 /* Generic boards retain their clock phase and existing IRQ configuration. */
 ctx.input_clock_delay=0;ctx.reset_by_controller=false;map.values[IT66121_INT_REG]=0xc0;
 map.values[IT66121_INT_MASK1_REG]=0x5a;
 memset(register_writes,0,sizeof(register_writes));
 it66121_bridge_enable(&ctx.bridge,&state);
 assert(map.values[IT66121_INPUT_MODE_REG]==0);
 assert(map.values[IT66121_INT_REG]==0xc0);
 assert(map.values[IT66121_INT_MASK1_REG]==0x5a);
 assert(!register_writes[IT66121_INT_REG] && !register_writes[IT66121_INT_MASK1_REG]);
 it66121_bridge_disable(&ctx.bridge,&state);assert(!ctx.connector);
 ctx.bus_width=12;assert(!it66121_configure_input(&ctx));
 assert(map.values[IT66121_INPUT_MODE_REG]==IT66121_INPUT_MODE_DDR);ctx.bus_width=24;
 /* Input timing, transport access, and reset ownership are independent. */
 ctx.aligned_i2c=false;ctx.input_clock_delay=IT66121_INPUT_MODE_PCLK_DELAY1;
 assert(!it66121_configure_input(&ctx));assert(map.values[IT66121_INPUT_MODE_REG]==1);
 ctx.aligned_i2c=true;ctx.input_clock_delay=0;
 assert(!it66121_configure_input(&ctx));assert(!map.values[IT66121_INPUT_MODE_REG]);
 ctx.reset_by_controller=true;
 ctx.irq_output_mode=IT66121_INT_ACTIVE_HIGH|IT66121_INT_OPEN_DRAIN;
 assert(!it66121_restore_interrupts(&ctx));
 assert((map.values[IT66121_INT_REG]&0xc0)==0xc0);
 ctx.irq_output_mode=0;ctx.aligned_i2c=false;
 assert(!it66121_restore_interrupts(&ctx));
 assert(!(map.values[IT66121_INT_REG]&0xc0));
 ctx.aligned_i2c=true;
 /* A transport reset must preserve an explicit framework HPD disable. */
 ctx.input_clock_delay=IT66121_INPUT_MODE_PCLK_DELAY1;ctx.reset_by_controller=true;ctx.bridge.dev=(void *)1;
 it66121_bridge_hpd_disable(&ctx.bridge);assert(!ctx.hpd_enabled);
 it66121_bridge_enable(&ctx.bridge,&state);
 assert(map.values[IT66121_INT_MASK1_REG]==0xff);
 it66121_bridge_hpd_enable(&ctx.bridge);assert(ctx.hpd_enabled);
 it66121_bridge_enable(&ctx.bridge,&state);
 assert(map.values[IT66121_INT_MASK1_REG]==0xfe);
 /* Polling must leave a latched IRQ untouched. */
 map.values[IT66121_SYS_STATUS_REG]=0xc0;
 map.values[IT66121_INT_STATUS1_REG]=1;
 writes=0;
 assert(it66121_bridge_detect(&ctx.bridge,&connector)==connector_status_connected);
 assert(writes==0 && map.values[IT66121_INT_STATUS1_REG]==1);
 fault_reg=IT66121_SYS_STATUS_REG;
 assert(it66121_bridge_detect(&ctx.bridge,&connector)==connector_status_unknown);
 /* A detached bridge must use the old connector's unplugged DRM device. */
 ctx.bridge.dev=NULL;
 fault_reg=-1;unplugged=true;
 assert(it66121_bridge_detect(&ctx.bridge,&connector)==connector_status_disconnected);
 it66121_bridge_hpd_enable(&ctx.bridge);
 it66121_bridge_hpd_disable(&ctx.bridge);
 unplugged=false;ctx.bridge.dev=(void *)1;
 it66121_bridge_hpd_enable(&ctx.bridge);
 assert(!(map.values[IT66121_INT_MASK1_REG]&IT66121_INT_MASK1_HPD));
 it66121_bridge_hpd_disable(&ctx.bridge);
 assert(map.values[IT66121_INT_MASK1_REG]&IT66121_INT_MASK1_HPD);
 /* The interrupt path owns acknowledgment and only notifies real HPD events. */
 assert(!it66121_bridge_interrupt(&ctx.bridge));
 assert(notifications==1 && notified_status==connector_status_connected);
 assert(!(map.values[IT66121_INT_STATUS1_REG]&1));
 assert(!(map.values[IT66121_SYS_STATUS_REG]&0x80));
 writes=0;assert(!it66121_bridge_interrupt(&ctx.bridge));
 assert(notifications==1 && writes==0);
 map.values[IT66121_SYS_STATUS_REG]=0x80;
 map.values[IT66121_INT_STATUS1_REG]=1;
 assert(!it66121_bridge_interrupt(&ctx.bridge));
 assert(notifications==2 && notified_status==connector_status_disconnected);
 /* Failed status reads and acknowledgments must reach the USB worker. */
 const unsigned int irq_regs[]={IT66121_SYS_STATUS_REG,IT66121_INT_STATUS1_REG,IT66121_INT_CLR1_REG};
 for(unsigned int i=0;i<sizeof(irq_regs)/sizeof(irq_regs[0]);i++) {
  map.values[IT66121_SYS_STATUS_REG]=0xc0;map.values[IT66121_INT_STATUS1_REG]=1;
  fault_reg=(int)irq_regs[i];int previous=notifications;
  assert(it66121_bridge_interrupt(&ctx.bridge)==-EIO && notifications==previous);
 }
 fault_reg=-1;
 map.values[IT66121_SYS_STATUS_REG]=0xc0;map.values[IT66121_INT_STATUS1_REG]=1;
 fault_write_reg=IT66121_SYS_STATUS_REG;int before_ack=notifications;
 assert(it66121_bridge_interrupt(&ctx.bridge)==-EIO && notifications==before_ack);
 fault_write_reg=-1;
 assert(!it66121_bridge_interrupt(&ctx.bridge) && notifications==before_ack);
 /* AFE clock selection changes only bit 4 of register 0x68. */
 struct drm_display_mode afe_mode={.clock=148500};
 map.values[IT66121_AFE_XP_EC1_REG]=0xb5;
 assert(!it66121_configure_afe(&ctx,&afe_mode));
 assert(map.values[IT66121_AFE_XP_EC1_REG]==0xa5);
 afe_mode.clock=74250;
 assert(!it66121_configure_afe(&ctx,&afe_mode));
 assert(map.values[IT66121_AFE_XP_EC1_REG]==0xb5);
 /* Mode limits and error propagation retain existing behavior. */
 struct drm_display_mode mode={.clock=25000};
 assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_OK);
 mode.clock=148500;assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_OK);
 mode.clock=148501;assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_CLOCK_HIGH);
 mode.clock=24999;assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_CLOCK_LOW);
 mode.clock=32000;mode.flags=DRM_MODE_FLAG_INTERLACE;
 assert(it66121_bridge_mode_valid(&ctx.bridge,&connector.display_info,&mode)==MODE_OK);
 fault_reg=IT66121_INPUT_MODE_REG;assert(it66121_configure_input(&ctx)==-EIO);
 fault_reg=IT66121_INT_MASK1_REG;assert(it66121_restore_interrupts(&ctx)==-EIO);
 assert(IT66121_CLK_BANK_MASK==3 && IT66121_CLK_BANK_SELECT==1);
 assert(IT66121_INPUT_MODE_COLOR_MASK==0xc0 && IT66121_INPUT_MODE_PCLK_DIV2==0x20);
 assert(IT66121_INPUT_MODE_PCLK_DELAY_MASK==3 && IT66121_INPUT_MODE_PCLK_DELAY2==2);
 assert(IT66121_INPUT_FIFO_REG==0x71 && IT66121_INPUT_FIFO_AUTO_RESET_EN==8);
 assert(IT66121_INPUT_CSC_SELECT_MASK==3 && IT66121_INPUT_CSC_BTAT1004==4);
 assert(IT66121_INT_HDMI_EN==0x20 && IT66121_INT_REF_CLK_DIV==0x0c);
 /* Binding owns a normal IRQ request and releases it before bridge removal. */
 struct device component={0};ctx.adapter=&adapter;
 bound_ctx=&ctx;fault_reg=-1;
 missing_domain=true;assert(it66121_bind(&component,NULL,&encoder)==-EPROBE_DEFER);
 missing_domain=false;missing_mapping=true;assert(it66121_bind(&component,NULL,&encoder)==-EPROBE_DEFER);
 missing_mapping=false;
 request_error=-EBUSY;
 assert(it66121_bind(&component,NULL,&encoder)==-EBUSY && !requested_irq);
 request_error=0;assert(!it66121_bind(&component,NULL,&encoder));
 assert(requested_irq==37 && requested_thread);
 map.values[IT66121_SYS_STATUS_REG]=0xc0;map.values[IT66121_INT_STATUS1_REG]=1;
 assert(requested_thread(37,&ctx)==IRQ_HANDLED);
 fault_reg=IT66121_SYS_STATUS_REG;
 assert(requested_thread(37,&ctx)==IRQ_NONE);
 ctx.retry_irq_ack=false;assert(requested_thread(37,&ctx)==IRQ_HANDLED);ctx.retry_irq_ack=true;
 it66121_unbind(&component,NULL,&encoder);
 assert(!requested_irq && !ctx.connector && irq_frees==1);
 attach_error=-EIO;assert(it66121_bind(&component,NULL,&encoder)==-EIO && !requested_irq && !ctx.irq);
 attach_error=0;
 fault_reg=-1;assert(!it66121_bind(&component,NULL,&encoder));
 it66121_unbind(&component,NULL,&encoder);assert(irq_frees==3);
 /* Audio register programming remains valid for every advertised channel count. */
 fault_reg=fault_write_reg=-1;bound_ctx=&ctx;ctx.audio.auto_cts=false;
 struct hdmi_codec_daifmt fmt={.fmt=HDMI_I2S};
 const struct { int rate;unsigned int n,fs; } rates[]={
  {32000,4096,3},{44100,6272,0},{48000,6144,2},{88200,12544,8},
  {96000,12288,10},{176400,25088,12},{192000,24576,14},{768000,24576,9}};
 const struct { int width;unsigned int swl; } widths[]={
  {16,2},{17,12},{18,4},{19,8},{20,3},{21,13},{22,5},{23,9},{24,11}};
 for(unsigned int r=0;r<sizeof(rates)/sizeof(rates[0]);r++)
  for(unsigned int w=0;w<sizeof(widths)/sizeof(widths[0]);w++)
   for(int channels=1;channels<=8;channels++) {
    struct hdmi_codec_params params={rates[r].rate,widths[w].width,channels};
    memset(map.values,0,sizeof(map.values));
    assert(!it66121_audio_hw_params(&component,NULL,&fmt,&params));
    assert(((u32)map.values[0x133]|map.values[0x134]<<8|map.values[0x135]<<16)==rates[r].n);
    assert(map.values[0x193]==(unsigned int)(channels+1)/2);
    assert(map.values[0x194]==channels);
    assert(map.values[0x198]==rates[r].fs);
    assert(map.values[0x199]==(((~rates[r].fs<<4)&0xf0)|widths[w].swl));
   }
 struct hdmi_codec_params params={48000,24,2};
 for(int channels=0;channels<=9;channels+=9) {
  params.channels=channels;writes=0;
  assert(it66121_audio_hw_params(&component,NULL,&fmt,&params)==-EINVAL && !writes);
 }
 params.channels=2;fmt.fmt=1;writes=0;
 assert(it66121_audio_hw_params(&component,NULL,&fmt,&params)==-EINVAL && !writes);
 fmt.fmt=HDMI_I2S;
 /* Clock and audio register failures propagate rather than enabling output. */
 const unsigned int audio_faults[]={0x58,0x133,0xe1,0x191};
 for(unsigned int i=0;i<sizeof(audio_faults)/sizeof(audio_faults[0]);i++) {
  fault_write_reg=(int)audio_faults[i];
  assert(it66121_audio_hw_params(&component,NULL,&fmt,&params)==-EIO);
 }
 puts("PASS: single-pass modesets across chip/wiring/output variants, enable failures preserve mute, IRQ request failure, handler delivery and rebind, bridge reset restoration, HDMI/DVI, input timing, polling without acknowledgment, HPD IRQ handling, mode limits, I/O errors, audio rates/widths/channels");
}
"""
names = (
    "it66121_fire_afe",
    "it66121_configure_input",
    "it66121_configure_afe",
    "it66121_set_mute",
    "it66121_restore_interrupts",
    "it66121_configure_hdmi",
    "it66121_bridge_enable",
    "it66121_bridge_disable",
    "it66121_bridge_mode_valid",
    "it66121_bridge_detect",
    "it66121_bridge_hpd_enable",
    "it66121_bridge_hpd_disable",
    "it66121_bridge_interrupt",
    "it66121_irq_thread",
    "it66121_bind",
    "it66121_unbind",
    "it661221_set_chstat",
    "it661221_set_lpcm_audio",
    "it661221_set_ncts",
    "it661221_audio_ch_enable",
    "it66121_audio_hw_params",
)
parts = [
    prelude,
    '#include "bridge/it66121_registers.h"',
    "enum chip_id { ID_IT6610, ID_IT66121, ID_IT66122 };",
    ctx,
    irq_mocks,
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
            "-fno-sanitize-recover=all",
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
