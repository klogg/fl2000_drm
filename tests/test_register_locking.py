#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise reset and detection setup against concurrent I2C bus users."""

import re
import subprocess
import tempfile
from pathlib import Path

from source_helpers import function

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "fl2000_registers.c").read_text()
header = (ROOT / "fl2000.h").read_text()
mask = re.search(r"#define fl2000_add_bitmask.*?(?=\n\n)", header, re.DOTALL)[0]
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define __aligned(n) __attribute__((aligned(n)))
#define __packed __attribute__((packed))
#define FL2000_HW_RST_MDELAY 10
#define I2C_LOCK_ROOT_ADAPTER 1
struct i2c_adapter { pthread_mutex_t bus; };
struct regmap { struct i2c_adapter *adapter; int calls,fail_at,sleeps,drops; };
struct device { struct i2c_adapter *adapter; struct regmap *map; };
struct usb_device { struct device dev; };
static _Thread_local struct i2c_adapter *held;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int attempts;
static bool pause_delay,at_delay,release_delay,i2c_entered;
static void i2c_lock_bus(struct i2c_adapter *a,int flags)
{
 assert(flags==I2C_LOCK_ROOT_ADAPTER && !held);
 pthread_mutex_lock(&gate);attempts++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
 pthread_mutex_lock(&a->bus);held=a;
}
static void i2c_unlock_bus(struct i2c_adapter *a,int flags)
{ assert(flags==I2C_LOCK_ROOT_ADAPTER && held==a);held=NULL;pthread_mutex_unlock(&a->bus); }
static struct i2c_adapter *fl2000_i2c_adapter(struct usb_device *u) { return u->dev.adapter; }
static struct regmap *dev_get_regmap(struct device *d,const char *name) { assert(!name);return d->map; }
static _Thread_local struct regmap *current_map;
static int regmap_write_bits(struct regmap *m,unsigned int reg,u32 mask,u32 val)
{
 (void)reg;(void)mask;(void)val;assert(held==m->adapter);current_map=m;
 return ++m->calls==m->fail_at?-EIO:0;
}
static void msleep(unsigned int ms)
{
 assert(ms==10 || ms==750);assert(current_map && held==current_map->adapter);
 current_map->sleeps++;
 pthread_mutex_lock(&gate);
 if(pause_delay){at_delay=true;pthread_cond_broadcast(&changed);while(!release_delay)pthread_cond_wait(&changed,&gate);}
 pthread_mutex_unlock(&gate);
}
static void regcache_drop_region(struct regmap *m,unsigned int first,unsigned int last)
{ assert(!first && last==0xffff && held==m->adapter);m->drops++; }
"""
checks = r"""
struct job { struct usb_device *usb; bool magic; int result; };
static void *configure(void *arg)
{ struct job *j=arg;j->result=j->magic?fl2000_usb_magic(j->usb):fl2000_reset(j->usb);return NULL; }
static void *transaction(void *arg)
{
 struct usb_device *usb=arg;
 i2c_lock_bus(usb->dev.adapter,I2C_LOCK_ROOT_ADAPTER);
 pthread_mutex_lock(&gate);i2c_entered=true;pthread_mutex_unlock(&gate);
 i2c_unlock_bus(usb->dev.adapter,I2C_LOCK_ROOT_ADAPTER);return NULL;
}
static void wait_attempt(int count)
{ pthread_mutex_lock(&gate);while(attempts<count)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate); }
static void check_unlocked(struct i2c_adapter *adapter)
{ assert(!pthread_mutex_trylock(&adapter->bus));pthread_mutex_unlock(&adapter->bus); }
int main(void)
{
 struct i2c_adapter adapter={.bus=PTHREAD_MUTEX_INITIALIZER};
 struct regmap map={.adapter=&adapter};
 struct usb_device usb={.dev={.adapter=&adapter,.map=&map}};
 for(int magic=0;magic<2;magic++) {
  /* A complete in-flight I2C transfer excludes reset and setup writes. */
  map.calls=map.sleeps=map.drops=0;
  i2c_lock_bus(&adapter,I2C_LOCK_ROOT_ADAPTER);
  struct job job={.usb=&usb,.magic=magic};pthread_t worker;
  int next=attempts+1;
  assert(!pthread_create(&worker,NULL,configure,&job));wait_attempt(next);
  assert(!map.calls && !map.sleeps && !map.drops);
  i2c_unlock_bus(&adapter,I2C_LOCK_ROOT_ADAPTER);
  assert(!pthread_join(worker,NULL) && !job.result && map.calls && map.sleeps==1);
  assert(map.drops==!magic);check_unlocked(&adapter);

  /* The bus stays locked through settling delays and cache invalidation. */
  map.calls=map.sleeps=map.drops=0;
  pthread_mutex_lock(&gate);pause_delay=true;at_delay=release_delay=i2c_entered=false;pthread_mutex_unlock(&gate);
  assert(!pthread_create(&worker,NULL,configure,&job));
  pthread_mutex_lock(&gate);while(!at_delay)pthread_cond_wait(&changed,&gate);next=attempts+1;pthread_mutex_unlock(&gate);
  pthread_t transfer;assert(!pthread_create(&transfer,NULL,transaction,&usb));wait_attempt(next);
  pthread_mutex_lock(&gate);assert(!i2c_entered);release_delay=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
  assert(!pthread_join(worker,NULL) && !pthread_join(transfer,NULL) && !job.result && i2c_entered);
  pause_delay=false;check_unlocked(&adapter);

  int writes=map.calls;
  for(int failure=1;failure<=writes;failure++) {
   map.calls=map.sleeps=map.drops=0;map.fail_at=failure;
   assert((magic?fl2000_usb_magic(&usb):fl2000_reset(&usb))==-EIO);
   assert(map.calls==failure && !map.drops);check_unlocked(&adapter);
  }
  map.fail_at=0;
 }
 /* Initial probe precedes adapter registration and needs no bus lock. */
 usb.dev.adapter=map.adapter=NULL;map.calls=map.sleeps=map.drops=0;
 assert(!fl2000_reset(&usb) && map.drops==1);
 assert(!fl2000_usb_magic(&usb));
 puts("PASS: concurrent I2C exclusion, reset/setup delays, cache invalidation, every write failure, pre-adapter probe");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="fl2000-register-locking-") as temp:
    c = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    c.write_text(
        "\n".join([
            prelude,
            mask,
            '#include "fl2000_registers.h"',
            function(source, "fl2000_reset"),
            function(source, "fl2000_usb_magic"),
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
            "-pthread",
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
    subprocess.run([str(exe)], check=True, timeout=15)
