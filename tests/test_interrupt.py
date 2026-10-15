#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise actual interrupt startup, recovery, and stop with mocked USB I/O."""

from pathlib import Path
import subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
ns = {"__file__": __file__}
exec((ROOT / "tests/test_logic.py").read_text().split("drm = ")[0], ns)
source = (ROOT / "fl2000_interrupt.c").read_text()
pre = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef unsigned char u8;
typedef int spinlock_t;
struct mutex { int unused; };
struct work_struct { bool pending; };
struct usb_device { int dev; };
struct urb { int status,pipe;void *context;bool submitted; };
#define FL2000_INTR_STALL_RETRIES 3
#define dev_warn_ratelimited(...) (++logs)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define spin_lock_irqsave(lock,flags) ((void)(lock),(flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(lock),(void)(flags))
#define GFP_ATOMIC 1
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
struct fl2000_interrupt;
static struct fl2000_interrupt *resource;
static void fl2000_intr_release(void *dev,void *res) { (void)dev;(void)res; }
static void *devres_find(void *dev,void (*release)(void *,void *),void *match,void *data)
{ (void)dev;(void)release;(void)match;(void)data;return resource; }
static int submissions,submit_error,clear_count,clear_error,status_reads,status_result,hotplugs,logs;
static int poll_enables,poll_disables;
static bool polling_current;
static void (*submit_hook)(void);
static void fl2000_drm_set_polling(struct usb_device *usb,bool enabled)
{ (void)usb;polling_current=enabled;if(enabled)poll_enables++;else poll_disables++; }
static void (*clear_hook)(void);
#define dev_err_ratelimited(...) (++logs)
static void msleep(int ms) { assert(ms==20); }
static int usb_clear_halt(struct usb_device *usb,int pipe)
{ (void)usb;assert(pipe==3);clear_count++;if(clear_hook)clear_hook();return clear_error; }
static int usb_submit_urb(struct urb *urb,int flags)
{ assert(flags==GFP_ATOMIC && !urb->submitted);submissions++;if(submit_error)return submit_error;urb->submitted=true;urb->status=-EINPROGRESS;if(submit_hook)submit_hook();return 0; }
static bool schedule_work(struct work_struct *work) { work->pending=true;return true; }
static void cancel_work_sync(struct work_struct *work) { work->pending=false; }
static int fl2000_check_interrupt(struct usb_device *usb) { (void)usb;status_reads++;return status_result; }
static void fl2000_drm_hotplug(struct usb_device *usb) { (void)usb;hotplugs++; }
static void fl2000_intr_completion(struct urb *urb);
static void usb_kill_urb(struct urb *urb)
{ if(urb->submitted){urb->submitted=false;urb->status=-ECONNRESET;fl2000_intr_completion(urb);} }
"""
checks = r"""
static struct fl2000_interrupt *current;
static void disable_during_clear(void) { current->enabled=false; }
static void run_work(struct fl2000_interrupt *intr) { intr->work.pending=false;fl2000_intr_work(&intr->work); }
static void immediate_failure(void)
{
 current->enabled=false;current->fallback=true;current->urb->submitted=false;
 current->work.pending=true;run_work(current);
}
static void complete(struct fl2000_interrupt *intr,int status)
{ intr->urb->submitted=false;intr->urb->status=status;fl2000_intr_completion(intr->urb); }
int main(void)
{
 struct usb_device usb={0};struct urb urb={.pipe=3};
 struct fl2000_interrupt intr={.usb_dev=&usb,.urb=&urb,.enabled=true};
 urb.context=&intr;current=&intr;
 complete(&intr,-EPIPE);
 assert(intr.enabled && intr.stalled && intr.work.pending && !submissions);
 run_work(&intr);
 assert(intr.enabled && !intr.stalled && urb.submitted && clear_count==1 && submissions==1);
 complete(&intr,0);
 assert(!urb.submitted && intr.work.pending && !logs);
 int before_ack=submissions;
 status_result=1;run_work(&intr);
 assert(status_reads==1 && hotplugs==1 && urb.submitted && submissions==before_ack+1);
 fl2000_intr_stop(&intr);
 assert(!intr.enabled && !intr.stalled && !urb.submitted && !intr.work.pending && !logs);
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
 assert(!intr.enabled && !urb.submitted && intr.work.pending);
 int previous_polls=poll_enables;run_work(&intr);
 assert(poll_enables==previous_polls+1 && !intr.work.pending);
 assert(clear_count==previous_clears+FL2000_INTR_STALL_RETRIES && logs==previous_logs+1);
 fl2000_intr_stop(&intr);
 /* Start checks/acknowledges stale status even without a USB completion. */
 resource=&intr;urb.status=0;
 previous_clears=clear_count;count=submissions;
 assert(fl2000_enable_interrupt(&usb)==0);
 assert(intr.enabled && urb.submitted && clear_count==previous_clears);
 assert(intr.work.pending);
 assert(submissions==count+1);
 int previous_reads=status_reads,previous_hotplugs=hotplugs;
 status_result=1;run_work(&intr);
 assert(status_reads==previous_reads+1 && hotplugs==previous_hotplugs+1);
 assert(fl2000_enable_interrupt(&usb)==0);
 assert(clear_count==previous_clears && submissions==count+1 && !intr.work.pending);
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
 complete(&intr,-EIO);assert(intr.work.pending && !intr.enabled);
 run_work(&intr);assert(poll_enables==previous_polls+1);
 fl2000_intr_stop(&intr);previous_polls=poll_disables;urb.status=0;
 assert(!fl2000_enable_interrupt(&usb) && poll_disables==previous_polls+1);
 fl2000_intr_stop(&intr);previous_polls=poll_enables;
 run_work(&intr);assert(poll_enables==previous_polls);
 resource=&intr;urb.status=0;submit_hook=immediate_failure;
 assert(!fl2000_enable_interrupt(&usb));
 assert(!intr.enabled && polling_current);
 submit_hook=NULL;fl2000_intr_stop(&intr);
 puts("PASS: immediate failure preserves polling fallback, polling fallback and IRQ recovery, status acknowledgment before rearm, initial status acknowledgment, idempotent start, error propagation, bounded stall recovery, cancellation, no resubmission after stop");
}
"""
parts = (
    [pre, ns["structure"](source, "fl2000_interrupt")]
    + [
        ns["function"](source, n)
        for n in (
            "fl2000_intr_work",
            "fl2000_intr_completion",
            "fl2000_intr_stop",
            "fl2000_enable_interrupt",
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
