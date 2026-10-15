#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise interface ownership and failure unwinding with synchronous USB callbacks."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, structure

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "fl2000_drv.c").read_text()
pre = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define __always_unused __attribute__((__unused__))
#define FL2000_USBIF_AVCONTROL 0
#define FL2000_USBIF_STREAMING 1
#define FL2000_USBIF_INTERRUPT 2
#define USB_SPEED_HIGH 3
#define GFP_KERNEL 0
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef int pm_message_t;
struct device { void *data; };
struct usb_host_interface { struct { unsigned int bInterfaceNumber; } desc; };
struct usb_device;
struct usb_interface {
 struct usb_device *usb;
 struct usb_host_interface *cur_altsetting;
 void *data;
 int owner;
};
struct usb_device { int speed; struct device dev; struct usb_interface *ifs[3]; };
struct usb_driver { int unused; };
struct usb_device_id { int unused; };
static struct usb_driver fl2000_driver;
static int fl2000_device_lock;
static void mutex_lock(int *lock) { assert(!*lock);*lock=1; }
static void mutex_unlock(int *lock) { assert(*lock);*lock=0; }
#define interface_to_usbdev(i) ((i)->usb)
#define dev_get_drvdata(d) ((d)->data)
#define dev_set_drvdata(d,v) ((d)->data=(v))
#define usb_get_intfdata(i) ((i)->data)
#define usb_set_intfdata(i,v) ((i)->data=(v))
static bool allocation_failure;
static void *devm_kzalloc(struct device *dev,size_t size,int flags)
{ (void)dev;(void)flags;return allocation_failure?NULL:calloc(1,size); }
static struct usb_interface *usb_ifnum_to_if(struct usb_device *usb,unsigned int n)
{ return usb->ifs[n]; }
static int fail_step,step,live,drm,release_count,suspend_count,resume_count;
static int destroyed[3],destroy_count;
static int operation(void) { return ++step==fail_step ? -EIO : 0; }
static int usb_driver_claim_interface(struct usb_driver *driver,struct usb_interface *i,void *data)
{
 (void)driver;if(!i)return -ENODEV;if(i->owner)return -EBUSY;
 i->owner=1;i->data=data;return 0;
}
static void fl2000_disconnect(struct usb_interface *i);
static void usb_driver_release_interface(struct usb_driver *driver,struct usb_interface *i)
{
 (void)driver;assert(i->owner==1);assert(!i->data);
 release_count++;fl2000_disconnect(i);i->owner=0;
}
static int usb_set_interface(struct usb_device *usb,int n,int alt)
{ assert(n==0 && alt==1);assert(usb->ifs[1]->owner==1 && usb->ifs[2]->owner==1);return operation(); }
static int fl2000_regmap_init(struct usb_device *usb)
{ (void)usb;int r=operation();if(!r)live|=1;return r; }
static int fl2000_reset(struct usb_device *usb) { (void)usb;return operation(); }
static int fl2000_init_clock(struct usb_device *usb) { (void)usb;return operation(); }
static int fl2000_usb_magic(struct usb_device *usb) { (void)usb;return operation(); }
static int fl2000_i2c_init(struct usb_device *usb)
{ (void)usb;int r=operation();if(!r)live|=2;return r; }
static void fl2000_i2c_cleanup(struct usb_device *usb)
{ (void)usb;assert(live&2);assert(!(live&12));live&=~2; }
static void fl2000_regmap_cleanup(struct usb_device *usb)
{ (void)usb;assert(live&1);assert(!(live&14));live&=~1; }
static int fl2000_streaming_create(struct usb_interface *i)
{ (void)i;int r=operation();assert((live&3)==3);if(!r)live|=4;return r; }
static int fl2000_interrupt_create(struct usb_interface *i)
{ (void)i;int r=operation();assert((live&7)==7);if(!r)live|=8;return r; }
static void fl2000_streaming_destroy(struct usb_interface *i)
{ (void)i;assert(live&4);assert(!(live&8));live&=~4;destroyed[destroy_count++]=1; }
static void fl2000_interrupt_destroy(struct usb_interface *i)
{ (void)i;assert(live&8);live&=~8;destroyed[destroy_count++]=2; }
static int fl2000_drm_init(struct usb_device *usb)
{ (void)usb;assert(live==15 && !drm);int r=operation();if(!r)drm=1;return r; }
static void fl2000_drm_cleanup(struct usb_device *usb) { (void)usb;assert(drm);drm=0; }
static void fl2000_disable_interrupt(struct usb_device *usb) { (void)usb;assert(!drm && (live&8)); }
static void fl2000_streaming_disable(struct usb_device *usb) { (void)usb;assert(!drm && (live&4)); }
static int fl2000_drm_suspend(struct usb_device *usb) { (void)usb;suspend_count++;return 0; }
static int fl2000_drm_resume(struct usb_device *usb) { (void)usb;resume_count++;return 0; }
"""
checks = r"""
int main(void)
{
 struct usb_device usb={.speed=USB_SPEED_HIGH};
 struct usb_interface interfaces[3];struct usb_host_interface alts[3];
 for(int i=0;i<3;i++) {
  alts[i].desc.bInterfaceNumber=i;
  interfaces[i]=(struct usb_interface){.usb=&usb,.cur_altsetting=&alts[i]};
  usb.ifs[i]=&interfaces[i];
 }
 /* Refuse unrelated interfaces and unavailable siblings before hardware I/O. */
 assert(fl2000_probe(&interfaces[1],NULL)==-ENODEV);
 allocation_failure=true;assert(fl2000_probe(&interfaces[0],NULL)==-ENOMEM);
 allocation_failure=false;
 for(int missing=1;missing<=2;missing++) {
  usb.ifs[missing]=NULL;
  assert(fl2000_probe(&interfaces[0],NULL)==-ENODEV);
  usb.ifs[missing]=&interfaces[missing];
  assert(!step && !live && !interfaces[0].data);
  for(int i=0;i<3;i++)assert(!interfaces[i].owner && !interfaces[i].data);
 }
 for(int busy=1;busy<=2;busy++) {
  interfaces[busy].owner=2;interfaces[busy].data=&usb;
  assert(fl2000_probe(&interfaces[0],NULL)==-EBUSY);
  assert(!step && interfaces[busy].owner==2 && interfaces[busy].data==&usb);
  interfaces[busy].owner=0;interfaces[busy].data=NULL;
  for(int i=0;i<3;i++)assert(!interfaces[i].owner && !interfaces[i].data);
 }
 /* Inject failure at every setup operation, then retry using the same state. */
 for(int failure=1;failure<=9;failure++) {
  fail_step=failure;step=destroy_count=0;
  assert(fl2000_probe(&interfaces[0],NULL)==-EIO);
  assert(step==failure && !live && !drm);
  for(int i=0;i<3;i++)assert(!interfaces[i].owner && !interfaces[i].data);
 }
 fail_step=0;
 /* Unbinding any interface tears down once and releases the other two. */
 for(int current=0;current<3;current++) {
  step=destroy_count=0;
  assert(!fl2000_probe(&interfaces[0],NULL));interfaces[0].owner=1;
  assert(live==15 && drm);
  for(int i=0;i<3;i++)assert(interfaces[i].owner==1 && interfaces[i].data);
  for(int i=0;i<3;i++)assert(!fl2000_suspend(&interfaces[i],0));
  assert(suspend_count==current+1);
  for(int i=2;i>=0;i--)assert(!fl2000_resume(&interfaces[i]));
  assert(resume_count==current+1);
  int before=release_count;
  fl2000_disconnect(&interfaces[current]);interfaces[current].owner=0;
  assert(release_count==before+2 && !live && !drm);
  assert(destroy_count==2 && destroyed[0]==2 && destroyed[1]==1);
  for(int i=0;i<3;i++) {
   assert(!interfaces[i].owner && !interfaces[i].data);
   fl2000_disconnect(&interfaces[i]);
  }
 }
 free(usb.dev.data);
 puts("PASS: interface claims, foreign ownership, missing siblings, all setup failures, retry, synchronous release callbacks, every unbind order and PM");
}
"""
parts = [pre, structure((ROOT / "fl2000.h").read_text(), "fl2000_device")]
parts += [
    function(source, name)
    for name in (
        "fl2000_destroy_interface",
        "fl2000_control_init",
        "fl2000_release_interfaces",
        "fl2000_cleanup",
        "fl2000_probe",
        "fl2000_disconnect",
        "fl2000_suspend",
        "fl2000_resume",
    )
]
with tempfile.TemporaryDirectory(prefix="fl2000-driver-") as directory:
    test = Path(directory) / "driver.c"
    binary = Path(directory) / "driver"
    test.write_text("\n".join([*parts, checks]))
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-fsanitize=address,undefined",
            "-fno-pie",
            "-no-pie",
            str(test),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
