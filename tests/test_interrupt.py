#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise actual interrupt startup, recovery, and stop with mocked USB I/O."""

import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, structure

ROOT = Path(__file__).resolve().parents[1]

source = (ROOT / "fl2000_interrupt.c").read_text()
pre = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef unsigned char u8;
#define __always_unused __attribute__((__unused__))
struct device { int unused; };
typedef int spinlock_t;
struct mutex { int unused; };
struct work_struct { bool pending; };
struct delayed_work { struct work_struct work; unsigned long delay; };
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define system_wq NULL
#define msecs_to_jiffies(ms) (ms)
enum usb_device_state { USB_STATE_CONFIGURED, USB_STATE_NOTATTACHED };
struct usb_device { int dev; enum usb_device_state state; };
static inline bool fl2000_usb_unavailable(const struct usb_device *usb);
static inline bool fl2000_usb_error_terminal(int error);
struct urb { int status,pipe;void *context;bool submitted; };
#define FL2000_INTR_STALL_RETRIES 3
#define FL2000_INTR_STATUS_RETRY_MS 1000
#define dev_warn_ratelimited(...) (++logs)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define xchg(p,v) ({ __typeof__(*(p)) old=*(p);*(p)=(v);old; })
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define spin_lock_irqsave(lock,flags) ((void)(lock),(flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(lock),(void)(flags))
#define GFP_ATOMIC 1
static void (*mutex_hook)(void);
#define mutex_lock(p) ((void)(p), (mutex_hook ? mutex_hook() : (void)0))
#define mutex_unlock(p) ((void)(p))
struct fl2000_interrupt;
typedef int atomic_t;
#define atomic_read(p) (*(p))
struct fl2000_device { struct fl2000_interrupt *interrupt; atomic_t i2c_errors; bool native_vga; };
static struct fl2000_device device;
struct irq_domain { int unused; };
struct fwnode_handle { int unused; };
static int allocation_failure,allocation_step,live_domains,live_mappings;
static const int fl2000_irq_domain_ops;
#define GFP_KERNEL 0
#define dev_name(dev) ((void)(dev), "fl2000-test")
struct i2c_adapter { struct device dev; };
static struct i2c_adapter adapter;
static struct fwnode_handle node;
static struct i2c_adapter *fl2000_i2c_adapter(const struct usb_device *usb) { (void)usb;return &adapter; }
static struct fwnode_handle *dev_fwnode(struct device *dev) { assert(dev==&adapter.dev);return &node; }
static struct irq_domain *irq_domain_create_linear(struct fwnode_handle *node,int size,const void *ops,void *data)
{ assert(node && size==1 && ops==&fl2000_irq_domain_ops && data);if(++allocation_step==allocation_failure)return NULL;live_domains++;return calloc(1,sizeof(struct irq_domain)); }
static unsigned int irq_create_mapping(struct irq_domain *domain,int hwirq)
{ assert(domain && !hwirq);if(++allocation_step==allocation_failure)return 0;live_mappings++;return 37; }
static void irq_dispose_mapping(unsigned int irq) { assert(irq==37 && live_mappings==1);live_mappings--; }
static void irq_domain_remove(struct irq_domain *domain) { assert(domain && !live_mappings);live_domains--;free(domain); }
static void usb_free_urb(struct urb *urb) { assert(!urb || !urb->submitted); }
static void kfree(void *buf) { assert(!buf); }
#define resource device.interrupt
static void *dev_get_drvdata(const void *dev) { (void)dev;return &device; }
static int submissions,submit_error,clear_count,clear_error,status_reads,status_result,hotplugs,logs,transfer_logs;
static int poll_enables,poll_disables,hotplug_result,irq_calls;
static bool irq_error;
static bool polling_current;
static void (*submit_hook)(void);
static void fl2000_drm_set_polling(struct usb_device *usb,bool enabled)
{ (void)usb;polling_current=enabled;if(enabled)poll_enables++;else poll_disables++; }
static void (*clear_hook)(void);
static void record_error(const char *message,int error)
{
 assert(error<0);
 if(!strcmp(message,"Interrupt transfer failed: %d\n"))transfer_logs++;
 else logs++;
}
#define dev_err_ratelimited(dev,message,error) record_error(message,error)
static void msleep(int ms) { assert(ms==20); }
static int usb_clear_halt(struct usb_device *usb,int pipe)
{ (void)usb;assert(pipe==3);clear_count++;if(clear_hook)clear_hook();return clear_error; }
static int usb_submit_urb(struct urb *urb,int flags)
{ assert(flags==GFP_ATOMIC && !urb->submitted);submissions++;if(submit_error)return submit_error;urb->submitted=true;urb->status=-EINPROGRESS;if(submit_hook)submit_hook();return 0; }
static bool schedule_delayed_work(struct delayed_work *work,unsigned long delay)
{ if(work->work.pending)return false;work->work.pending=true;work->delay=delay;return true; }
static bool mod_delayed_work(void *queue,struct delayed_work *work,unsigned long delay)
{ (void)queue;bool pending=work->work.pending;work->work.pending=true;work->delay=delay;return pending; }
static void cancel_delayed_work_sync(struct delayed_work *work) { work->work.pending=false; }
static void (*status_hook)(void);
static int fl2000_check_interrupt(struct usb_device *usb) { (void)usb;status_reads++;if(status_hook)status_hook();return status_result; }
static void handle_nested_irq(unsigned int irq)
{ assert(irq==37);irq_calls++;if(irq_error)device.i2c_errors++; }
static int fl2000_drm_hotplug(struct usb_device *usb) { (void)usb;hotplugs++;return hotplug_result; }
struct irq_data { void *chip_data; };
static void *irq_data_get_irq_chip_data(struct irq_data *data) { return data->chip_data; }
static void fl2000_intr_completion(struct urb *urb);
static void usb_kill_urb(struct urb *urb)
{ if(urb && urb->submitted){int before=transfer_logs;urb->submitted=false;urb->status=-ECONNRESET;fl2000_intr_completion(urb);assert(transfer_logs==before+1);} }
"""
checks = r"""
static struct fl2000_interrupt *current;
static void disconnect_during_lock(void) { current->usb_dev->state=USB_STATE_NOTATTACHED; }
static void disable_during_clear(void) { current->enabled=false; }
static void run_work(struct fl2000_interrupt *intr) { intr->work.work.pending=false;fl2000_intr_work(&intr->work.work); }
static void immediate_failure(void)
{
 current->enabled=false;current->fallback=true;current->urb->submitted=false;
 current->work.work.pending=true;run_work(current);
}
static void complete(struct fl2000_interrupt *intr,int status)
{ int before=transfer_logs;intr->urb->submitted=false;intr->urb->status=status;fl2000_intr_completion(intr->urb);assert(transfer_logs==before+(status!=0)); }
static void shutdown_during_status(void) { complete(current,-ESHUTDOWN); }
int main(void)
{
 struct usb_device usb={0};struct urb urb={.pipe=3};
 for(allocation_failure=1;allocation_failure<=3;allocation_failure++) {
  struct fl2000_interrupt allocated={.usb_dev=&usb};
  allocation_step=0;
  assert(fl2000_intr_irq_init(&allocated)==(allocation_failure<=2?-ENOMEM:0));
  if(allocation_failure==3)assert(allocated.irq==37);
  fl2000_intr_release(NULL,&allocated);
  assert(!live_domains && !live_mappings);
 }
 struct fl2000_interrupt intr={.usb_dev=&usb,.urb=&urb,.irq=37,.enabled=true};
 urb.context=&intr;current=&intr;
 complete(&intr,-EPIPE);
 assert(intr.enabled && intr.stalled && intr.work.work.pending && !submissions);
 run_work(&intr);
 assert(intr.enabled && !intr.stalled && urb.submitted && clear_count==1 && submissions==1);
 complete(&intr,0);
 assert(!urb.submitted && intr.work.work.pending && !logs);
 int before_ack=submissions;
 status_result=1;run_work(&intr);
 assert(status_reads==1 && hotplugs==1 && urb.submitted && submissions==before_ack+1);
 fl2000_intr_stop(&intr);
 assert(!intr.enabled && !intr.stalled && !urb.submitted && !intr.work.work.pending && !logs);
 int count=submissions;run_work(&intr);assert(submissions==count && status_reads==1);
 intr.enabled=true;complete(&intr,-EPIPE);clear_error=-EIO;run_work(&intr);
 assert(!intr.enabled && !urb.submitted && logs==1);
 fl2000_intr_stop(&intr);
 /* Successful completion defers rearm, including submission failures. */
 intr.enabled=true;complete(&intr,0);count=submissions;
 submit_error=-ENOMEM;run_work(&intr);
 assert(!intr.enabled && !urb.submitted && submissions==count+1 && logs==2);
 fl2000_intr_stop(&intr);
 clear_error=0;submit_error=-ENOMEM;intr.enabled=true;complete(&intr,-EPIPE);run_work(&intr);
 assert(!intr.enabled && !urb.submitted && logs==3);
 submit_error=0;intr.enabled=true;complete(&intr,-EPIPE);fl2000_intr_stop(&intr);
 count=clear_count;run_work(&intr);assert(clear_count==count);
 intr.enabled=true;complete(&intr,-EPIPE);clear_hook=disable_during_clear;
 count=submissions;run_work(&intr);assert(submissions==count && !intr.enabled);
 clear_hook=NULL;fl2000_intr_stop(&intr);
 intr.enabled=true;
 int previous_logs=logs,previous_clears=clear_count;
 for(unsigned int i=0;i<=FL2000_INTR_STALL_RETRIES;i++) {
  complete(&intr,-EPIPE);
  if(i<FL2000_INTR_STALL_RETRIES)run_work(&intr);
 }
 assert(!intr.enabled && !urb.submitted && intr.work.work.pending);
 int previous_polls=poll_enables;run_work(&intr);
 assert(poll_enables==previous_polls+1 && !intr.work.work.pending);
 assert(clear_count==previous_clears+FL2000_INTR_STALL_RETRIES && logs==previous_logs+1);
 fl2000_intr_stop(&intr);
 /* Start checks/acknowledges stale status even without a USB completion. */
 resource=&intr;urb.status=0;
 previous_clears=clear_count;count=submissions;
 assert(fl2000_enable_interrupt(&usb)==0);
 assert(intr.enabled && urb.submitted && clear_count==previous_clears);
 assert(intr.work.work.pending);
 assert(submissions==count+1);
 int previous_reads=status_reads,previous_hotplugs=hotplugs;
 status_result=1;run_work(&intr);
 assert(status_reads==previous_reads+1 && hotplugs==previous_hotplugs+1);
 assert(fl2000_enable_interrupt(&usb)==0);
 assert(clear_count==previous_clears && submissions==count+1 && !intr.work.work.pending);
 fl2000_intr_stop(&intr);
 clear_error=-EIO;urb.status=-EPIPE;count=submissions;
 assert(fl2000_enable_interrupt(&usb)==-EIO);
 assert(!intr.enabled && !urb.submitted && submissions==count);
 clear_error=0;submit_error=-ENOMEM;
 assert(fl2000_enable_interrupt(&usb)==-ENOMEM);
 assert(!intr.enabled && !urb.submitted);
 submit_error=0;resource=NULL;previous_clears=clear_count;
 assert(fl2000_enable_interrupt(&usb)==-ENODEV);
 assert(clear_count==previous_clears);
 resource=&intr;intr.enabled=true;previous_polls=poll_enables;
 complete(&intr,-EIO);assert(intr.work.work.pending && !intr.enabled);
 run_work(&intr);assert(poll_enables==previous_polls+1);
 fl2000_intr_stop(&intr);previous_polls=poll_disables;urb.status=0;
 assert(!fl2000_enable_interrupt(&usb) && poll_disables==previous_polls+1);
 fl2000_intr_stop(&intr);previous_polls=poll_enables;
 run_work(&intr);assert(poll_enables==previous_polls);
 resource=&intr;urb.status=0;submit_hook=immediate_failure;
 assert(!fl2000_enable_interrupt(&usb));
 assert(!intr.enabled && polling_current);
 submit_hook=NULL;fl2000_intr_stop(&intr);
 /* Failed startup status enables polling and retries without another USB edge. */
 status_result=-EIO;hotplug_result=0;urb.status=0;
 assert(!fl2000_enable_interrupt(&usb));count=submissions;
 previous_polls=poll_enables;previous_reads=status_reads;
 run_work(&intr);
 assert(intr.enabled && intr.status_polling && polling_current);
 assert(poll_enables==previous_polls+1 && intr.work.work.pending);
 assert(intr.work.delay==FL2000_INTR_STATUS_RETRY_MS && submissions==count);
 run_work(&intr);
 assert(status_reads==previous_reads+2 && poll_enables==previous_polls+1);
 assert(intr.work.work.pending && intr.work.delay==FL2000_INTR_STATUS_RETRY_MS);
 /* A completion expedites retry, but errors must not rearm an unacked endpoint. */
 complete(&intr,0);assert(intr.work.delay==0);
 run_work(&intr);
 assert(!urb.submitted && intr.rearm && submissions==count && polling_current);
 /* Even a cleared FL2000 event must retry bridge acknowledgement. */
 status_result=0;hotplug_result=-EIO;previous_hotplugs=hotplugs;
 run_work(&intr);
 assert(hotplugs==previous_hotplugs+1 && intr.status_polling && polling_current);
 assert(intr.work.work.pending && !urb.submitted && intr.rearm);
 hotplug_result=0;previous_polls=poll_disables;run_work(&intr);
 assert(hotplugs==previous_hotplugs+2 && !intr.status_polling && !polling_current);
 assert(poll_disables==previous_polls+1 && urb.submitted && !intr.rearm);
 assert(submissions==count+1 && !intr.work.work.pending);
 /* Permanent failures remain delayed and stop cancels the outstanding timer. */
 complete(&intr,0);status_result=-ETIMEDOUT;run_work(&intr);
 assert(intr.status_polling && !urb.submitted && intr.work.work.pending);
 fl2000_intr_stop(&intr);previous_reads=status_reads;count=submissions;
 assert(!intr.status_polling && !intr.work.work.pending);
 run_work(&intr);assert(status_reads==previous_reads && submissions==count);
 /* Endpoint recovery must retain a pending status retry. */
 status_result=-EIO;urb.status=0;assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 complete(&intr,-EPIPE);run_work(&intr);
 assert(!urb.submitted && intr.rearm && intr.status_polling && intr.work.work.pending);
 assert(intr.work.delay==FL2000_INTR_STATUS_RETRY_MS);
 status_result=0;run_work(&intr);
 assert(urb.submitted && !intr.rearm && !intr.status_polling);
 assert(!polling_current && !intr.work.work.pending);
 fl2000_intr_stop(&intr);
 /* If rearm itself fails after status recovery, transport fallback wins. */
 urb.status=0;status_result=-EIO;assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 complete(&intr,0);run_work(&intr);
 status_result=0;submit_error=-ENOMEM;run_work(&intr);
 assert(!intr.enabled && polling_current && !intr.work.work.pending);
 submit_error=0;fl2000_intr_stop(&intr);
 /* Re-enabling an IRQ rescans even after its FL2000 event was cleared. */
 struct irq_data irq_data={.chip_data=&intr};
 status_result=0;urb.status=0;
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 int unmask_irqs=irq_calls;
 fl2000_irq_unmask(&irq_data);
 assert(intr.irq_rescan && intr.work.work.pending);
 run_work(&intr);
 assert(irq_calls==unmask_irqs+1 && !intr.irq_rescan && !intr.work.work.pending);
 fl2000_intr_stop(&intr);
 fl2000_irq_unmask(&irq_data);
 assert(intr.irq_rescan && !intr.work.work.pending);
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 assert(irq_calls==unmask_irqs+2 && !intr.irq_rescan);
 fl2000_intr_stop(&intr);
 /* Native VGA rescans even when detect consumed the event; there is no bridge IRQ. */
 device.native_vga=true;status_result=0;urb.status=0;
 int previous_irqs=irq_calls;
 previous_hotplugs=hotplugs;
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 complete(&intr,0);run_work(&intr);
 assert(hotplugs==previous_hotplugs+2 && irq_calls==previous_irqs && urb.submitted);
 complete(&intr,0);status_result=-EIO;run_work(&intr);
 assert(intr.status_polling && polling_current && irq_calls==previous_irqs);
 status_result=0;run_work(&intr);
 assert(!intr.status_polling && !polling_current && urb.submitted && irq_calls==previous_irqs);
 fl2000_intr_stop(&intr);device.native_vga=false;
 /* Acknowledgment can fail even when a later detect succeeds. */
 urb.status=0;status_result=1;hotplug_result=0;irq_error=true;
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 assert(intr.status_polling && polling_current && intr.work.work.pending);
 complete(&intr,0);run_work(&intr);
 assert(!urb.submitted && intr.rearm && intr.status_polling);
 previous_irqs=irq_calls;
 status_result=0;irq_error=false;run_work(&intr);
 assert(irq_calls==previous_irqs+1 && urb.submitted && !intr.status_polling);
 assert(!polling_current && !intr.work.work.pending);
 fl2000_intr_stop(&intr);
 /* Explicit restart must retry bridge acknowledgment after the USB event cleared. */
 urb.status=0;status_result=1;irq_error=true;
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 assert(intr.status_polling && intr.work.work.pending);
 fl2000_disable_interrupt(&usb);
 status_result=0;irq_error=false;previous_irqs=irq_calls;
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 assert(irq_calls==previous_irqs+1 && !intr.status_polling);
 assert(intr.enabled && urb.submitted && !intr.work.work.pending);
 fl2000_disable_interrupt(&usb);
 /* An unplugged DRM device is terminal even when USB is still present. */
 int gone[]={-ENODEV,-ESHUTDOWN};
 for(unsigned int i=0;i<sizeof(gone)/sizeof(gone[0]);i++) {
  int old_logs=logs,old_polls=poll_enables;
  intr.enabled=true;intr.rearm=true;status_result=1;hotplug_result=gone[i];
  run_work(&intr);
  assert(!intr.enabled && !intr.work.work.pending && !urb.submitted);
  assert(logs==old_logs+1 && poll_enables==old_polls);
  fl2000_intr_stop(&intr);
  assert(!fl2000_usb_unavailable(&usb)); /* DRM loss does not disconnect its USB transport. */
 }
 hotplug_result=0;
 /* Terminal cancellations never enable connector polling or retry. */
 const int terminal[]={-ENOENT,-ECONNRESET,-ESHUTDOWN,-ENODEV,-EPERM};
 const int completion_stop[]={-ENOENT,-ECONNRESET,-ESHUTDOWN,-ENODEV};
 for(unsigned int i=0;i<sizeof(completion_stop)/sizeof(completion_stop[0]);i++) {
  intr.enabled=true;int old_logs=logs,old_polls=poll_enables;
  complete(&intr,completion_stop[i]);
  assert(!intr.enabled && !intr.fallback && !intr.work.work.pending);
  run_work(&intr);assert(logs==old_logs && poll_enables==old_polls);
  assert(!fl2000_usb_unavailable(&usb));
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  fl2000_intr_stop(&intr);
 }
 /* Concurrent acknowledgment failures must preserve terminal detection errors. */
 for(unsigned int i=0;i<sizeof(terminal)/sizeof(terminal[0]);i++) {
  urb.status=0;status_result=1;irq_error=true;hotplug_result=terminal[i];
  int old_polls=poll_enables;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  assert(!intr.enabled && !intr.status_polling && !intr.work.work.pending);
  assert(poll_enables==old_polls);
  fl2000_disable_interrupt(&usb);irq_error=false;hotplug_result=0;
 }
 /* Terminal halt-clear failures stop stall recovery without polling or retry. */
 for(unsigned int i=0;i<sizeof(terminal)/sizeof(terminal[0]);i++) {
  urb.status=0;status_result=0;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  complete(&intr,-EPIPE);clear_error=terminal[i];
  int old_polls=poll_enables,old_logs=logs;count=submissions;
  run_work(&intr);
  assert(!intr.enabled && !intr.work.work.pending && !urb.submitted);
  assert(poll_enables==old_polls && logs==old_logs+1 && submissions==count);
  clear_error=0;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  assert(intr.enabled && urb.submitted);
  fl2000_disable_interrupt(&usb);
 }
 /* Terminal startup and worker submissions must not enable polling. */
 for(unsigned int i=0;i<sizeof(terminal)/sizeof(terminal[0]);i++) {
  int old_logs=logs,old_polls=poll_enables;
  submit_error=terminal[i];urb.status=0;
  assert(fl2000_enable_interrupt(&usb)==terminal[i] && !fl2000_usb_unavailable(&usb));
  assert(logs==old_logs && poll_enables==old_polls);
  fl2000_intr_stop(&intr);submit_error=0;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  complete(&intr,0);submit_error=terminal[i];run_work(&intr);
  assert(!fl2000_usb_unavailable(&usb) && !intr.enabled && !intr.work.work.pending);
  assert(logs==old_logs+1 && poll_enables==old_polls);
  submit_error=0;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  fl2000_intr_stop(&intr);
 }
 clear_error=-ENODEV;urb.status=-EPIPE;previous_polls=poll_enables;
 assert(fl2000_enable_interrupt(&usb)==-ENODEV && !fl2000_usb_unavailable(&usb));
 assert(poll_enables==previous_polls);
 fl2000_intr_stop(&intr);clear_error=0;urb.status=0;
 assert(!fl2000_enable_interrupt(&usb));fl2000_intr_stop(&intr);
 assert(!fl2000_usb_unavailable(&usb)); /* Intentional stop does not change USB core state. */
 /* A status read failure also stops locally and permits an explicit restart. */
 for(unsigned int i=0;i<sizeof(terminal)/sizeof(terminal[0]);i++) {
  status_result=terminal[i];previous_polls=poll_enables;previous_logs=logs;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  assert(!intr.enabled && !intr.work.work.pending && !fl2000_usb_unavailable(&usb));
  assert(poll_enables==previous_polls && logs==previous_logs+1);
  status_result=0;
  assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
  fl2000_intr_stop(&intr);
 }
 /* Disconnect drains queued work and URBs before resource teardown. */
 assert(!fl2000_enable_interrupt(&usb));
 assert(urb.submitted && intr.work.work.pending);
 usb.state=USB_STATE_NOTATTACHED;
 fl2000_disable_interrupt(&usb);
 int removal_logs=logs,removal_polls=poll_enables,removal_reads=status_reads;
 count=submissions;complete(&intr,0);run_work(&intr);
 assert(!intr.enabled && !intr.rearm && !intr.work.work.pending && !intr.fallback);
 assert(!urb.submitted);
 assert(fl2000_enable_interrupt(&usb)==-ENODEV && submissions==count);
 assert(logs==removal_logs && poll_enables==removal_polls && status_reads==removal_reads);
 /* Logging is independent of disconnect state; genuine errors stay visible. */
 previous_logs=transfer_logs;complete(&intr,-EIO);
 assert(transfer_logs==previous_logs+1 && !intr.enabled && !intr.fallback);
 /* Enable must check removal after acquiring the start/stop mutex. */
 usb.state=USB_STATE_CONFIGURED;mutex_hook=disconnect_during_lock;
 assert(fl2000_enable_interrupt(&usb)==-ENODEV && submissions==count);
 assert(!intr.enabled && !urb.submitted && !intr.work.work.pending);
 mutex_hook=NULL;
 usb.state=USB_STATE_CONFIGURED;

 /* Stop observed during halt clearing prevents fallback, even on I/O failure. */
 urb.status=0;status_result=0;
 assert(!fl2000_enable_interrupt(&usb));run_work(&intr);
 complete(&intr,-EPIPE);clear_error=-EIO;clear_hook=disable_during_clear;
 previous_polls=poll_enables;previous_logs=logs;count=submissions;run_work(&intr);
 assert(!intr.enabled && !urb.submitted && submissions==count);
 assert(poll_enables==previous_polls && logs==previous_logs+1);
 clear_hook=NULL;clear_error=0;fl2000_intr_stop(&intr);
 /* Shutdown during status processing prevents fallback and delayed retry. */
 urb.status=0;status_result=-EIO;assert(!fl2000_enable_interrupt(&usb));
 previous_polls=poll_enables;count=submissions;status_hook=shutdown_during_status;
 run_work(&intr);status_hook=NULL;
 assert(!intr.enabled && !urb.submitted && submissions==count);
 assert(!intr.status_polling && !intr.work.work.pending && poll_enables==previous_polls);
 status_result=0;fl2000_intr_stop(&intr);
 /* Resource release drains active URBs and queued work without outer disable. */
 intr.irq=0;intr.enabled=true;urb.submitted=true;intr.work.work.pending=true;
 fl2000_intr_release(NULL,&intr);
 assert(!intr.enabled && !urb.submitted && !intr.work.work.pending);
 puts("PASS: nested IRQ acknowledgment failures delay USB rearm; status failures and bridge errors enable polling, delayed recovery without USB edges, retry cancellation, polling fallback and IRQ recovery, status acknowledgment before rearm, initial status acknowledgment, idempotent start, error propagation, bounded stall recovery, cancellation, no resubmission after stop");
}
"""
parts = (
    [
        pre,
        function((ROOT / "fl2000.h").read_text(), "fl2000_usb_unavailable"),
        function((ROOT / "fl2000.h").read_text(), "fl2000_usb_error_terminal"),
        structure(source, "fl2000_interrupt"),
    ]
    + [
        function(source, n)
        for n in (
            "fl2000_irq_unmask",
            "fl2000_intr_recover_stall",
            "fl2000_intr_process_status",
            "fl2000_intr_work",
            "fl2000_intr_completion",
            "fl2000_intr_stop",
            "fl2000_intr_irq_init",
            "fl2000_intr_release",
            "fl2000_enable_interrupt",
            "fl2000_disable_interrupt",
        )
    ]
    + [checks]
)
with tempfile.TemporaryDirectory(prefix="fl2000-intr-") as temp:
    p = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    p.write_text("\n\n".join(parts))
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
            str(p),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
