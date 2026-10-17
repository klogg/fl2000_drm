#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise actual EDID helpers against a side-effecting register model."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function

ROOT = Path(__file__).resolve().parents[1]

source = (
    (ROOT / "bridge/it66121_drv.c")
    .read_text()
    .replace("struct it66121_ctx", "struct it66121_priv")
)
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "bridge/it66121_registers.h"
#define EDID_SLEEP 1000
#define EDID_TIMEOUT 200000
#define EDID_FIFO_SIZE 32
#define FL2000_EDID_FIFO_SKIP 3
#define EDID_DDC_ADDR IT66121_DDC_HEADER_EDID
#define IT66121_DDC_CONTROL IT66121_MASTER_SEL_REG
#define IT66121_DDC_SEGMENT IT66121_DDC_SEGMENT_REG
#define IT66121_DDC_MASTER_HOST IT66121_MASTER_SEL_HOST
#define DDC_CMD_FIFO_CLEAR IT66121_DDC_COMMAND_FIFO_CLR
#define DDC_CMD_ABORT IT66121_DDC_COMMAND_ABORT
#define DDC_CMD_EDID_READ IT66121_DDC_COMMAND_EDID_READ
typedef long long ktime_t;
#define dev_dbg(...) ((void)0)
#define BIT(n) (1U << (n))
#define GENMASK(h,l) (((1U << ((h)+1))-1) & ~((1U << (l))-1))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define I2C_M_RD 1
struct i2c_adapter { int unused; };
struct i2c_client { unsigned short addr; };
struct i2c_msg { unsigned short addr, flags, len; u8 *buf; };
struct regmap { int unused; };
struct it66121_priv { struct i2c_client *client; struct i2c_adapter *adapter; struct regmap *regmap; bool fl2000; };
static long long now;
static ktime_t ktime_get(void) { now += 100; return now; }
static ktime_t ktime_add_us(ktime_t t, int us) { return t + us; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a>b)-(a<b); }
static void usleep_range(int a, int b) { (void)b; now += a; }
static int command, offset, count, segment, consumed, polls, fault, latency;
static u8 expected(unsigned int address) { return (address * 37 + 19) & 255; }
static int i2c_transfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int n)
{
 (void)adapter;
 if (fault == 4) return -ENODEV;
 if (fault == 5) return 0;
 if (n == 1) {
  assert(msgs[0].len == 5);
  u8 *b = msgs[0].buf;
  if (b[0] == 0x10) { assert(b[1] == 1 && b[2] == 0xa0); offset=b[3]; count=b[4]; }
  else { assert(b[0] == 0x14 && !b[3] && !b[4]); segment=b[1]; command=b[2]; consumed=polls=0; }
  return 1;
 }
 assert(n == 2 && msgs[0].len == 1 && msgs[0].buf[0] == 0x14);
 assert(msgs[1].len == 4 && msgs[1].flags == I2C_M_RD);
 now += latency;
 u8 *b=msgs[1].buf;
 b[0]=segment; b[1]=command; b[3]=0xee;
 if (command != 3) { b[2]=0x82; return 2; }
 polls++;
 if (fault == 1) { b[2]=0x20; return 2; }
 if (fault == 2) { b[2]=2; return 2; }
 if (fault == 3 && consumed == count-4) { b[2]=0x82; return 2; }
 /* Empty polls, then data before DONE, then remaining data with DONE. */
 if (polls <= 2) { b[2]=2; return 2; }
 b[2] = polls < 5 ? 0 : 0x80;
 if (consumed == count-3) b[2] |= 2;
 else b[3]=expected(segment*256 + ((offset + 3 + consumed++) & 255));
 return 2;
}
"""
native_stubs = r"""
static int regmap_read(struct regmap *map,unsigned int reg,unsigned int *value)
{ (void)map;(void)reg;(void)value;return -EOPNOTSUPP; }
static int regmap_write(struct regmap *map,unsigned int reg,unsigned int value)
{ (void)map;(void)reg;(void)value;return -EOPNOTSUPP; }
static int regmap_noinc_read(struct regmap *map,unsigned int reg,u8 *buffer,size_t count)
{ (void)map;(void)reg;(void)buffer;(void)count;return -EOPNOTSUPP; }
#define regmap_read_poll_timeout(map,reg,value,condition,delay,timeout) \
 ((void)(map),(void)(reg),(void)(delay),(void)(timeout),(value)=0,(void)(condition),-EOPNOTSUPP)
"""
common_names = (
    "it66121_preamble_ddc",
    "it66121_ddc_snapshot",
    "it66121_ddc_write",
    "it66121_ddc_collect",
    "it66121_wait_ddc_ready",
    "it66121_ddc_command",
    "it66121_abort_ddc_ops",
    "it66121_ddc_prepare",
    "it66121_ddc_set_request",
    "it66121_ddc_read_fifo",
    "it66121_get_edid_block",
)
checks = r"""
int main(void)
{
 struct i2c_client client={.addr=0x4c};
 struct i2c_adapter adapter={0};
 struct it66121_priv priv={.client=&client,.adapter=&adapter,.fl2000=true};
 u8 buffer[128];
 for (unsigned int block=0; block<6; block++) {
  assert(!it66121_get_edid_block(&priv,buffer,block,sizeof(buffer)));
  for (unsigned int i=0;i<128;i++) assert(buffer[i]==expected(block*128+i));
 }
 latency=12000;
 assert(!it66121_get_edid_block(&priv,buffer,0,128));
 for(unsigned int i=0;i<128;i++)assert(buffer[i]==expected(i));
 latency=0;
 assert(!it66121_get_edid_block(&priv,buffer,511,1));
 assert(it66121_get_edid_block(&priv,buffer,512,128)==-EINVAL);
 assert(it66121_get_edid_block(&priv,buffer,0,129)==-EINVAL);
 assert(it66121_get_edid_block(&priv,buffer,0,0)==-EINVAL);
 const int errors[]={0,-EIO,-ETIMEDOUT,-EIO,-ENODEV,-EIO};
 for (fault=1;fault<=5;fault++) {
  buffer[127]=0xa5;
  assert(it66121_get_edid_block(&priv,buffer,2,128)==errors[fault]);
  assert(buffer[127]==0xa5);
 }
 puts("PASS: EDID FIFO side effects, early data, empty polls, segments, timeout/error/short transfers");
}
"""
parts = [
    prelude,
    native_stubs,
    *[function(source, name) for name in common_names],
    checks,
]
with tempfile.TemporaryDirectory(prefix="fl2000-edid-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text("\n\n".join(parts))
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-I",
            str(ROOT),
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


# Ordinary I2C controllers must not inherit the FL2000 FIFO compensation.
generic_prelude = r"""
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "bridge/it66121_registers.h"
#define BIT(n) (1U << (n))
struct regmap { u8 values[256]; };
struct i2c_adapter { int unused; };
struct i2c_client { unsigned short addr; };
struct i2c_msg { unsigned short addr,flags,len; u8 *buf; };
struct it66121_priv { struct regmap *regmap; struct i2c_client *client; struct i2c_adapter *adapter; bool fl2000; };
typedef long long ktime_t;
#define dev_dbg(...) ((void)0)
#define I2C_M_RD 1
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GENMASK(h,l) (((1U << ((h)+1))-1) & ~((1U << (l))-1))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define EDID_SLEEP 1000
#define EDID_TIMEOUT 200000
#define EDID_FIFO_SIZE 32
#define FL2000_EDID_FIFO_SKIP 3
#define EDID_DDC_ADDR IT66121_DDC_HEADER_EDID
#define IT66121_DDC_CONTROL IT66121_MASTER_SEL_REG
#define IT66121_DDC_SEGMENT IT66121_DDC_SEGMENT_REG
#define IT66121_DDC_MASTER_HOST IT66121_MASTER_SEL_HOST
#define DDC_CMD_FIFO_CLEAR IT66121_DDC_COMMAND_FIFO_CLR
#define DDC_CMD_ABORT IT66121_DDC_COMMAND_ABORT
#define DDC_CMD_EDID_READ IT66121_DDC_COMMAND_EDID_READ
static ktime_t ktime_get(void) { return 0; }
static ktime_t ktime_add_us(ktime_t time,int us) { return time+us; }
static int ktime_compare(ktime_t a,ktime_t b) { return (a>b)-(a<b); }
static void usleep_range(int a,int b) { (void)a;(void)b; }
static int i2c_transfer(struct i2c_adapter *adapter,struct i2c_msg *messages,int count)
{ (void)adapter;(void)messages;(void)count;return -EOPNOTSUPP; }
static int fault=-1, fifo_error, fifo_reads;
static u8 expected(unsigned int address) { return (address*37+19)&255; }
static int regmap_write(struct regmap *map,unsigned int reg,unsigned int value)
{ if((int)reg==fault)return -EIO;map->values[reg]=value;return 0; }
static int regmap_read(struct regmap *map,unsigned int reg,unsigned int *value)
{ if((int)reg==fault)return -EIO;*value=reg==0x16?0x80:map->values[reg];return 0; }
#define regmap_read_poll_timeout(map,reg,value,condition,delay,timeout) \
 ({ assert((delay)==20000 && (timeout)==200000);int r=regmap_read(map,reg,(unsigned int *)&value); \
 r?r:((condition)?0:-ETIMEDOUT); })
static int regmap_noinc_read(struct regmap *map,unsigned int reg,u8 *buffer,unsigned int count)
{
 assert(reg==0x17 && count==map->values[0x13] && count<=32);
 if(fifo_error)return -EIO;
 for(unsigned int i=0;i<count;i++)buffer[i]=expected(map->values[0x14]*256+map->values[0x12]+i);
 fifo_reads++;return 0;
}
"""
generic_checks = r"""
int main(void) {
 struct regmap map={0};struct it66121_priv ctx={.regmap=&map};u8 buffer[128];
 for(unsigned int block=0;block<6;block++) {
  assert(!it66121_preamble_ddc(&ctx));
  assert(!regmap_write(&map,IT66121_DDC_HEADER_REG,IT66121_DDC_HEADER_EDID));
  assert(!it66121_get_edid_block(&ctx,buffer,block,sizeof(buffer)));
  for(unsigned int i=0;i<sizeof(buffer);i++)assert(buffer[i]==expected(block*128+i));
 }
 assert(fifo_reads==24);
 fault=IT66121_DDC_OFFSET_REG;
 assert(it66121_get_edid_block(&ctx,buffer,0,sizeof(buffer))==-EIO);
 fault=-1;fifo_error=1;
 assert(it66121_get_edid_block(&ctx,buffer,0,sizeof(buffer))==-EIO);
 fifo_error=0;map.values[IT66121_HDCP_REG]=0xff;
 assert(!it66121_abort_ddc_ops(&ctx));
 assert(map.values[IT66121_HDCP_REG]==0xfe);
 assert(map.values[IT66121_SW_RST_REG]&IT66121_SW_RST_HDCP);
 puts("PASS: ordinary-I2C EDID blocks, segments, native FIFO reads, I/O failures and DDC recovery");
}
"""
with tempfile.TemporaryDirectory(prefix="it66121-generic-edid-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    names = common_names
    c.write_text(
        "\n\n".join(
            [
                generic_prelude,
                *[function(source, name) for name in names],
                generic_checks,
            ]
        )
    )
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-I",
            str(ROOT),
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-no-pie",
            str(c),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
