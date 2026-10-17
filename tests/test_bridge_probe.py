#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise normal firmware and opt-in FL2000 bridge probe paths."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, structure

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "bridge/it66121_drv.c").read_text()
prelude = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((1U << ((h)+1))-1) & ~((1U << (l))-1))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define EPROBE_DEFER 517
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p)?PTR_ERR(p):0)
#define I2C_FUNC_I2C 1
#define DRM_MODE_CONNECTOR_HDMIA 1
#define DRM_BRIDGE_OP_DETECT 1
#define DRM_BRIDGE_OP_EDID 2
#define DRM_BRIDGE_OP_HPD 4
#define IRQF_ONESHOT 1
#define GPIOD_OUT_LOW 0
#define PLATFORM_DEVID_AUTO -1
#define HDMI_CODEC_DRV_NAME "hdmi-audio-codec"
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
struct device_node { int unused; };
struct device { void *data; struct device_node *of_node; };
struct i2c_adapter { struct device dev; };
struct i2c_client { struct device dev; struct i2c_adapter *adapter; int irq; };
struct drm_bridge { struct drm_bridge *next_bridge; struct device_node *of_node; int type,ops; };
struct drm_connector { int unused; };
struct gpio_desc { int unused; };
struct mutex { int unused; };
struct hdmi_avi_infoframe { int unused; };
struct platform_device { int unused; };
struct regmap { int unused; };
struct regmap_config { int val_bits,reg_bits,max_register,cache_type; const void *ranges; size_t num_ranges; bool use_single_read,use_single_write; };
#define REGCACHE_NONE 0
struct hdmi_codec_pdata { const void *ops; int i2s,spdif,max_i2s_channels,no_capture_mute; };
enum chip_id { ID_IT6610, ID_IT66121, ID_IT66122 };
"""
mocks = r"""
static struct it66121_ctx *allocated;
static struct i2c_adapter adapter;
static struct device_node node;
static struct drm_bridge downstream;
static struct gpio_desc gpio;
static struct regmap map;
static struct platform_device codec;
static const int it66121_bridge_funcs,it66121_component_ops,it66121_audio_codec_ops;
static const char *const it66121_supplies[]={"vcn33","vcn18","vrf12"};
static const struct it66121_chip_info it66xx_chip_info[]={
 {ID_IT6610,0xca00,0x0611},{ID_IT66121,0x4954,0x0612},{ID_IT66122,0x4954,0x0622}};
static bool transport,ready=true,has_audio=true,gpio_present=true;
static unsigned int width=24,chip=ID_IT66121;
static int regulator_error,gpio_error,irq_error,codec_error,action_error,component_error,read_error;
static int supplies,resets,irq_requests,codec_creates,codec_removes,components,component_removes,bridges,bridge_removes;
static void (*release_codec)(void *);
static void *release_data;
static struct it66121_ctx *allocate_ctx(struct device *dev)
{ (void)dev;assert(!allocated);allocated=calloc(1,sizeof(*allocated));return allocated; }
#define devm_drm_bridge_alloc(dev,type,member,funcs) ((void)(funcs),allocate_ctx(dev))
static bool i2c_check_functionality(struct i2c_adapter *a,int flags)
{ assert(a==&adapter && flags==1);return true; }
static unsigned int alignment=4,delay=1;
static int property_error;
static bool device_property_present(struct device *dev,const char *name)
{ assert(dev==&adapter.dev);return transport && (!strcmp(name,"i2c-register-access-alignment") || !strcmp(name,"display-input-clock-delay")); }
static bool device_property_read_bool(struct device *dev,const char *name)
{
 assert(dev==&adapter.dev);
 if(!strcmp(name,"interrupt-active-high") || !strcmp(name,"interrupt-open-drain"))return false;
 assert(!strcmp(name,"display-controller-reset") || !strcmp(name,"drm-component-attachment") || !strcmp(name,"interrupt-ack-retry"));
 return transport;
}
static int device_property_read_u32(struct device *dev,const char *name,u32 *value)
{
 assert(dev==&adapter.dev && transport);
 if(property_error)return property_error;
 if(!strcmp(name,"i2c-register-access-alignment"))*value=alignment;
 else if(!strcmp(name,"display-input-clock-delay"))*value=delay;
 else {assert(!strcmp(name,"display-input-bus-width"));*value=width;}
 return 0;
}
static struct device_node *of_graph_get_endpoint_by_regs(struct device_node *n,int port,int endpoint)
{ assert(!port && !endpoint);return n; }
static int of_property_read_u32(struct device_node *n,const char *name,u32 *value)
{ assert(n==&node && !strcmp(name,"bus-width"));*value=width;return 0; }
static void of_node_put(struct device_node *n) { assert(n==&node); }
static struct device_node *of_graph_get_remote_node(struct device_node *n,int port,int endpoint)
{ assert(n==&node && port==1 && endpoint==-1);return n; }
static struct drm_bridge *of_drm_find_and_get_bridge(struct device_node *n)
{ assert(n==&node);return ready?&downstream:NULL; }
static int devm_regulator_bulk_get_enable(struct device *dev,size_t count,const char *const *names)
{ (void)dev;assert(count==3 && !strcmp(names[0],"vcn33"));supplies++;return regulator_error; }
static struct gpio_desc *devm_gpiod_get_optional(struct device *dev,const char *name,int flags)
{ (void)dev;assert(!strcmp(name,"reset") && flags==0);return gpio_error?ERR_PTR(gpio_error):(gpio_present?&gpio:NULL); }
static void gpiod_set_value_cansleep(struct gpio_desc *g,int value)
{ assert(!g || g==&gpio);assert(value==1 || value==0);if(g)resets++; }
static void msleep(int delay) { assert(delay==20); }
static void mutex_init(struct mutex *m) { (void)m; }
static struct regmap *devm_regmap_init_i2c(struct i2c_client *client,const void *config)
{ (void)client;const struct regmap_config *c=config;assert(c->use_single_read==(transport && alignment==4) && c->use_single_write==(transport && alignment==4));return &map; }
static const int it66121_regmap_banks[]={0};
static int dev_err_probe(struct device *dev,int error,const char *format,...)
{ (void)dev;(void)format;return error; }
static int regmap_read(struct regmap *m,unsigned int reg,unsigned int *value)
{
 assert(m==&map && reg<4);if(read_error)return read_error;
 const struct it66121_chip_info *info=&it66xx_chip_info[chip%3];
 *value=reg==0?(info->vid&255):reg==1?(info->vid>>8):reg==2?(info->pid&255):((info->pid>>8)|0xa0);
 if(chip==3)*value=0;return 0;
}
static void i2c_set_clientdata(struct i2c_client *c,void *data) { c->dev.data=data; }
static void *i2c_get_clientdata(struct i2c_client *c) { return c->dev.data; }
static int it66121_irq_thread(int irq,void *data) { (void)irq;(void)data;return 1; }
#define dev_name(dev) ((void)(dev),"it66121-test")
static int devm_request_threaded_irq(struct device *dev,int irq,void *primary,void *thread,int flags,const char *name,void *data)
{ (void)dev;assert(irq==37 && !primary && thread && flags==IRQF_ONESHOT && name && data==allocated);irq_requests++;return irq_error; }
static bool of_property_present(struct device_node *n,const char *property)
{ assert(!strcmp(property,"#sound-dai-cells"));return n && has_audio; }
static struct platform_device *platform_device_register_data(struct device *dev,const char *name,int id,const void *data,size_t size)
{
 (void)dev;const struct hdmi_codec_pdata *p=data;
 assert(!strcmp(name,HDMI_CODEC_DRV_NAME) && id==-1 && size==sizeof(*p));
 assert(p->ops && p->i2s==1 && !p->spdif && p->max_i2s_channels==8 && p->no_capture_mute);
 codec_creates++;return codec_error?ERR_PTR(codec_error):&codec;
}
static void platform_device_unregister(void *p) { assert(p==&codec);codec_removes++; }
static int devm_add_action_or_reset(struct device *dev,void (*action)(void *),void *data)
{ (void)dev;if(action_error){action(data);return action_error;}assert(!release_codec);release_codec=action;release_data=data;return 0; }
static void drm_bridge_add(struct drm_bridge *b) { assert(b==&allocated->bridge);bridges++; }
static void drm_bridge_remove(struct drm_bridge *b) { assert(b==&allocated->bridge);bridge_removes++; }
static int component_add(struct device *dev,const void *ops)
{ assert(dev->data==allocated && ops==&it66121_component_ops);components++;return component_error; }
static void component_del(struct device *dev,const void *ops)
{ assert(dev->data==allocated && ops==&it66121_component_ops);component_removes++; }
"""
checks = r"""
static void cleanup(void)
{
 if(release_codec){release_codec(release_data);release_codec=NULL;}
 free(allocated);allocated=NULL;
 regulator_error=gpio_error=irq_error=codec_error=action_error=component_error=read_error=0;
 supplies=resets=irq_requests=codec_creates=codec_removes=components=component_removes=bridges=bridge_removes=0;
 alignment=4;delay=1;property_error=0;transport=false;ready=has_audio=gpio_present=true;width=24;chip=ID_IT66121;
}
int main(void)
{
 struct i2c_client client={.dev={.of_node=&node},.adapter=&adapter};
 for(chip=0;chip<3;chip++)for(width=12;width<=24;width+=12)for(int irq=0;irq<2;irq++) {
  unsigned int saved_chip=chip,saved_width=width;
  client.irq=irq?37:0;
  assert(!it66121_probe(&client));
  assert(allocated->id==chip && allocated->bus_width==width && !allocated->aligned_i2c);
  assert(!allocated->reset_by_controller && !allocated->component_binding && !allocated->retry_irq_ack);
  assert(!allocated->input_clock_delay && !allocated->irq_output_mode);
  assert(allocated->bridge.of_node==&node && allocated->bridge.next_bridge==&downstream);
  assert(supplies==1 && resets==2 && irq_requests==irq && codec_creates==1 && release_codec);
  assert(!!(allocated->bridge.ops&DRM_BRIDGE_OP_HPD)==!!irq && !components && bridges==1);
  it66121_remove(&client);assert(bridge_removes==1 && !component_removes);
  release_codec(release_data);release_codec=NULL;assert(codec_removes==1);
  cleanup();chip=saved_chip;width=saved_width;
 }
 chip=ID_IT66121;width=24;client.irq=37;
 has_audio=false;gpio_present=false;assert(!it66121_probe(&client));
 assert(!codec_creates && !resets);cleanup();
 ready=false;assert(it66121_probe(&client)==-EPROBE_DEFER && !bridges);cleanup();
 width=8;assert(it66121_probe(&client)==-EINVAL && !supplies);cleanup();
 regulator_error=-EIO;assert(it66121_probe(&client)==-EIO && !resets);cleanup();
 gpio_error=-EBUSY;assert(it66121_probe(&client)==-EBUSY && !resets);cleanup();
 read_error=-EIO;assert(it66121_probe(&client)==-EIO && !bridges);cleanup();
 chip=3;assert(it66121_probe(&client)==-ENODEV && !bridges);cleanup();
 irq_error=-EBUSY;assert(it66121_probe(&client)==-EBUSY && !bridges && !codec_creates);cleanup();
 codec_error=-ENOMEM;assert(it66121_probe(&client)==-ENOMEM && !bridges);cleanup();
 action_error=-ENOMEM;assert(it66121_probe(&client)==-ENOMEM && codec_removes==1 && !release_codec);cleanup();
 client.dev.of_node=NULL;
 assert(it66121_probe(&client)==-EINVAL && !components && !supplies);cleanup();
 transport=true;assert(!it66121_probe(&client));
 assert(allocated->aligned_i2c && allocated->bus_width==24 && components==1 && bridges==1);
 assert(!supplies && !resets && !irq_requests && !codec_creates);
 assert(allocated->reset_by_controller && allocated->component_binding && allocated->retry_irq_ack);
 assert(allocated->input_clock_delay==IT66121_INPUT_MODE_PCLK_DELAY1 && !allocated->irq_output_mode);
 it66121_remove(&client);assert(component_removes==1 && bridge_removes==1);cleanup();
 transport=true;alignment=2;assert(it66121_probe(&client)==-EINVAL && !bridges);cleanup();
 transport=true;delay=4;assert(it66121_probe(&client)==-EINVAL && !bridges);cleanup();
 transport=true;width=8;assert(it66121_probe(&client)==-EINVAL && !bridges);cleanup();
 transport=true;property_error=-EIO;assert(it66121_probe(&client)==-EIO && !bridges);cleanup();
 transport=true;alignment=1;delay=0;width=12;assert(!it66121_probe(&client));
 assert(!allocated->aligned_i2c && !allocated->input_clock_delay && allocated->bus_width==12);
 assert(allocated->component_binding && allocated->reset_by_controller);cleanup();
 transport=true;component_error=-EPROBE_DEFER;
 assert(it66121_probe(&client)==-EPROBE_DEFER && bridges==1 && bridge_removes==1);cleanup();
 puts("PASS: firmware graph, three chips, both bus widths, native/absent IRQ, power/reset, managed audio, probe failures, explicit adapter properties and teardown");
}
"""
with tempfile.TemporaryDirectory(prefix="it66121-probe-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    names = (
        "it66121_audio_unregister",
        "it66121_audio_codec_init",
        "it66121_init_regmap",
        "it66121_probe_firmware",
        "it66121_probe_properties",
        "it66121_probe",
        "it66121_remove",
    )
    c.write_text(
        "\n\n".join(
            [
                prelude,
                '#include "bridge/it66121_registers.h"',
                structure(source, "it66121_chip_info"),
                structure(source, "it66121_ctx"),
                mocks,
                *[function(source, name) for name in names],
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
            "-Wno-misleading-indentation",
            "-I",
            str(ROOT),
            "-fsanitize=address,undefined",
            "-no-pie",
            str(c),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
