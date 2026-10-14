#!/usr/bin/env python3
"""Build a temporary computation benchmark and run it only in hardware-free KVM.

Extracts the real conversion helpers and backends. Includes real kernel FPU
protection and rotating vmalloc buffers, but excludes DRM/USB/DMA and compositor
costs. Native SSSE3 reordering is an experimental benchmark-only candidate.
"""
import argparse
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', default=os.uname().release)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
ns = {'__file__': str(ROOT / 'tests/test_logic.py')}
exec((ROOT / 'tests/test_logic.py').read_text().split('drm = ')[0], ns)
function = ns['function']
stream = (ROOT / 'fl2000_streaming.c').read_text()
ssse3 = (ROOT / 'frame/x86.c').read_text()
bmi2 = (ROOT / 'frame/x86_bmi2.c').read_text()
pre = r'''
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/unaligned.h>
#include <linux/ktime.h>
#include <linux/sched/cputime.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include "ssse3.h"
#include "bmi2.h"
static unsigned int backend;
'''
parts = [pre]
parts.append(function(ssse3, 'fl2000_frame_convert').replace('unsigned int fl2000_frame_convert', 'static unsigned int bench_ssse3_convert'))
parts.append(function(bmi2, 'fl2000_frame_convert').replace('unsigned int fl2000_frame_convert', 'static unsigned int bench_bmi2_convert'))
parts.append(r'''
void fl2000_ssse3_reorder(u8 *dst, const u8 *src, unsigned int bytes);
static unsigned int fl2000_frame_convert(u8 *dst, const u8 *src, unsigned int pixels, unsigned int bpp)
{
 if(backend==1)return bench_ssse3_convert(dst,src,pixels,bpp);
 if(backend==2)return bench_bmi2_convert(dst,src,pixels,bpp);
 return 0;
}
static unsigned int fl2000_frame_reorder(u8 *dst, const u8 *src, unsigned int bytes)
{
 unsigned int count=min(bytes&~15U,65536U);
 if(backend!=1 || count<128 || !boot_cpu_has(X86_FEATURE_SSSE3) || !irq_fpu_usable())return 0;
 kernel_fpu_begin();fl2000_ssse3_reorder(dst,src,count);kernel_fpu_end();return count;
}
''')
for name in ('fl2000_rgb565', 'fl2000_rgb565_pair', 'fl2000_convert_line',
             'fl2000_convert_frame', 'fl2000_reorder_line', 'fl2000_reorder_frame'):
    parts.append(function(stream, name))
parts.append(r'''
static u64 cpu_ns(void)
{
 u64 user,system;
 task_cputime_adjusted(current,&user,&system);return user+system;
}
static int __init frame_bench_init(void)
{
 const char *names[]={"generic","ssse3","bmi2"};
 const unsigned int sizes[][2]={{64,64},{800,480},{1920,1080}};
 unsigned int k,bpp,path,trial,b,j;
 u64 start,cpu,elapsed;unsigned int count;
 if(!boot_cpu_has(X86_FEATURE_SSSE3)||!boot_cpu_has(X86_FEATURE_BMI2))return -EOPNOTSUPP;
 start=ktime_get_ns();cpu=cpu_ns();count=0;
 do {
  count++;if(!(count&63))cond_resched();
 }while(ktime_get_ns()-start<100000000ULL);
 elapsed=cpu_ns()-cpu;
 pr_info("CLOCK_BENCH calls=%u cpu_ns=%llu ns_per_iteration=%llu\n",count,elapsed,div64_u64(elapsed,count));
 start=ktime_get_ns();cpu=cpu_ns();count=0;
 do {
  kernel_fpu_begin();kernel_fpu_end();count++;
  if(!(count&63))cond_resched();
 }while(ktime_get_ns()-start<100000000ULL);
 elapsed=cpu_ns()-cpu;
 pr_info("FPU_BENCH calls=%u cpu_ns=%llu ns_per_pair=%llu\n",count,elapsed,div64_u64(elapsed,count));
 for(k=0;k<ARRAY_SIZE(sizes);k++)for(bpp=2;bpp<=3;bpp++)for(path=0;path<3;path++) {
  unsigned int width=sizes[k][0],height=sizes[k][1];
  unsigned int pitch=width*(path?bpp:4)+(path==2?19:0),size=ALIGN(width*height*bpp,8U);
  u8 *allocation=vzalloc((size_t)pitch*height+1),*source,*expected=vzalloc(size),*dest[5]={0};
  int ret=-ENOMEM;
  if(!allocation||!expected)goto free;
  source=allocation+1;
  for(j=0;j<(size_t)pitch*height;j++)source[j]=(j*71U+j/7U)&255;
  for(j=0;j<5;j++){dest[j]=vzalloc(size);if(!dest[j])goto free;}
  backend=0;
  if(path)fl2000_reorder_frame(expected,source,height,width,pitch,bpp,size);
  else fl2000_convert_frame(expected,source,height,width,pitch,bpp,size);
  for(b=0;b<3;b++) {
   backend=b;
   for(j=0;j<5;j++) {
    if(path)fl2000_reorder_frame(dest[j],source,height,width,pitch,bpp,size);
    else fl2000_convert_frame(dest[j],source,height,width,pitch,bpp,size);
    if(memcmp(dest[j],expected,size)){ret=-EINVAL;goto free;}
   }
   for(trial=0;trial<3;trial++) {
    start=ktime_get_ns();cpu=cpu_ns();count=0;
    do {
     if(path)fl2000_reorder_frame(dest[count%5],source,height,width,pitch,bpp,size);
     else fl2000_convert_frame(dest[count%5],source,height,width,pitch,bpp,size);
     count++;
    }while(ktime_get_ns()-start<100000000ULL);
    elapsed=cpu_ns()-cpu;
    pr_info("FRAME_BENCH backend=%s path=%u width=%u height=%u bpp=%u trial=%u frames=%u cpu_ns=%llu ns_per_frame=%llu\n",
     names[b],path,width,height,bpp,trial,count,elapsed,div64_u64(elapsed,count));
   }
  }
  ret=0;
free:
  for(j=0;j<5;j++)
   vfree(dest[j]);
  vfree(expected);vfree(allocation);
  if(ret)return ret;
 }
 pr_info("FRAME_BENCH_PASS byte comparisons and protected computation complete\n");
 return 0;
}
static void __exit frame_bench_exit(void) { }
module_init(frame_bench_init);
module_exit(frame_bench_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Isolated FL2000 frame preparation benchmark");
''')
(args.output / 'bench.c').write_text('\n\n'.join(parts))
for name in ('ssse3.c', 'ssse3.h', 'bmi2.c', 'bmi2.h'):
    shutil.copyfile(ROOT / 'frame' / name, args.output / name)
with (args.output / 'ssse3.c').open('a') as output:
    output.write('\nvoid fl2000_ssse3_reorder(u8 *dst, const u8 *src, unsigned int bytes);\n')
    output.write(function((ROOT / 'tests/benchmark_cpu.py').read_text(), 'fl2000_ssse3_reorder'))
(args.output / 'Makefile').write_text('''obj-m := frame_bench.o
frame_bench-y := bench.o ssse3.o bmi2.o
CFLAGS_REMOVE_ssse3.o += -mno-sse -mno-sse2
CFLAGS_ssse3.o += -mssse3 -mno-avx
CFLAGS_bmi2.o += -mbmi2
''')
subprocess.run(['make', '-C', f'/usr/src/linux-headers-{args.kernel}',
                f'M={args.output.resolve()}', 'W=1', 'KCFLAGS=-Werror', 'modules'], check=True)
init = r'''#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/bin:/sbin:/usr/lib/klibc/bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
if insmod /frame_bench.ko; then
 rmmod frame_bench || echo FRAME_BENCH_UNLOAD_FAIL
else
 echo FRAME_BENCH_LOAD_FAIL
fi
dmesg
/usr/lib/klibc/bin/poweroff -f
'''
archive=bytearray()
for ino,(name,data,mode) in enumerate([('init',init.encode(),0o100755),
 ('frame_bench.ko',(args.output/'frame_bench.ko').read_bytes(),0o100644),('TRAILER!!!',b'',0)],1):
    fields=[ino,mode,0,0,1,0,len(data),0,0,0,0,len(name)+1,0]
    archive.extend(b'070701'+''.join(f'{x:08x}' for x in fields).encode())
    archive.extend(name.encode()+b'\0');archive.extend(b'\0'*(-len(archive)%4))
    archive.extend(data);archive.extend(b'\0'*(-len(archive)%4))
archive.extend(b'\0'*(-len(archive)%512))
with (args.output/'boot.cpio').open('wb') as output:
    with (ROOT/'base-initramfs.cpio.gz').open('rb') as base:
        shutil.copyfileobj(base,output)
    output.write(b'\0'*(-output.tell()%4));output.write(archive)
command=['qemu-system-x86_64','-enable-kvm','-cpu','host','-m','1G','-nic','none',
 '-kernel',f'/boot/vmlinuz-{args.kernel}','-initrd',str(args.output/'boot.cpio'),
 '-append','console=ttyS0 quiet rdinit=/init panic=1 oops=panic','-display','none',
 '-monitor','none','-serial','stdio','-no-reboot']
with (args.output/'guest.log').open('w') as output:
    subprocess.run(command,stdout=output,stderr=subprocess.STDOUT,timeout=180,check=True)
log=(args.output/'guest.log').read_text()
if 'FRAME_BENCH_PASS' not in log or re.search(r'FRAME_BENCH_(?:LOAD|UNLOAD)_FAIL|BUG:|Oops:|WARNING:',log):
    raise RuntimeError('kernel benchmark failed; inspect guest.log')
print(f'PASS: real kernel FPU protection, byte comparisons and CPU timings; log: {args.output / "guest.log"}')
