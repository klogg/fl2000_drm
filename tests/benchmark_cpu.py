#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Benchmark production frame preparation helpers without hardware.

Portable/SSSE3/BMI2 code, CPU thread time, rotating five destination buffers, padded
unaligned input and three trials per case. CPU percentages assume 60 updates/sec
and one core; they exclude kernel FPU protection, mapping, scheduling, USB and DMA costs.
This compares computation only; select backends using the complete guest workload.
Run: python3 tests/benchmark_cpu.py
"""

import os
import re
import subprocess
import tempfile
from pathlib import Path

from source_helpers import function as f

root = Path(__file__).resolve().parents[1]
stream = (root / "fl2000_streaming.c").read_text()
pre = r"""
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define cond_resched() ((void)0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
static void put_unaligned_le64(u64 v,u8 *p){memcpy(p,&v,8);}
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
static void put_unaligned_le16(u16 v,u8 *p){memcpy(p,&v,2);}
static void put_unaligned_le32(u32 v,u8 *p){memcpy(p,&v,4);}
static u32 get_unaligned_le32(const void *p){u32 v;memcpy(&v,p,4);return v;}
static u64 get_unaligned_le64(const void *p){u64 v;memcpy(&v,p,8);return v;}
static unsigned long long ns(void){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
static volatile unsigned long sink;
"""
main = r"""
int main(void){
 const unsigned int sizes[][2]={{64,64},{800,480},{1920,1080}};
 const char *backends[]={"generic","ssse3","bmi2"};
 puts("backend,conversion,width,height,bpp,us/frame,one-core-percent-at-60-updates");
 for(backend=0;backend<3;backend++){
  if(backend==1 && !__builtin_cpu_supports("ssse3"))continue;
  if(backend==2 && !__builtin_cpu_supports("bmi2"))continue;
 for(unsigned int k=0;k<ARRAY_SIZE(sizes);k++)for(unsigned int bpp=2;bpp<=3;bpp++)for(int path=0;path<3;path++){
  if(path && backend)continue; /* Native formats share the portable reorder path. */
  const char *label=path==0 ? "conversion" : path==1 ? "native-tight" : "native-padded";
  unsigned int w=sizes[k][0],h=sizes[k][1],pitch=w*(path?bpp:4)+(path==1?0:16),size=(w*h*bpp+7)&~7U;
  u8 *src=malloc((size_t)pitch*h+1); memset(src,0xab,(size_t)pitch*h+1);
  u8 *dst[5];for(int j=0;j<5;j++)dst[j]=malloc(size);
  for(int j=0;j<5;j++) {
   if(path)fl2000_reorder_frame(dst[j],src+1,h,w,pitch,bpp,size);
   else fl2000_convert_frame(dst[j],src+1,h,w,pitch,bpp,size);
  }
  for(int trial=0;trial<3;trial++){
   unsigned long long start=ns();unsigned int n=0;
   while(ns()-start<100000000){
    if(path)fl2000_reorder_frame(dst[n%5],src+1,h,w,pitch,bpp,size);
    else fl2000_convert_frame(dst[n%5],src+1,h,w,pitch,bpp,size);
    sink+=dst[n%5][n%size];n++;
   }
   double us=(double)(ns()-start)/n/1000;
   printf("%s,%s,%u,%u,%u,%.2f,%.2f\n",backends[backend],label,w,h,bpp,us,us*60/10000);
  }
  free(src);for(int j=0;j<5;j++)free(dst[j]);
 }
 }
 return 0;
}
"""
simd = (
    (root / "frame/ssse3.c")
    .read_text()
    .replace("#include <linux/types.h>", "")
    .replace('#include "ssse3.h"', '#include "frame/ssse3.h"')
)
bmi = (root / "frame/bmi2.c").read_text()
bmi = re.sub(r"^#include <linux/.*$", "", bmi, flags=re.MULTILINE).replace(
    '#include "bmi2.h"', '#include "frame/bmi2.h"'
)
dispatch = r"""
static unsigned int backend;
static unsigned int fl2000_frame_convert(u8 *dst,const u8 *src,unsigned int pixels,unsigned int bpp){
 unsigned int count=min(pixels&~7U,4096U);if(count<32 || !backend)return 0;
 if(backend==1)fl2000_ssse3_convert(dst,src,count,bpp);
 else fl2000_bmi2_convert(dst,src,count,bpp);
 return count;
}

"""
parts = [
    pre,
    simd,
    bmi,
    dispatch,
    f(stream, "fl2000_rgb565"),
    f(stream, "fl2000_rgb565_pair"),
    f(stream, "fl2000_convert_line"),
    f(stream, "fl2000_convert_frame"),
    f(stream, "fl2000_reorder_line"),
    f(stream, "fl2000_reorder_frame"),
]
parts.append(main)
with tempfile.TemporaryDirectory(prefix="fl2000-cpu-") as temp:
    p = Path(temp) / "benchmark.c"
    exe = Path(temp) / "benchmark"
    p.write_text("\n".join(parts))
    subprocess.run(
        [
            os.environ.get("CC", "gcc"),
            "-std=gnu11",
            "-O2",
            "-fno-tree-vectorize",
            "-mssse3",
            "-mbmi2",
            "-mno-avx",
            "-I",
            str(root),
            "-fno-strict-aliasing",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(p),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
