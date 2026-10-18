#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check register words, self-clear status handling, I2C commands, and USB byte order."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, register_header, structure

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "fl2000_registers.c").read_text()
header = (ROOT / "fl2000.h").read_text()
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t __le32;
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define I2C_RDWR_INTERVAL 200
#define I2C_RDWR_TIMEOUT 256000
#define I2C_LOCK_ROOT_ADAPTER 1
#define FL2000_HW_RST_MDELAY 10
#define FL2000_MONITOR_DETECT_MDELAY 750
#define GFP_KERNEL 0
#define GFP_DMA 0
#define kmalloc_obj(v,f) malloc(sizeof(v))
#define kfree(p) free(p)
#define le32_to_cpu(v) le32toh(v)
#define cpu_to_le32(v) htole32(v)
#define CONTROL_MSG_READ 64
#define CONTROL_MSG_WRITE 65
#define USB_DIR_IN 0x80
#define USB_DIR_OUT 0
#define USB_TYPE_VENDOR 0x40
#define USB_CTRL_GET_TIMEOUT 1000
#define USB_CTRL_SET_TIMEOUT 1000
#define dev_dbg(...) ((void)0)
/* Opaque USB/I2C contexts retain the native ABI in the storage-order test. */
#ifdef TEST_BIG_ENDIAN_STORAGE
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define NATIVE_STORAGE __attribute__((scalar_storage_order("little-endian")))
#else
#define NATIVE_STORAGE __attribute__((scalar_storage_order("big-endian")))
#endif
#else
#define NATIVE_STORAGE
#endif
struct device { int unused; } NATIVE_STORAGE;
struct usb_device { struct device dev; } NATIVE_STORAGE;
struct i2c_adapter { int unused; } NATIVE_STORAGE;
struct regmap { u32 regs[0x10000/4]; };
struct op { char type; unsigned int reg; u32 mask,value; };
static struct regmap map;
static struct op ops[16];
static unsigned int count,fail_at;
static int poll_error,wire_result=4;
static u32 poll_status;
static unsigned char wire[4];
static struct regmap *dev_get_regmap(struct device *d,const char *n) { (void)d;(void)n;return &map; }
static struct i2c_adapter *fl2000_i2c_adapter(struct usb_device *u) { (void)u;return NULL; }
static void i2c_lock_bus(struct i2c_adapter *a,int f) { (void)a;(void)f;assert(false); }
static void i2c_unlock_bus(struct i2c_adapter *a,int f) { (void)a;(void)f;assert(false); }
static void msleep(unsigned int ms) { assert(ms==10 || ms==750); }
static void regcache_drop_region(struct regmap *m,unsigned int a,unsigned int b) { (void)m;assert(!a && b==0xffff); }
static int record(char type,unsigned int reg,u32 mask,u32 value)
{ assert(count<16);ops[count++]=(struct op){type,reg,mask,value};return count==fail_at?-EIO:0; }
static int regmap_write(struct regmap *m,unsigned int reg,u32 value)
{ int ret=record('w',reg,UINT32_MAX,value);if(!ret)m->regs[reg/4]=value;return ret; }
static int regmap_write_bits(struct regmap *m,unsigned int reg,u32 mask,u32 value)
{ int ret=record('b',reg,mask,value);if(!ret)m->regs[reg/4]=(m->regs[reg/4]&~mask)|(value&mask);return ret; }
static int regmap_read(struct regmap *m,unsigned int reg,u32 *value)
{
 int ret=record('r',reg,0,0);if(ret)return ret;
 *value=reg==0x8020?poll_status:m->regs[reg/4];return 0;
}
#define regmap_read_poll_timeout(m,r,v,condition,delay,timeout) \
 ({ int err=regmap_read(m,r,&(v)); (void)(delay);(void)(timeout); \
    err?err:poll_error?poll_error:(condition)?0:-ETIMEDOUT; })
static int control_message(struct usb_device *u,int pipe,int request,int type,int value,
                          u16 offset,void *buffer,size_t size,int timeout)
{
 (void)u;(void)pipe;(void)type;(void)value;(void)offset;(void)timeout;assert(size==4);
 if(wire_result>0) {
  size_t len=wire_result<4?(size_t)wire_result:4;
  if(request==CONTROL_MSG_READ)memcpy(buffer,wire,len);else memcpy(wire,buffer,len);
 }
 return wire_result;
}
static int usb_control_msg_recv(struct usb_device *usb, int ep, int request, int type,
                               int value, u16 offset, void *data, size_t size,
                               int timeout, int flags)
{
 (void)flags;
 int ret = control_message(usb, ep, request, type, value, offset, data, size, timeout);
 return ret < 0 ? ret : ret == (int)size ? 0 : -EREMOTEIO;
}
static int usb_control_msg_send(struct usb_device *usb, int ep, int request, int type,
                               int value, u16 offset, const void *data, size_t size,
                               int timeout, int flags)
{
 (void)flags;
 int ret = control_message(usb, ep, request, type, value, offset, (void *)data,
                           size, timeout);
 return ret < 0 ? ret : 0;
}

static void expect(unsigned int index,char type,unsigned int reg,u32 mask,u32 value)
{ assert(index<count && ops[index].type==type && ops[index].reg==reg && ops[index].mask==mask && ops[index].value==value); }
"""
checks = r"""
int main(void)
{
#ifdef TEST_BIG_ENDIAN_STORAGE
 struct { uint32_t word; } stored={.word=0x12345678};
 unsigned char storage[4];memcpy(storage,&stored,sizeof(storage));
 assert(storage[0]==0x12 && storage[1]==0x34 && storage[2]==0x56 && storage[3]==0x78);
#endif
 /* Independent manual bit positions, including the previously reversed labels. */
 assert(FL2000_VGA_STATUS_LBUF_OVERFLOW==0x200);
 assert(FL2000_VGA_STATUS_LBUF_UNDERFLOW==0x100);
 assert(FL2000_VGA_TEST_PATTERN_MODE==0x0e && FL2000_VGA_TEST_PATTERN_EN==1);
 assert(FL2000_VGA_PLL_PRE_DIV==0xff00);
 assert((FL2000_VGA_CTRL3_EXT_MON_ACTIVE_HIGH | FL2000_VGA_CTRL3_EEPROM_SLOW_CLK |
         FL2000_VGA_CTRL3_GPIO_OUTPUT | FL2000_VGA_CTRL3_WAKEUP_CLR_EN |
         FL2000_VGA_CTRL3_EEPROM_BLOCKS | FL2000_VGA_CTRL3_SPI_CLK_SELECT |
         FL2000_VGA_CTRL3_SPI_SLOW_READ | FL2000_VGA_CTRL3_SPI_INIT_EN |
         FL2000_VGA_CTRL3_TEST_BUS_SELECT)==0xffff);
 struct usb_device usb={0};
 struct fl2000_pll pll={.prescaler=2,.multiplier=50,.divisor=10,.function=1};
 map.regs[0x803c/4]=UINT32_MAX;
 assert(!fl2000_set_pll(&usb,&pll) && count==2);
 expect(0,'w',0x802c,UINT32_MAX,0x0032220a);
 expect(1,'b',0x803c,0x06000000,0x04000000);
 assert(map.regs[0x803c/4]==0xfdffffff);
 struct fl2000_timings timings={.hactive=800,.htotal=1056,.hsync_width=128,.hstart=216,
  .vactive=600,.vtotal=628,.vsync_width=4,.vstart=27,.hsync_positive=true};
 count=0;assert(!fl2000_set_timings(&usb,&timings) && count==5);
 expect(0,'w',0x8008,UINT32_MAX,0x03200420);
 expect(1,'w',0x800c,UINT32_MAX,0x008000d8);
 expect(2,'w',0x8010,UINT32_MAX,0x02580274);
 expect(3,'w',0x8014,UINT32_MAX,0x01b4001b);
 expect(4,'b',0x8004,0xc,4);
 timings.vstart=2048;count=0;assert(!fl2000_set_timings(&usb,&timings));
 expect(3,'w',0x8014,UINT32_MAX,0x3ff40800);
 for(unsigned int bytes=2;bytes<=3;bytes++) {
  count=0;assert(!fl2000_set_pixfmt(&usb,bytes) && count==1);
  expect(0,'b',0x8004,0x830020c1,bytes==2?0xc1:0x81);
 }
 count=0;assert(!fl2000_set_transfers(&usb) && count==2);
 expect(0,'b',0x803c,0x39000000,0x11000000);
 expect(1,'b',0x801c,0x3fff0000,0);
 count=0;assert(!fl2000_reset(&usb) && count==1);
 expect(0,'b',0x8048,0x8000,0x8000);
 count=0;assert(!fl2000_afe_magic(&usb) && count==1);
 expect(0,'b',0x70,0x2000,0x2000);
 count=0;assert(!fl2000_usb_magic(&usb) && count==6);
 expect(0,'b',0x8020,0x50000000,0);
 expect(1,'b',0x8020,0x50000000,0x40000000);
 expect(2,'b',0x8020,0x50000000,0x50000000);
 expect(3,'b',0x8088,0x400,0);
 expect(4,'b',0x70,0x180000,0x180000);
 expect(5,'b',0x78,0x20000,0);
 count=0;assert(!fl2000_enable_interrupts(&usb) && count==2);
 expect(0,'b',0x803c,0x0120201c,0x0120000c);
 expect(1,'b',0x8064,0x180000,0x80000);

 /* Read the precious status register once; write only observed LBUF errors. */
 for(unsigned int bit=0;bit<32;bit++) {
  count=0;map.regs[0x8000/4]=UINT32_C(1)<<bit;
  assert(fl2000_check_interrupt(&usb)==(bit==26 || bit==30 || bit==31));
  assert(count==(bit==8 || bit==9?2U:1U));
  expect(0,'r',0x8000,0,0);
  if(count==2)expect(1,'w',0x8000,UINT32_MAX,UINT32_C(1)<<bit);
 }
 count=0;map.regs[0x8000/4]=0x84000300;
 assert(fl2000_check_interrupt(&usb)==1 && count==2);
 expect(1,'w',0x8000,UINT32_MAX,0x300);

 /* I2C command fields and status decoding, preserving unrelated SC bits. */
 u32 data=0x12345678;map.regs[0x8020/4]=0x205a0000;map.regs[0x8024/4]=0x89abcdef;
 poll_status=0x80000000;count=0;
 assert(!fl2000_i2c_dword(&usb,true,0x4c,0x18,&data) && count==3 && data==0x89abcdef);
 expect(0,'b',0x8020,0xdf00ffff,0x500018cc);
 assert(map.regs[0x8020/4]==0x705a18cc);
 expect(1,'r',0x8020,0,0);expect(2,'r',0x8024,0,0);
 count=0;data=0x12345678;
 assert(!fl2000_i2c_dword(&usb,false,0x4c,0x18,&data) && count==3);
 expect(0,'w',0x8028,UINT32_MAX,0x12345678);
 expect(1,'b',0x8020,0xdf00ffff,0x5000184c);
 for(unsigned int status=1;status<16;status++) {
  poll_status=0x80000000|(status<<24);count=0;data=0x12345678;
  assert(fl2000_i2c_dword(&usb,true,0x4c,0x18,&data)==-EIO && count==2 && data==0x12345678);
 }
 poll_status=0x80000000;poll_error=-ETIMEDOUT;count=0;
 assert(fl2000_i2c_dword(&usb,true,0x4c,0x18,&data)==-ETIMEDOUT);
 poll_error=0;
 for(unsigned int failure=1;failure<=3;failure++) {
  count=0;fail_at=failure;
  assert(fl2000_i2c_dword(&usb,true,0x4c,0x18,&data)==-EIO && count==failure);
  count=0;
  assert(fl2000_i2c_dword(&usb,false,0x4c,0x18,&data)==-EIO && count==failure);
 }
 fail_at=0;

 /* The custom USB callbacks always exchange little-endian register words. */
 unsigned char expected[]={0xef,0xcd,0xab,0x89};
 assert(!fl2000_reg_write(&usb,0x802c,0x89abcdef) && !memcmp(wire,expected,4));
 data=0;assert(!fl2000_reg_read(&usb,0x802c,&data) && data==0x89abcdef);
 for(int length=0;length<4;length++) {
  wire_result=length;data=0x12345678;
  assert(fl2000_reg_read(&usb,0x802c,&data)==-EREMOTEIO && data==0x12345678);
  assert(!fl2000_reg_write(&usb,0x802c,0x89abcdef));
 }
 wire_result=-ENODEV;assert(fl2000_reg_read(&usb,0x802c,&data)==-ENODEV);
 assert(fl2000_reg_write(&usb,0x802c,0x89abcdef)==-ENODEV);
 puts("PASS: register golden words, timing fields, all status bits, I2C commands/errors, reserved bits, little-endian USB words");
 return 0;
}
"""
names = (
    "fl2000_set_pll",
    "fl2000_set_timings",
    "fl2000_set_pixfmt",
    "fl2000_set_transfers",
    "fl2000_reset",
    "fl2000_afe_magic",
    "fl2000_usb_magic",
    "fl2000_enable_interrupts",
    "fl2000_check_interrupt",
    "fl2000_i2c_dword",
    "fl2000_reg_read",
    "fl2000_reg_write",
)
parts = [
    prelude,
    '#include "tests/register_helpers.h"',
    register_header((ROOT / "fl2000_registers.h").read_text()),
    structure(header, "fl2000_pll"),
    structure(header, "fl2000_timings"),
    *(function(source, name) for name in names),
    checks,
]
with tempfile.TemporaryDirectory(prefix="fl2000-registers-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text("\n".join(parts))
    command = [
        "cc",
        "-std=gnu11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-no-pie",
        "-I",
        str(ROOT),
        str(c),
        "-o",
        str(exe),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)
    probe = subprocess.run(
        ["cc", "-Werror", "-fsso-struct=big-endian", "-x", "c", "-fsyntax-only", "-"],
        input="struct sample { unsigned int word; };\n",
        check=False,
        text=True,
        capture_output=True,
    )
    if probe.returncode == 0:
        subprocess.run(
            command + ["-fsso-struct=big-endian", "-DTEST_BIG_ENDIAN_STORAGE"],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
        print("PASS: register helpers with forced big-endian scalar storage")
    else:
        print("SKIP: compiler does not support forced big-endian scalar storage")
