#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run actual driver helpers in a userspace harness with ASan/UBSan.

USB submission is mocked to exercise ownership, partial submission failure and
cancellation. This does not emulate USB DMA or physical display timing.
"""

import os
import re
import subprocess
import tempfile
from pathlib import Path

from source_helpers import function, register_header, structure

ROOT = Path(__file__).resolve().parents[1]


drm = (ROOT / "fl2000_drm.c").read_text()
stream = (ROOT / "fl2000_streaming.c").read_text()
header = (ROOT / "fl2000.h").read_text()
i2c = (ROOT / "fl2000_i2c.c").read_text()
registers = (ROOT / "fl2000_registers.c").read_text()
defines = "\n".join(
    re.findall(r"^#define FL2000_.*$", drm + "\n" + stream, re.MULTILINE)
)
prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uintptr_t dma_addr_t;
#define DRM_FORMAT_XRGB8888 1
#define DRM_FORMAT_RGB888 2
#define DRM_FORMAT_RGB565 3
#define DMA_FROM_DEVICE 2
#define check_mul_overflow(a,b,p) __builtin_mul_overflow(a,b,p)
#define __always_unused __attribute__((__unused__))
#define URB_NO_TRANSFER_DMA_MAP 4
#define __aligned(n) __attribute__((aligned(n)))
#define __packed __attribute__((packed))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
struct drm_rect { int x1,y1,x2,y2; };
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define kmalloc_obj(value, flags) kmalloc(sizeof(value), flags)
#define kzalloc_obj(value) calloc(1, sizeof(value))
#define swap(a,b) do { __typeof__(a) tmp=(a);(a)=(b);(b)=tmp; } while (0)
#define GFP_ATOMIC 1
#define STREAMING_EP 1
#define PAGE_SIZE 4096
#define ALIGN(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define cond_resched() ((void)0)
#ifndef TEST_SIMD
static unsigned int fl2000_frame_convert(u8 *d, const u8 *s, unsigned int n, unsigned int b)
{ (void)d; (void)s; (void)n; (void)b; return 0; }
static unsigned int fl2000_frame_reorder(u8 *d, const u8 *s, unsigned int n)
{ (void)d; (void)s; (void)n; return 0; }
#endif
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
#define dev_err_ratelimited(...) (++log_count)
#define DRM_MODE_FLAG_INTERLACE BIT(0)
#define DRM_MODE_FLAG_DBLSCAN BIT(1)
#define DRM_MODE_FLAG_3D_MASK (BIT(2) | BIT(3))
struct drm_display_mode {
 int clock, hdisplay, hsync_start, hsync_end, htotal;
 int vdisplay, vsync_start, vsync_end, vtotal;
 unsigned int flags;
 int vscan;
 int crtc_clock,crtc_htotal;
 struct { void *next,*prev; } head;
};
static void drm_mode_copy(struct drm_display_mode *dst, const struct drm_display_mode *src)
{ typeof(dst->head) head=dst->head;*dst = *src;dst->head=head; }
static void drm_mode_set_crtcinfo(struct drm_display_mode *mode,int flags)
{ assert(!flags);mode->crtc_clock=mode->clock;mode->crtc_htotal=mode->htotal; }
static u64 div_u64(u64 dividend, u32 divisor) { return dividend / divisor; }
static u64 div64_u64(u64 dividend, u64 divisor) { return dividend / divisor; }
static u32 get_unaligned_le32(const u8 *p)
{ return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 get_unaligned_le64(const u8 *p) { return (u64)get_unaligned_le32(p) | (u64)get_unaligned_le32(p+4)<<32; }
static void put_unaligned_le64(u64 value,u8 *p) { for(unsigned int i=0;i<8;i++)p[i]=value>>(i*8); }
static void put_unaligned_le16(u16 value, u8 *p) { p[0]=value; p[1]=value>>8; }
static void put_unaligned_le32(u32 value, u8 *p) { p[0]=value; p[1]=value>>8; p[2]=value>>16; p[3]=value>>24; }
struct drm_format_info { u32 format; unsigned int cpp[1]; };
struct drm_framebuffer { const struct drm_format_info *format; unsigned int pitches[1]; };
struct drm_plane { struct drm_device *dev; struct drm_plane_state *state; };
struct drm_crtc;
struct drm_plane_state { struct drm_atomic_state *state; u32 src_x,src_y,src_w,src_h; struct drm_framebuffer *fb; struct drm_crtc *crtc; bool visible; };
struct drm_crtc_state { struct drm_display_mode mode,adjusted_mode; bool no_vblank,mode_changed,active; };
struct iosys_map { bool is_iomem; void *vaddr; };
struct drm_shadow_plane_state { struct drm_plane_state base; struct iosys_map map[1],data[1]; };
#define to_drm_shadow_plane_state(state) ((struct drm_shadow_plane_state *)(state))
static int prepare_result,map_result,map_refs,map_creations,map_removals,mode_calls;
static bool map_iomem;
static int drm_gem_plane_helper_prepare_fb(struct drm_plane *p,struct drm_plane_state *s)
{ (void)p; (void)s; return prepare_result; }
static int drm_gem_fb_vmap(struct drm_framebuffer *fb,struct iosys_map *map,struct iosys_map *data)
{ (void)fb; if(map_result)return map_result; if(!map_refs++)map_creations++; map[0].vaddr=(void *)1; data[0].is_iomem=map_iomem; return 0; }
static void drm_gem_fb_vunmap(struct drm_framebuffer *fb,struct iosys_map *map)
{ (void)fb; assert(map[0].vaddr && map_refs>0); map[0].vaddr=NULL; if(!--map_refs)map_removals++; }
enum usb_device_speed { USB_SPEED_HIGH, USB_SPEED_SUPER, USB_SPEED_SUPER_PLUS };
struct device { int unused; };
struct usb_bus { unsigned int sg_tablesize; };
struct usb_device { struct usb_bus *bus; struct device dev; enum usb_device_speed speed; };
struct regmap { u32 aclk, ctrl2, pll, regs[0x10000/4]; };
static struct regmap test_regmap;
static int regmap_error, regmap_calls, detection_writes, detection_delay;
static struct regmap *dev_get_regmap(struct device *dev, const char *name)
{ (void)dev; (void)name; return &test_regmap; }
static int regmap_write(struct regmap *map, unsigned int reg, u32 val)
{ assert(reg==0x802c); if(regmap_error)return regmap_error;map->pll=val;return 0; }
static void msleep(unsigned int ms)
{ assert(ms==750 && detection_writes==2);detection_delay=ms; }
static int regmap_write_bits(struct regmap *map, unsigned int reg, u32 mask, u32 val)
{
 regmap_calls++;
 if(regmap_error)return regmap_error;
 assert(reg<0x10000 && !(reg&3));
 if(reg==0x8020) {
  const u32 expected[]={0,BIT(30),BIT(30)|BIT(28)};
  assert(detection_writes<3 && (val & (BIT(30)|BIT(28)))==expected[detection_writes]);
  if(detection_writes==2)assert(detection_delay==750);
  detection_writes++;
 }
 u32 *target=reg==0x803c?&map->aclk:reg==0x8064?&map->ctrl2:&map->regs[reg/4];
 *target=(*target & ~mask)|(val & mask);
 return 0;
}

struct drm_device { bool unplugged; };
struct drm_crtc { struct drm_device *dev; struct drm_crtc_state *state; };
struct fl2000_drm_if { struct drm_device drm; struct usb_device *usb_dev; struct drm_crtc crtc; };
static struct fl2000_drm_if test_drm_if;
struct drm_atomic_state { struct drm_plane plane; struct drm_plane_state *old_plane,*new_plane; struct drm_crtc_state *crtc; int get_error; bool allow_modeset; };
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p) ? (int)PTR_ERR(p) : 0)
#define for_each_oldnew_plane_in_state(s,p,o,n,i) \
 for((i)=0,(p)=&(s)->plane,(o)=(s)->old_plane,(n)=(s)->new_plane; (i)<1 && (p); (i)++)
static struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_state *state,struct drm_crtc *crtc)
{ (void)crtc; return state->get_error ? (struct drm_crtc_state *)(intptr_t)state->get_error : state->crtc; }
static int drm_atomic_helper_check(struct drm_device *drm,struct drm_atomic_state *state)
{ (void)drm; return state->crtc->mode_changed && !state->allow_modeset ? -EINVAL : 0; }
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define DRM_PLANE_NO_SCALING 0x10000
static int primary_check_result,affected_result,affected_calls,plane_check_result;
static bool plane_visible;
static int drm_crtc_helper_atomic_check(struct drm_crtc *crtc,struct drm_atomic_state *state)
{ (void)crtc;(void)state;return primary_check_result; }
static int drm_atomic_add_affected_planes(struct drm_atomic_state *state,struct drm_crtc *crtc)
{ (void)crtc;(void)state;affected_calls++;return affected_result; }
static struct drm_plane_state *drm_atomic_get_new_plane_state(struct drm_atomic_state *state,struct drm_plane *plane)
{ (void)plane;return state->new_plane; }
static struct drm_plane_state *drm_atomic_get_old_plane_state(struct drm_atomic_state *state,struct drm_plane *plane)
{ (void)plane;return state->old_plane; }
static bool damage_merged;
static struct drm_rect merged_damage;
static bool drm_atomic_helper_damage_merged(const struct drm_plane_state *old,const struct drm_plane_state *new,struct drm_rect *rect)
{ (void)old;(void)new;*rect=merged_damage;return damage_merged; }
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(struct drm_atomic_state *state,struct drm_crtc *crtc)
{ (void)crtc;return state->crtc; }
static int drm_atomic_helper_check_plane_state(struct drm_plane_state *plane,const struct drm_crtc_state *crtc,int min_scale,int max_scale,bool position,bool disabled)
{
 (void)crtc;assert(min_scale==DRM_PLANE_NO_SCALING && max_scale==DRM_PLANE_NO_SCALING);
 assert(!position && !disabled);plane->visible=plane_visible;return plane_check_result;
}
static bool drm_atomic_crtc_needs_modeset(const struct drm_crtc_state *s) { return s->mode_changed; }
static bool drm_dev_enter(struct drm_device *drm,int *idx) { *idx=0;return !drm->unplugged; }
static void drm_dev_exit(int idx) { (void)idx; }
static int cpu_access_result,cpu_access_count,cpu_access_calls;
static int drm_gem_fb_begin_cpu_access(struct drm_framebuffer *fb,int dir)
{ (void)fb;assert(dir==DMA_FROM_DEVICE);if(cpu_access_result)return cpu_access_result;cpu_access_count++;cpu_access_calls++;return 0; }
static void drm_gem_fb_end_cpu_access(struct drm_framebuffer *fb,int dir)
{ (void)fb;assert(dir==DMA_FROM_DEVICE && cpu_access_count==1);cpu_access_count--; }

struct mutex { int unused; };
typedef int spinlock_t;
struct page { int unused; };
struct scatterlist { struct page *page; unsigned int length; };
struct sg_table { struct scatterlist *sgl; unsigned int orig_nents; };
struct urb {
 void *context;
 unsigned int transfer_buffer_length, num_sgs, actual_length;
 struct scatterlist *sg;
 struct sg_table *sgt;
 void *transfer_buffer;
 dma_addr_t transfer_dma;
 unsigned int transfer_flags;
 int status;
 bool submitted;
};
#define for_each_sg(list, sg, count, i) \
 for ((i) = 0, (sg) = (list); (i) < (count); (i)++, (sg)++)
static unsigned int sg_sets, sg_allocs, sg_frees, fail_sg_at;
static int sg_alloc_table(struct sg_table *t,unsigned int n,int flags)
{ (void)flags;if(++sg_allocs==fail_sg_at)return -ENOMEM;t->sgl=calloc(n,sizeof(*t->sgl));t->orig_nents=n;return t->sgl?0:-ENOMEM; }
static void sg_free_table(struct sg_table *t)
{ if(t->sgl){free(t->sgl);sg_frees++;}memset(t,0,sizeof(*t)); }
static void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int length, int offset)
{ assert(!offset); sg_sets++; sg->page = page; sg->length = length; }
static int submit_count, fail_at, log_count;
static int usb_submit_urb(struct urb *urb, int flags)
{
 assert(flags == GFP_ATOMIC);
 assert(!urb->submitted);
 submit_count++;
 if (submit_count == fail_at) return -ENOMEM;
 urb->submitted = true;
 urb->status = -EINPROGRESS;
 urb->actual_length = 0;
 return 0;
}
static int usb_sndbulkpipe(struct usb_device *usb, int ep) { (void)usb; return ep; }
static int usb_clear_halt(struct usb_device *usb, int pipe) { (void)usb; (void)pipe; return 0; }
static void usb_kill_urb(struct urb *urb);
static struct fl2000_stream *test_stream;
static struct fl2000_stream *fl2000_find_stream(struct usb_device *usb_dev);
#define I2C_M_RD 1
#define I2C_M_DMA_SAFE 0x0200
#define I2C_XFER_ADDR_MASK (~0x3ul)
struct i2c_adapter { void *algo_data; };
#define I2C_LOCK_ROOT_ADAPTER 1
static struct i2c_adapter *fl2000_i2c_adapter(struct usb_device *usb) { (void)usb;return NULL; }
static void i2c_lock_bus(struct i2c_adapter *a,int flags) { (void)a;(void)flags;assert(false); }
static void i2c_unlock_bus(struct i2c_adapter *a,int flags) { (void)a;(void)flags;assert(false); }
struct i2c_msg { u16 addr, flags, len; u8 *buf; };
static int i2c_calls, i2c_result;
static u32 i2c_value;
static int detect_bad_id, detect_short;
static int fl2000_i2c_dword(struct usb_device *usb, bool read, u16 addr, u8 offset, u32 *data)
{
 (void)usb;
 if(offset==0) {
  assert(read);
  if(addr==0x4d)return -EIO;
  assert(addr==0x4c);*data=detect_bad_id ? 0x06120000 : 0x06124954;return 0;
 }
 assert(addr == 0x4c); assert(offset == 0xfc);
 i2c_calls++;
 if (i2c_result) return i2c_result;
 if (read) *data = i2c_value; else i2c_value = *data;
 return 0;
}
typedef u32 __le32;
#define GFP_KERNEL 0
#define GFP_DMA 0
#define kmalloc(size, flags) malloc(size)
#define kfree(ptr) free(ptr)
#define le32_to_cpu(value) (value)
#define cpu_to_le32(value) (value)
#define USB_DIR_IN 0x80
#define USB_DIR_OUT 0
#define USB_TYPE_VENDOR 0x40
#define FL2000_MONITOR_DETECT_MDELAY 750
#define CONTROL_MSG_READ 64
#define CONTROL_MSG_WRITE 65
#define USB_CTRL_GET_TIMEOUT 1000
#define USB_CTRL_SET_TIMEOUT 1000
static int control_result;
static u32 control_value;
static int usb_rcvctrlpipe(struct usb_device *usb, int ep) { (void)usb; return ep; }
static int usb_sndctrlpipe(struct usb_device *usb, int ep) { (void)usb; return ep; }
static int usb_control_msg(struct usb_device *usb, int pipe, int request, int type,
                          int value, u16 offset, void *data, size_t size, int timeout)
{
 (void)usb; (void)pipe; (void)type; (void)value; (void)offset; (void)timeout;
 assert(size == 4);
 if (request == CONTROL_MSG_READ && control_result > 0)
  memcpy(data, &control_value, control_result < 4 ? control_result : 4);
 if (request == CONTROL_MSG_WRITE) control_value = *(u32 *)data;
 return control_result;
}

#define DMA_TO_DEVICE 1
#define __GFP_ZERO 2
#define VM_MAP 0
#define PAGE_KERNEL 0
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define kcalloc(n,size,flags) calloc(n,size)
static bool fail_dma_alloc;
static unsigned int dma_allocs,dma_frees,page_allocs,page_frees,fail_page_at;
static void *usb_alloc_noncoherent(struct usb_device *usb,unsigned int size,int flags,
                                  dma_addr_t *dma,int dir,struct sg_table **sgt)
{
 (void)usb;(void)flags;assert(dir==DMA_TO_DEVICE);
 if(fail_dma_alloc)return NULL;
 void *buffer=malloc(size);
 *sgt=calloc(1,sizeof(**sgt)); *dma=0x12340000;dma_allocs++;
 return buffer;
}
static void usb_free_noncoherent(struct usb_device *usb,unsigned int size,void *buffer,
                                 int dir,struct sg_table *sgt)
{ (void)usb;assert(size && dir==DMA_TO_DEVICE && buffer && sgt);free(buffer);free(sgt);dma_frees++; }
static struct page *alloc_page(int flags)
{ (void)flags;if(++page_allocs==fail_page_at)return NULL;return calloc(1,sizeof(struct page)); }
static void __free_page(struct page *page) { assert(page);free(page);page_frees++; }
static void *vmap(struct page **pages,unsigned int count,int flags,int prot)
{ (void)flags;(void)prot;for(unsigned int i=0;i<count;i++)assert(pages[i]);return calloc(count,PAGE_SIZE); }
static void vunmap(void *buffer) { free(buffer); }

"""
checks = r"""
static void test_conversion(void)
{
 const unsigned int widths[] = { 1, 2, 3, 4, 5, 7, 8, 13, 31, 32, 33, 4095, 4096, 4097, 1366, 1920 };
 for (unsigned int bpp = 2; bpp <= 3; bpp++) {
  for (unsigned int w = 0; w < ARRAY_SIZE(widths); w++)
   for (unsigned int padding=0;padding<=20;padding+=20) {
   unsigned int width = widths[w], height = 7, pitch = width * 4 + padding;
   unsigned int size = width * height * bpp, padded = ALIGN(size, 8U);
   u8 *allocation = malloc((size_t)pitch * height + 1);
   u8 *src = allocation + 1; /* Exercise unaligned and padded source rows. */
   u8 *dst = malloc(padded), *expected = calloc(padded, 1);
   unsigned int position = 0;
   memset(dst, 0xa5, padded);
   for (unsigned int y = 0; y < height; y++) {
    for (unsigned int x = 0; x < width; x++) {
     u32 pixel = (x * 0x2741U + y * 0x437721U) & 0xffffff;
     u8 *p = src + (size_t)y * pitch + x * 4;
     p[0] = pixel; p[1] = pixel >> 8; p[2] = pixel >> 16; p[3] = 0xfa;
     if (bpp == 3) {
      for (unsigned int c = 0; c < 3; c++)
       expected[position++] = p[c];
     } else {
      u16 value = ((p[2] / 8) << 11) | ((p[1] / 4) << 5) | (p[0] / 8);
      expected[position++] = value;
      expected[position++] = value >> 8;
     }
    }
   }
   /* Independent golden packing: exchange dwords in each padded wire word. */
   for (unsigned int i = 0; i < padded; i += 8) {
    u8 word[8];
    memcpy(word, expected + i, 8);
    memcpy(expected + i, word + 4, 4);
    memcpy(expected + i + 4, word, 4);
   }
   fl2000_convert_frame(dst, src, height, width, pitch, bpp, padded);
   assert(!memcmp(dst, expected, padded));
   free(expected); free(dst); free(allocation);
  }
 }
}


static void test_damage_simd_boundaries(void)
{
 u8 src[4096], dst[4096], expected[4096];
 for(unsigned int i=0;i<sizeof(src);i++)src[i]=(i*71U+i/7U)&255;
 for(unsigned int bpp=2;bpp<=3;bpp++)for(unsigned int start=0;start<8;start++)
  for(unsigned int width=1;width<180;width++) {
   memset(dst,0xa5,sizeof(dst));memset(expected,0xa5,sizeof(expected));
   unsigned int pos=start*bpp;
   for(unsigned int x=0;x<width;x++) {
    const u8 *p=src+1+x*4;
    unsigned int pixel=bpp==3 ? (unsigned int)p[0]|((unsigned int)p[1]<<8)|((unsigned int)p[2]<<16)
     : ((p[2]/8U)<<11)|((p[1]/4U)<<5)|(p[0]/8U);
    for(unsigned int c=0;c<bpp;c++)expected[pos++^4]=pixel>>(8*c);
   }
   fl2000_convert_line(dst,src+1,width,start*bpp,bpp);
   assert(!memcmp(dst,expected,sizeof(dst)));
  }
 for(unsigned int start=0;start<16;start++)for(unsigned int size=0;size<300;size++) {
  memset(dst,0xa5,sizeof(dst));memset(expected,0xa5,sizeof(expected));
  for(unsigned int i=0;i<size;i++)expected[(start+i)^4]=src[1+i];
  fl2000_reorder_line(dst,src+1,size,start);
  assert(!memcmp(dst,expected,sizeof(dst)));
 }
}

static void test_native_reordering(void)
{
 const unsigned int widths[]={1,2,3,4,5,7,8,13,127,128,129,16383,16384,16385,1366,1920};
 for(unsigned int bpp=2;bpp<=3;bpp++)for(unsigned int k=0;k<ARRAY_SIZE(widths);k++)
  for(unsigned int padding=0;padding<=19;padding+=19) {
   unsigned int width=widths[k],height=7,pitch=width*bpp+padding;
   unsigned int size=width*height*bpp,padded=ALIGN(size,8U);
   u8 *allocation=malloc((size_t)pitch*height+1),*src=allocation+1;
   u8 *expected=calloc(padded,1),*dst=malloc(padded);
   memset(allocation,0xdb,(size_t)pitch*height+1);memset(dst,0xa5,padded);
   for(unsigned int y=0;y<height;y++)for(unsigned int x=0;x<width*bpp;x++) {
    u8 byte=(x*37+y*91+13)&255;
    src[(size_t)y*pitch+x]=byte;
    expected[(y*width*bpp+x)^4]=byte;
   }
   fl2000_reorder_frame(dst,src,height,width,pitch,bpp,padded);
   assert(!memcmp(dst,expected,padded));
   free(allocation);free(expected);free(dst);
  }
}

static void test_native_formats(void)
{
 struct drm_display_mode mode={.clock=25175,.hdisplay=640,.htotal=800,.vdisplay=480,.vtotal=525};
 assert(fl2000_frame_bytes_pix(USB_SPEED_HIGH,&mode,DRM_FORMAT_XRGB8888)==2);
 assert(fl2000_frame_bytes_pix(USB_SPEED_HIGH,&mode,DRM_FORMAT_RGB565)==2);
 assert(!fl2000_frame_bytes_pix(USB_SPEED_HIGH,&mode,DRM_FORMAT_RGB888));
 assert(fl2000_frame_bytes_pix(USB_SPEED_SUPER,&mode,DRM_FORMAT_RGB888)==3);
 assert(fl2000_frame_bytes_pix(USB_SPEED_SUPER,&mode,DRM_FORMAT_RGB565)==2);
 assert(!fl2000_frame_bytes_pix(USB_SPEED_SUPER,&mode,0));
 mode.clock=148500;mode.hdisplay=1920;mode.htotal=2200;mode.vdisplay=1080;mode.vtotal=1125;
 assert(!fl2000_frame_bytes_pix(USB_SPEED_HIGH,&mode,DRM_FORMAT_RGB565));
 assert(fl2000_frame_bytes_pix(USB_SPEED_SUPER,&mode,DRM_FORMAT_RGB888)==3);
 const struct drm_format_info formats[]={{DRM_FORMAT_XRGB8888,{4}}, {DRM_FORMAT_RGB888,{3}}, {DRM_FORMAT_RGB565,{2}}};
 struct drm_framebuffer old_fb={.format=&formats[0]},new_fb={.format=&formats[1]};
 struct drm_crtc_state crtc={.active=true};struct drm_crtc output={.state=&crtc};
 struct drm_plane_state old={.fb=&old_fb,.crtc=&output},new={.fb=&new_fb,.crtc=&output};
 struct drm_atomic_state atomic={.old_plane=&old,.new_plane=&new,.crtc=&crtc,.allow_modeset=true};
 struct drm_device drm={0};
 assert(!fl2000_atomic_check(&drm,&atomic) && crtc.mode_changed);
 crtc.mode_changed=false;atomic.allow_modeset=false;
 assert(fl2000_atomic_check(&drm,&atomic)==-EINVAL);
 crtc.mode_changed=false;new_fb.format=&formats[0];
 assert(!fl2000_atomic_check(&drm,&atomic) && !crtc.mode_changed);
 new_fb.format=&formats[2];atomic.allow_modeset=true;atomic.get_error=-EDEADLK;
 assert(fl2000_atomic_check(&drm,&atomic)==-EDEADLK);
 atomic.get_error=0;crtc.active=false;
 assert(!fl2000_atomic_check(&drm,&atomic) && !crtc.mode_changed);
}

static void test_native_crop(void)
{
 struct usb_device usb={.speed=USB_SPEED_SUPER};
 struct fl2000_stream stream={.usb_dev=&usb,.bytes_pix=3,.buf_size=16};
 u8 storage[100],expected[16]={0},destination[16],second[16];
 for(unsigned int i=0;i<sizeof(storage);i++)storage[i]=i*17+5;
 stream.frames[0].vaddr=destination;test_stream=&stream;
 test_drm_if.usb_dev=&usb;
 const struct drm_format_info rgb={DRM_FORMAT_RGB888,{3}};
 struct drm_framebuffer fb={.format=&rgb,.pitches={19}};
 struct drm_shadow_plane_state state={.base={.fb=&fb,.src_x=2U<<16,.src_y=1U<<16,.src_w=2U<<16,.src_h=2U<<16},.data={{.vaddr=storage+5}}};
 for(unsigned int i=0;i<12;i++)expected[i^4]=storage[5+19+6+(i/6)*19+(i%6)];
 assert(!fl2000_copy_frame(&test_drm_if,&state.base,NULL));
 assert(!memcmp(destination,expected,16) && !cpu_access_count);
 stream.frames[1].vaddr=second;
 assert(!fl2000_copy_frame(&test_drm_if,&state.base,NULL));
 storage[5+19+6]^=0x7f;expected[4]=storage[5+19+6];
 struct drm_rect crop_damage={2,1,3,2};
 assert(!fl2000_copy_frame(&test_drm_if,&state.base,&crop_damage));
 assert(!memcmp(destination,expected,16));
 assert(fl2000_streaming_update(&usb,storage,2,2,19,DRM_FORMAT_RGB565,NULL)==-EINVAL);
 assert(fl2000_streaming_update(&usb,storage,2,2,5,DRM_FORMAT_RGB888,NULL)==-EINVAL);
 assert(fl2000_streaming_update(&usb,storage,2,2,19,0,NULL)==-EINVAL);
 cpu_access_result=-EIO;
 assert(fl2000_copy_frame(&test_drm_if,&state.base,NULL)==-EIO && !cpu_access_count);
 cpu_access_result=0;test_drm_if.drm.unplugged=true;
 assert(fl2000_copy_frame(&test_drm_if,&state.base,NULL)==-ENODEV && !cpu_access_count);
 test_drm_if.drm.unplugged=false;
 /* Use this commit's CRTC state even if the live pointer has advanced. */
 struct drm_crtc_state active={.active=true},later={.active=false};
 test_drm_if.crtc.state=&later;
 struct drm_plane plane={.dev=&test_drm_if.drm};
 struct drm_atomic_state atomic={.crtc=&active,.new_plane=&state.base,.old_plane=&state.base};
 stream.enabled=true;int accesses=cpu_access_calls;
 damage_merged=false;
 fl2000_plane_atomic_update(&plane,&atomic);
 assert(cpu_access_calls==accesses);
 merged_damage=crop_damage;damage_merged=true;
 fl2000_plane_atomic_update(&plane,&atomic);
 assert(cpu_access_calls==++accesses && !memcmp(stream.latest->vaddr,expected,16));
 active.mode_changed=true;
 fl2000_plane_atomic_update(&plane,&atomic);
 assert(cpu_access_calls==accesses);
 active.mode_changed=false;active.active=false;
 fl2000_plane_atomic_update(&plane,&atomic);
 assert(cpu_access_calls==accesses);
 active.active=true;damage_merged=false;
 struct drm_plane_state detached={0};atomic.old_plane=&detached;
 fl2000_plane_atomic_update(&plane,&atomic);
 assert(cpu_access_calls==++accesses && !memcmp(stream.latest->vaddr,expected,16));
 state.base.fb=NULL;
 fl2000_plane_atomic_update(&plane,&atomic);
 assert(cpu_access_calls==accesses);
 test_drm_if.crtc.state=NULL;
}

static void test_partial_damage(void)
{
 const u32 formats[]={DRM_FORMAT_XRGB8888,DRM_FORMAT_XRGB8888,DRM_FORMAT_RGB888,DRM_FORMAT_RGB565};
 const unsigned int outputs[]={3,2,3,2},inputs[]={4,4,3,2};
 for(unsigned int f=0;f<4;f++) {
  enum { W=7,H=5,PITCH=37,SIZE=112 };
  struct usb_device usb={0};struct fl2000_stream stream={.usb_dev=&usb,.bytes_pix=outputs[f],.buf_size=ALIGN(W*H*outputs[f],8)};
  u8 src[PITCH*H],buffers[FL2000_FRAME_COUNT][SIZE],before[FL2000_FRAME_COUNT][SIZE],expected[SIZE];
  memset(buffers,0xa5,sizeof(buffers));for(unsigned int i=0;i<sizeof(src);i++)src[i]=i*17;
  for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)stream.frames[i].vaddr=buffers[i];
  test_stream=&stream;
  for(unsigned int step=0;step<200;step++) {
   struct drm_rect damage={step%W,(step/3)%H,0,0};damage.x2=damage.x1+1;damage.y2=damage.y1+1;
   for(unsigned int b=0;b<inputs[f];b++)src[damage.y1*PITCH+damage.x1*inputs[f]+b]^=step+b+1;
   unsigned int target=step%FL2000_FRAME_COUNT;
   if(&stream.frames[target]==stream.latest)target=(target+1)%FL2000_FRAME_COUNT;
   for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)stream.frames[i].users=i!=target && &stream.frames[i]!=stream.latest;
   memcpy(before,buffers,sizeof(before));
   assert(!fl2000_streaming_update(&usb,src,H,W,PITCH,formats[f],&damage));
   memset(expected,0,sizeof(expected));
   for(unsigned int y=0;y<H;y++)for(unsigned int x=0;x<W;x++) {
    const u8 *pixel=src+y*PITCH+x*inputs[f];unsigned int offset=(y*W+x)*outputs[f];
    if(inputs[f]==4 && outputs[f]==2) {
     u16 value=((pixel[2]>>3)<<11)|((pixel[1]>>2)<<5)|(pixel[0]>>3);
     expected[offset^4]=value;expected[(offset+1)^4]=value>>8;
    } else for(unsigned int b=0;b<outputs[f];b++)expected[(offset+b)^4]=pixel[b];
   }
   assert(stream.latest==&stream.frames[target]);
   assert(!memcmp(expected,buffers[target],stream.buf_size));
   for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)if(i!=target)assert(!memcmp(before[i],buffers[i],SIZE));
  }
  /* Failed publication must not lose damage needed by a later reuse. */
  struct drm_rect damage={0,0,1,1};src[0]^=123;
  for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)stream.frames[i].users=1;
  assert(fl2000_streaming_update(&usb,src,H,W,PITCH,formats[f],&damage)==-EBUSY);
  damage=(struct drm_rect){W-1,H-1,W,H};src[(H-1)*PITCH+(W-1)*inputs[f]]^=91;
  unsigned int target=stream.latest==&stream.frames[0]?1:0;stream.frames[target].users=0;
  assert(!fl2000_streaming_update(&usb,src,H,W,PITCH,formats[f],&damage));
  assert(buffers[target][4]==(outputs[f]==2 && inputs[f]==4 ? (((src[2]>>3)<<11)|((src[1]>>2)<<5)|(src[0]>>3))&255 : src[0]));
  /* Reusing a mapping or geometry must not retain another image's pixels. */
  u8 replacement[PITCH*7];memset(replacement,0x73,sizeof(replacement));
  for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)stream.frames[i].users=0;
  damage=(struct drm_rect){0,0,1,1};
  assert(!fl2000_streaming_update(&usb,replacement,7,5,PITCH,formats[f],&damage));
  memset(expected,0,sizeof(expected));
  for(unsigned int y=0;y<7;y++)for(unsigned int x=0;x<5;x++) {
   unsigned int offset=(y*5+x)*outputs[f];
   if(inputs[f]==4 && outputs[f]==2) {
    u16 value=((0x73>>3)<<11)|((0x73>>2)<<5)|(0x73>>3);
    expected[offset^4]=value;expected[(offset+1)^4]=value>>8;
   } else for(unsigned int b=0;b<outputs[f];b++)expected[(offset+b)^4]=0x73;
  }
  assert(!memcmp(stream.latest->vaddr,expected,stream.buf_size));
  damage=(struct drm_rect){-1,0,1,1};assert(fl2000_streaming_update(&usb,src,H,W,PITCH,formats[f],&damage)==-EINVAL);
  damage=(struct drm_rect){0,0,W+1,H};assert(fl2000_streaming_update(&usb,src,H,W,PITCH,formats[f],&damage)==-EINVAL);
 }
 test_stream=NULL;
}

static void test_modes(void)
{
 struct fl2000_pll pll;
 struct drm_display_mode mode = {
  .clock = 148500, .hdisplay = 1920, .hsync_start = 2008,
  .hsync_end = 2052, .htotal = 2200, .vdisplay = 1080,
  .vsync_start = 1084, .vsync_end = 1089, .vtotal = 1125,
 }, adjusted={0};
 assert(fl2000_get_bytes_pix(USB_SPEED_HIGH, 148500000) == 0);
 assert(fl2000_get_bytes_pix(USB_SPEED_SUPER, 148500000) == 3);
 assert(fl2000_get_bytes_pix(USB_SPEED_HIGH, 20000000) == 2);
 assert(fl2000_get_bytes_pix(USB_SPEED_HIGH, 0) == 0);
 assert(fl2000_active_pixel_rate(&mode) == 124416000);
 assert(!fl2000_mode_calc(&mode, &adjusted, &pll));
 /* A small clock sweep proves that horizontal adjustments are actually used. */
 bool found_adjustment = false;
 for (int clock = 140001; clock < 150000; clock += 13) {
  mode.clock = clock;
  if (!fl2000_mode_calc(&mode, &adjusted, &pll) && adjusted.htotal != mode.htotal) {
   found_adjustment = true;
   assert(abs(adjusted.htotal - mode.htotal) <= 10);
   assert(adjusted.htotal >= mode.hsync_end);
   break;
  }
 }
 assert(found_adjustment);
 struct drm_display_mode bridge_adjusted=adjusted,request=mode;
 bridge_adjusted.htotal+=2;
 bridge_adjusted.head.next=bridge_adjusted.head.prev=&bridge_adjusted;
 struct drm_display_mode expected={0};expected.head=bridge_adjusted.head;
 assert(!fl2000_mode_calc(&bridge_adjusted,&expected,&pll));
 assert(fl2000_crtc_mode_fixup(NULL,&request,&bridge_adjusted));
 assert(!memcmp(&bridge_adjusted,&expected,sizeof(expected)));
 assert(!memcmp(&request,&mode,sizeof(mode)));
 struct drm_display_mode invalid={.clock=0},before=invalid;
 assert(!fl2000_crtc_mode_fixup(NULL,&mode,&invalid));
 assert(!memcmp(&invalid,&before,sizeof(invalid)));
 for(int clock=25000;clock<=148500;clock+=500) {
  struct drm_display_mode boundary=mode;boundary.clock=clock;
  if(!fl2000_mode_calc(&boundary,&adjusted,&pll)) {
   assert(adjusted.clock>=25000 && adjusted.clock<=148500);
   assert(adjusted.crtc_clock==adjusted.clock && adjusted.crtc_htotal==adjusted.htotal);
  }
 }
 mode.clock = 0;
 assert(fl2000_mode_calc(&mode, &adjusted, &pll) == -EINVAL);
 mode.clock = 148500; mode.flags = DRM_MODE_FLAG_INTERLACE;
 assert(fl2000_mode_calc(&mode, &adjusted, &pll) == -EINVAL);
 mode.flags = 0; mode.vsync_end = mode.vsync_start + 8;
 assert(fl2000_mode_calc(&mode, &adjusted, &pll) == -EINVAL);
 mode.vsync_end = mode.vsync_start + 5; mode.htotal = 4096;
 assert(fl2000_mode_calc(&mode, &adjusted, &pll) == -EINVAL);
 assert(FL2000_VGA_CTRL_PXCLK_HSYNC_POLARITY == BIT(2));
 assert(FL2000_VGA_CTRL_PXCLK_VSYNC_POLARITY == BIT(3));

 /* Configure IRQ sources and acknowledgment even when reset defaults differ. */
 struct usb_device irq_usb={0};
 test_regmap=(struct regmap){.aclk=BIT(13)|BIT(4),.ctrl2=BIT(20)|BIT(18)};
 regmap_calls=0;
 assert(!fl2000_enable_interrupts(&irq_usb));
 assert(regmap_calls==2);
 assert((test_regmap.aclk & (BIT(2)|BIT(3)))==(BIT(2)|BIT(3)));
 assert(!(test_regmap.aclk & BIT(4)));
 assert(!(test_regmap.aclk & BIT(13)));
 assert((test_regmap.ctrl2 & (BIT(19)|BIT(20)))==BIT(19));
 assert(test_regmap.ctrl2 & BIT(18));
 regmap_error=-EIO;regmap_calls=0;
 assert(fl2000_enable_interrupts(&irq_usb)==-EIO && regmap_calls==1);
 regmap_error=0;
 test_regmap.aclk=BIT(25);detection_writes=detection_delay=0;
 assert(!fl2000_init_clock(&irq_usb));
 assert(!(test_regmap.aclk & BIT(25)) && (test_regmap.aclk & BIT(26)));
 assert((u64)FL2000_XTAL*FIELD_GET(FL2000_VGA_PLL_MULTIPLIER,test_regmap.pll)/
  FIELD_GET(FL2000_VGA_PLL_PRESCALER,test_regmap.pll)/
  FIELD_GET(FL2000_VGA_PLL_DIVISOR,test_regmap.pll)==25000000);
 assert(!fl2000_usb_magic(&irq_usb));
 assert(detection_writes==3 && detection_delay==750);
 assert((test_regmap.regs[0x8020/4] & (BIT(30)|BIT(28)))==(BIT(30)|BIT(28)));
 /* Datasheet 0x8064: HDMI enable is bit 19, polarity is bit 20. */
 assert(FL2000_VGA_CTRL2_ACLK_HDMI_INT_EN == BIT(19));
 assert(FL2000_VGA_CTRL2_ACLK_HDMI_INT_ACTIVE_HIGH == BIT(20));
 u32 control = (0x311c0118 | FL2000_VGA_CTRL2_ACLK_HDMI_INT_EN) &
  ~FL2000_VGA_CTRL2_ACLK_HDMI_INT_ACTIVE_HIGH;
 assert(control == 0x310c0118);
}


static void test_i2c(void)
{
 struct i2c_adapter probe_adapter={0};
 assert(fl2000_bridge_detect(&probe_adapter,0x4c)==1);
 assert(fl2000_bridge_detect(&probe_adapter,0x4d)==0);
 detect_bad_id=1;
 assert(fl2000_bridge_detect(&probe_adapter,0x4c)==0);
 detect_bad_id=0;detect_short=1;
 assert(fl2000_bridge_detect(&probe_adapter,0x4c)==0);
 detect_short=0;
 struct i2c_adapter adapter = {0};
 u8 reg = 0xff, value = 0;
 struct i2c_msg msgs[] = {
  { .addr = 0x4c, .len = 1, .buf = &reg },
  { .addr = 0x4c, .flags = I2C_M_RD, .len = 1, .buf = &value },
 };
 i2c_value = 0x12345678;
 assert(fl2000_i2c_xfer(&adapter, msgs, 2) == 2 && value == 0x12);
 u8 group[4], aligned = 0xfc;
 msgs[0].buf = &aligned; msgs[1].buf = group; msgs[1].len = 4;
 assert(fl2000_i2c_xfer(&adapter, msgs, 2) == 2);
 assert(group[0] == 0x78 && group[3] == 0x12);
 /* The DMA-safe annotation is independent for each message and read size. */
 for(unsigned int len=1;len<=4;len+=3) {
  msgs[1].len=len;
  for(unsigned int flags=0;flags<4;flags++) {
   msgs[0].flags=(flags&1)?I2C_M_DMA_SAFE:0;
   msgs[1].flags=I2C_M_RD|((flags&2)?I2C_M_DMA_SAFE:0);
   assert(fl2000_i2c_xfer(&adapter,msgs,2)==2 && group[0]==0x78);
  }
 }
 int rejected_calls=i2c_calls;
 for(unsigned int bit=0;bit<16;bit++) {
  unsigned int flag=1U<<bit;
  if(flag==I2C_M_DMA_SAFE)continue;
  msgs[0].flags=I2C_M_DMA_SAFE|flag;msgs[1].flags=I2C_M_RD|I2C_M_DMA_SAFE;
  assert(fl2000_i2c_xfer(&adapter,msgs,2)==-EOPNOTSUPP);
  msgs[0].flags=I2C_M_DMA_SAFE;msgs[1].flags=I2C_M_RD|I2C_M_DMA_SAFE|flag;
  if(flag!=I2C_M_RD)assert(fl2000_i2c_xfer(&adapter,msgs,2)==-EOPNOTSUPP);
 }
 msgs[1].flags=I2C_M_DMA_SAFE;
 assert(fl2000_i2c_xfer(&adapter,msgs,2)==-EOPNOTSUPP); /* Missing read bit. */
 assert(i2c_calls==rejected_calls);
 msgs[0].flags=0;msgs[1].flags=I2C_M_RD;msgs[1].len=4;
 aligned++;
 assert(fl2000_i2c_xfer(&adapter, msgs, 2) == -EOPNOTSUPP);
 u8 full[] = {0xfc, 1, 2, 3, 4};
 msgs[0].buf = full; msgs[0].len = 5;
 int before = i2c_calls;
 assert(fl2000_i2c_xfer(&adapter, msgs, 1) == 1);
 assert(i2c_calls == before + 1 && i2c_value == 0x04030201);
 msgs[0].flags=I2C_M_DMA_SAFE;
 before=i2c_calls;
 assert(fl2000_i2c_xfer(&adapter,msgs,1)==1 && i2c_calls==before+1);
 u8 byte_write[]={0xff,0xab};msgs[0].buf=byte_write;msgs[0].len=2;
 before=i2c_calls;
 assert(fl2000_i2c_xfer(&adapter,msgs,1)==1 && i2c_calls==before+2);
 assert(i2c_value==0xab030201); /* Preserve the other three bytes. */
 msgs[0].flags=0;
 msgs[0].buf = &reg; msgs[0].len = 1;
 msgs[1].buf = &value; msgs[1].len = 1; i2c_value = 0x12345678;
 int calls = i2c_calls;
 msgs[0].len = 0;
 assert(fl2000_i2c_xfer(&adapter, msgs, 2) == -EOPNOTSUPP);
 msgs[0].len = 1; msgs[1].addr = 0x4d;
 assert(fl2000_i2c_xfer(&adapter, msgs, 2) == -EOPNOTSUPP);
 assert(fl2000_i2c_xfer(&adapter, NULL, 0) == -EOPNOTSUPP);
 assert(calls == i2c_calls);
 msgs[1].addr = 0x4c;
 i2c_result = -ENODEV;
 assert(fl2000_i2c_xfer(&adapter, msgs, 2) == -ENODEV);
 i2c_result = 0;
 u8 write[] = {0xff, 0xab};
 msgs[0].len = 2; msgs[0].buf = write;
 assert(fl2000_i2c_xfer(&adapter, msgs, 1) == 1);
 assert(i2c_value == 0xab345678);
}


static void test_frame_allocation(void)
{
 struct usb_bus bus={.sg_tablesize=1024};
 struct usb_device usb={.bus=&bus};struct fl2000_frame frame={0};
 assert(!fl2000_alloc_frame(&usb,&frame,8192));
 assert(frame.dma_sgt && !frame.pages && frame.size==8192 && dma_allocs==1);
 for(unsigned int i=0;i<8192;i++)assert(!frame.vaddr[i]);
 fl2000_free_frame(&usb,&frame);
 assert(dma_frees==1 && !frame.vaddr && !frame.dma_sgt);
 fail_dma_alloc=true;
 assert(!fl2000_alloc_frame(&usb,&frame,8192));
 assert(!frame.dma_sgt && frame.nr_pages==2 && page_allocs==2);
 fl2000_free_frame(&usb,&frame);
 assert(page_frees==2);
 fail_page_at=page_allocs+2;
 assert(fl2000_alloc_frame(&usb,&frame,12288)==-ENOMEM);
 fl2000_free_frame(&usb,&frame);
 assert(page_frees==3 && !frame.pages);
 fail_dma_alloc=false;fail_page_at=0;
}

static void test_stream_preparation(void)
{
 struct usb_bus bus={.sg_tablesize=1024};
 struct usb_device usb={.bus=&bus,.speed=USB_SPEED_SUPER};
 struct fl2000_stream stream={.usb_dev=&usb};
 struct urb urbs[FL2000_QUEUE_DEPTH]={0};
 struct fl2000_stream_buffers *prepared;
 unsigned int dma_live=dma_allocs-dma_frees, pages_live=page_allocs-page_frees;
 test_stream=&stream;fail_at=0;
 assert(PTR_ERR(fl2000_streaming_prepare(&usb,0,3))==-EINVAL);
 assert(PTR_ERR(fl2000_streaming_prepare(&usb,-1,3))==-EINVAL);
 assert(PTR_ERR(fl2000_streaming_prepare(&usb,16,4))==-EINVAL);
 assert(PTR_ERR(fl2000_streaming_prepare(&usb,INT_MAX,3))==-EINVAL);
 for(unsigned int i=0;i<FL2000_QUEUE_DEPTH;i++) {
  stream.transfers[i].stream=&stream;stream.transfers[i].urb=&urbs[i];
  urbs[i].context=&stream.transfers[i];
 }
 prepared=fl2000_streaming_prepare(&usb,16,3);assert(!IS_ERR(prepared));
 assert(!fl2000_streaming_mode_set(&usb,&prepared) && !prepared);
 stream.latest=&stream.frames[0];stream.frames[0].valid=true;
 assert(!fl2000_streaming_enable(&usb));
 u8 *active=stream.frames[0].vaddr;
 active[0]=0xa5;
 /* Fail every frame position through the page fallback. */
 fail_dma_alloc=true;
 for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++) {
  unsigned int freed=page_frees,allocated=page_allocs;
  fail_page_at=page_allocs+i+1;
  prepared=fl2000_streaming_prepare(&usb,16,2);
  assert(PTR_ERR(prepared)==-ENOMEM);
  assert(page_frees-freed==page_allocs-allocated-1);
  assert(stream.enabled && stream.buf_size==48 && stream.bytes_pix==3);
  assert(stream.latest==&stream.frames[0] && active==stream.frames[0].vaddr && active[0]==0xa5);
  for(unsigned int j=0;j<FL2000_QUEUE_DEPTH;j++)assert(urbs[j].submitted);
 }
 fail_page_at=0;
 /* SG allocation errors must unwind complete frames and partial tables. */
 for(unsigned int i=0;i<FL2000_QUEUE_DEPTH;i++) {
  unsigned int freed=sg_frees;
  fail_sg_at=sg_allocs+i+1;
  prepared=fl2000_streaming_prepare(&usb,16,2);
  assert(PTR_ERR(prepared)==-ENOMEM && sg_frees-freed==i);
  assert(stream.enabled && stream.frames[0].vaddr==active);
 }
 fail_sg_at=0;bus.sg_tablesize=0;
 prepared=fl2000_streaming_prepare(&usb,16,2);
 assert(PTR_ERR(prepared)==-EOPNOTSUPP && stream.enabled);
 fail_dma_alloc=false;
 prepared=fl2000_streaming_prepare(&usb,16,2);assert(!IS_ERR(prepared));
 for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)assert(prepared->frames[i].dma_sgt);
 for(unsigned int i=0;i<FL2000_QUEUE_DEPTH;i++)assert(!prepared->tables[i].sgl);
 fl2000_streaming_free_buffers(&usb,prepared);
 fail_dma_alloc=true;
 bus.sg_tablesize=1024;
 prepared=fl2000_streaming_prepare(&usb,16,2);assert(!IS_ERR(prepared));
 fl2000_streaming_free_buffers(&usb,prepared); /* Cancelled pending commit. */
 assert(stream.enabled && stream.frames[0].vaddr==active);
 prepared=fl2000_streaming_prepare(&usb,16,2);assert(!IS_ERR(prepared));
 unsigned int allocations=page_allocs+dma_allocs+sg_allocs;
 assert(!fl2000_streaming_mode_set(&usb,&prepared) && !prepared);
 assert(allocations==page_allocs+dma_allocs+sg_allocs);
 assert(!stream.enabled && !stream.latest && !stream.source);
 assert(stream.buf_size==32 && stream.bytes_pix==2 && !stream.frames[0].valid);
 for(unsigned int i=0;i<FL2000_QUEUE_DEPTH;i++) {
  assert(!urbs[i].submitted && !stream.transfers[i].sg_frame);
  sg_free_table(&stream.transfers[i].sgt);
 }
 for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)fl2000_free_frame(&usb,&stream.frames[i]);
 assert(dma_allocs-dma_frees==dma_live);
 /* Five intentionally failed alloc_page calls increment the mock counter. */
 assert(page_allocs-page_frees==pages_live+FL2000_FRAME_COUNT);
 fail_dma_alloc=false;test_stream=NULL;

 /* prepare_fb must return allocation errors and unwind its mapping itself. */
 const struct drm_format_info rgb={DRM_FORMAT_RGB888,{3}};
 struct drm_framebuffer fb={.format=&rgb};
 struct fl2000_plane_state state={0};
 struct drm_plane plane={.dev=&test_drm_if.drm};
 struct drm_crtc_state crtc={.active=true,.mode_changed=true,
  .adjusted_mode={.clock=25175,.hdisplay=640,.htotal=800,.vdisplay=480,.vtotal=525}};
 struct drm_atomic_state atomic={.crtc=&crtc};
 state.shadow.base=(struct drm_plane_state){.fb=&fb,.crtc=&test_drm_if.crtc,.state=&atomic};
 test_drm_if.usb_dev=&usb;
 fail_dma_alloc=true;fail_page_at=page_allocs+1;
 assert(fl2000_prepare_fb(&plane,&state.shadow.base)==-ENOMEM);
 assert(!state.prepared && !map_refs);
 fail_dma_alloc=false;fail_page_at=0;
 assert(!fl2000_prepare_fb(&plane,&state.shadow.base) && state.prepared && map_refs==1);
 fl2000_cleanup_fb(&plane,&state.shadow.base);
 assert(!state.prepared && !map_refs && dma_allocs-dma_frees==dma_live);
 crtc.mode_changed=false;
 unsigned int allocated=dma_allocs;
 assert(!fl2000_prepare_fb(&plane,&state.shadow.base) && !state.prepared);
 assert(dma_allocs==allocated); /* Ordinary page flips allocate no wire buffers. */
 fl2000_cleanup_fb(&plane,&state.shadow.base);
 test_drm_if.drm.unplugged=true;crtc.mode_changed=true;
 assert(fl2000_prepare_fb(&plane,&state.shadow.base)==-ENODEV && !map_refs);
 test_drm_if.drm.unplugged=false;
}

static void test_atomic_state(void)
{
 struct drm_plane plane={0};
 struct drm_crtc_state crtc={ .mode={.clock=40000,.hdisplay=800,.hsync_start=840,.hsync_end=968,.htotal=1056,.vdisplay=600,.vsync_start=601,.vsync_end=605,.vtotal=628}, .mode_changed=true };
 struct usb_device usb={.speed=USB_SPEED_SUPER};test_drm_if.usb_dev=&usb;
 const struct drm_format_info xrgb={DRM_FORMAT_XRGB8888,{4}};
 struct drm_framebuffer fb={.format=&xrgb};
 struct fl2000_plane_state old={.shadow={.base={.fb=&fb,.src_w=800U<<16,.src_h=600U<<16}}}, new=old;
 int calls=mode_calls;
 crtc.adjusted_mode=crtc.mode;
 assert(fl2000_crtc_mode_fixup(&test_drm_if.crtc,&crtc.mode,&crtc.adjusted_mode));
 assert(mode_calls==calls+1);
 assert(crtc.adjusted_mode.crtc_clock==crtc.adjusted_mode.clock);
 assert(crtc.adjusted_mode.crtc_htotal==crtc.adjusted_mode.htotal);
 assert(!fl2000_display_check(&test_drm_if,&old.shadow.base,&crtc));
 assert(mode_calls==calls+1 && !crtc.no_vblank);
 calls=mode_calls; crtc.mode_changed=false;
 struct drm_display_mode adjusted=crtc.adjusted_mode;
 for(int i=0;i<100;i++)assert(!fl2000_display_check(&test_drm_if,&old.shadow.base,&crtc));
 assert(mode_calls==calls && !memcmp(&adjusted,&crtc.adjusted_mode,sizeof(adjusted)));
 old.shadow.base.src_x=1;
 assert(fl2000_display_check(&test_drm_if,&old.shadow.base,&crtc)==-EINVAL);
 old.shadow.base.src_x=0;
 const struct drm_format_info rgb888={DRM_FORMAT_RGB888,{3}},rgb565={DRM_FORMAT_RGB565,{2}};
 usb.speed=USB_SPEED_HIGH;
 crtc.mode=(struct drm_display_mode){.clock=25175,.hdisplay=640,.hsync_start=656,.hsync_end=752,.htotal=800,.vdisplay=480,.vsync_start=490,.vsync_end=492,.vtotal=525};
 crtc.adjusted_mode=crtc.mode;
 assert(fl2000_crtc_mode_fixup(&test_drm_if.crtc,&crtc.mode,&crtc.adjusted_mode));
 crtc.mode_changed=true;old.shadow.base.src_w=640U<<16;old.shadow.base.src_h=480U<<16;
 fb.format=&rgb888;
 assert(fl2000_display_check(&test_drm_if,&old.shadow.base,&crtc)==-EINVAL);
 fb.format=&rgb565;
 assert(!fl2000_display_check(&test_drm_if,&old.shadow.base,&crtc));
 crtc.mode_changed=false;fb.format=&rgb888;
 assert(fl2000_display_check(&test_drm_if,&old.shadow.base,&crtc)==-EINVAL);
 fb.format=&xrgb;usb.speed=USB_SPEED_SUPER;
 /* CRTC-only changes must pull in planes; failures must propagate unchanged. */
 struct drm_atomic_state atomic={.crtc=&crtc,.new_plane=&old.shadow.base};
 primary_check_result=-EINVAL;
 assert(fl2000_crtc_atomic_check(&test_drm_if.crtc,&atomic)==-EINVAL && !affected_calls);
 primary_check_result=0;affected_result=-EDEADLK;
 assert(fl2000_crtc_atomic_check(&test_drm_if.crtc,&atomic)==-EDEADLK && affected_calls==1);
 affected_result=0;
 assert(!fl2000_crtc_atomic_check(&test_drm_if.crtc,&atomic) && affected_calls==2 && crtc.no_vblank);
 plane.dev=&test_drm_if.drm;
 plane_check_result=-ERANGE;
 assert(fl2000_plane_atomic_check(&plane,&atomic)==-ERANGE);
 plane_check_result=0;plane_visible=false;old.shadow.base.fb=NULL;
 assert(!fl2000_plane_atomic_check(&plane,&atomic));
 old.shadow.base.fb=&fb;plane_visible=true;crtc.no_vblank=false;
 assert(!fl2000_plane_atomic_check(&plane,&atomic) && !crtc.no_vblank);
 old.shadow.base.src_w-=1;
 assert(fl2000_plane_atomic_check(&plane,&atomic)==-EINVAL);
 old.shadow.base.src_w+=1;
 assert(!fl2000_prepare_fb(&plane,&old.shadow.base));
 assert(!fl2000_prepare_fb(&plane,&new.shadow.base));
 fl2000_cleanup_fb(&plane,&old.shadow.base);
 assert(map_refs==1 && map_creations==1 && !map_removals);
 fl2000_cleanup_fb(&plane,&new.shadow.base);
 assert(!map_refs && map_removals==1);
 prepare_result=-ENOMEM;
 assert(fl2000_prepare_fb(&plane,&new.shadow.base)==-ENOMEM && !map_refs);
 prepare_result=0;map_result=-ENOMEM;
 assert(fl2000_prepare_fb(&plane,&new.shadow.base)==-ENOMEM && !map_refs);
 map_result=0;map_iomem=true;
 assert(fl2000_prepare_fb(&plane,&new.shadow.base)==-EOPNOTSUPP && !map_refs);
 map_iomem=false;new.shadow.base.fb=NULL;
 assert(!fl2000_prepare_fb(&plane,&new.shadow.base));
 fl2000_cleanup_fb(&plane,&new.shadow.base);
}

static void test_register_io(void)
{
 struct usb_device usb = {0};
 const int results[] = {-ENODEV, 0, 1, 2, 3, 4};
 for (unsigned int i = 0; i < ARRAY_SIZE(results); i++) {
  u32 value = 0xa5a5a5a5;
  control_result = results[i]; control_value = 0x12345678;
  int ret = fl2000_reg_read(&usb, 0x1234, &value);
  if (control_result == 4) assert(!ret && value == control_value);
  else {
   assert(ret == (control_result < 0 ? control_result : -EIO));
   assert(value == 0xa5a5a5a5);
  }
  ret = fl2000_reg_write(&usb, 0x1234, 0x12345678);
  assert(ret == (control_result == 4 ? 0 : control_result < 0 ? control_result : -EIO));
 }
}

static struct fl2000_stream *fl2000_find_stream(struct usb_device *usb_dev)
{ (void)usb_dev; return test_stream; }
static void usb_kill_urb(struct urb *urb)
{
 if (urb && urb->submitted) {
  urb->submitted = false;
  urb->status = -ENOENT;
  urb->actual_length = 0;
  fl2000_data_completion(urb);
 }
}
static void complete(struct urb *urb, int status)
{
 assert(urb->submitted);
 urb->submitted = false;
 urb->status = status;
 urb->actual_length = urb->transfer_buffer_length;
 fl2000_data_completion(urb);
}
static void test_queue(void)
{
 struct fl2000_stream stream = { .buf_size = 8 };
 struct usb_device usb = {0};
 struct urb urbs[FL2000_QUEUE_DEPTH] = {0};
 struct scatterlist lists[FL2000_QUEUE_DEPTH] = {0};
 struct page pages[FL2000_FRAME_COUNT] = {0};
 struct page *pointers[FL2000_FRAME_COUNT];
 test_stream = &stream;
 stream.usb_dev = &usb;
 for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++) {
  pointers[i] = &pages[i];
  stream.frames[i].pages = &pointers[i];
 }
 for (unsigned int i = 0; i < FL2000_QUEUE_DEPTH; i++) {
  stream.transfers[i].stream = &stream;
  stream.transfers[i].urb = &urbs[i];
  stream.transfers[i].sgt.sgl = &lists[i];
  stream.transfers[i].sgt.orig_nents = 1;
  urbs[i].context = &stream.transfers[i];
 }
 stream.latest = &stream.frames[0];
 submit_count = fail_at = log_count = 0;
 assert(!fl2000_streaming_enable(&usb));
 assert(submit_count == FL2000_QUEUE_DEPTH);
 assert(stream.frames[0].users == FL2000_QUEUE_DEPTH);
 assert(urbs[0].sg != urbs[1].sg && urbs[1].sg != urbs[2].sg);
 unsigned int sets=sg_sets;
 complete(&urbs[0],0);
 assert(sg_sets==sets); /* Identical frame does not rebuild SG entries. */
 stream.latest = &stream.frames[1];
 complete(&urbs[0], 0);
 assert(sg_sets==sets+1);
 assert(stream.frames[0].users == 2 && stream.frames[1].users == 1);
 assert(!log_count); /* Resubmission changes urb->status to EINPROGRESS. */
 fl2000_stop_stream(&stream);
 assert(!stream.enabled);
 for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++) assert(!stream.frames[i].users);
 struct sg_table dma_sgt={0};
 stream.frames[1].dma_sgt=&dma_sgt;stream.frames[1].dma=0x12340000;
 stream.frames[1].vaddr=(u8 *)1;
 sets=sg_sets;
 assert(!fl2000_streaming_enable(&usb));
 for(unsigned int i=0;i<FL2000_QUEUE_DEPTH;i++) {
  assert(urbs[i].sgt==&dma_sgt && !urbs[i].sg && !urbs[i].num_sgs);
  assert(urbs[i].transfer_flags & URB_NO_TRANSFER_DMA_MAP);
  assert(urbs[i].transfer_dma==0x12340000 && urbs[i].transfer_buffer==(u8 *)1);
 }
 assert(sg_sets==sets);
 complete(&urbs[0],0);
 fl2000_stop_stream(&stream);
 for(unsigned int i=0;i<FL2000_FRAME_COUNT;i++)assert(!stream.frames[i].users);
 stream.frames[1].dma_sgt=NULL;
 submit_count = 0; fail_at = 2;
 assert(fl2000_streaming_enable(&usb) == -ENOMEM);
 assert(!stream.enabled);
 for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++) assert(!stream.frames[i].users);
 submit_count = fail_at = 0;
 assert(!fl2000_streaming_enable(&usb));
 complete(&urbs[1], -EPIPE);
 assert(!stream.enabled);
 assert(!fl2000_streaming_enable(&usb)); /* Drains remaining URBs before recovery. */
 assert(stream.enabled);
 fl2000_stop_stream(&stream);
 for (unsigned int i = 0; i < FL2000_FRAME_COUNT; i++) assert(!stream.frames[i].users);
}
int main(void)
{
 test_damage_simd_boundaries(); test_partial_damage(); test_conversion(); test_native_reordering(); test_native_formats(); test_native_crop(); test_modes(); test_i2c(); test_register_io(); test_queue(); test_atomic_state(); test_frame_allocation(); test_stream_preparation();
#ifdef TEST_SIMD
 assert(!fpu_depth);
 simd_cpu_available=false;
 test_damage_simd_boundaries(); test_conversion(); test_native_reordering();
 simd_cpu_available=true; simd_context_available=false;
 test_damage_simd_boundaries(); test_conversion(); test_native_reordering();
#endif
 puts("PASS: conversion, PLL/modes, masks, I2C validation, short USB replies, queue failure/recovery, persistent DMA, mapping lifetime, unchanged-mode checks, native formats/padding/crops/switches, partial damage/busy buffers, atomic buffer preparation/failure/unwind");
 return 0;
}
"""
parts = [
    prelude,
    defines,
    '#include "tests/register_helpers.h"',
    register_header((ROOT / "fl2000_registers.h").read_text()),
    '#include "bridge/it66121_registers.h"',
    structure(header, "fl2000_pll"),
]
parts += [
    function(drm, name)
    for name in (
        "fl2000_get_bytes_pix",
        "fl2000_active_pixel_rate",
        "fl2000_frame_bytes_pix",
        "fl2000_pll_ppm_err",
        "fl2000_pll_get_divisor",
        "fl2000_pll_calc",
        "fl2000_mode_calc",
        "fl2000_crtc_mode_fixup",
        "fl2000_atomic_check",
        "fl2000_display_check",
        "fl2000_crtc_atomic_check",
        "fl2000_plane_atomic_check",
    )
]
parts = [
    part.replace(
        "const int max_h_adjustment = 10;",
        "const int max_h_adjustment = 10; mode_calls++;",
    )
    for part in parts
]
parts += [
    structure(stream, name)
    for name in (
        "fl2000_frame",
        "fl2000_transfer",
        "fl2000_stream_buffers",
        "fl2000_stream",
    )
]
parts += [
    function(stream, name)
    for name in (
        "fl2000_free_frame",
        "fl2000_alloc_frame",
        "fl2000_rgb565",
        "fl2000_rgb565_pair",
        "fl2000_convert_line",
        "fl2000_convert_frame",
        "fl2000_reorder_line",
        "fl2000_reorder_frame",
        "fl2000_source_bytes_pix",
        "fl2000_damage_add",
        "fl2000_convert_damage",
        "fl2000_frame_layout_valid",
        "fl2000_find_free_frame",
        "fl2000_streaming_update",
        "fl2000_submit_transfer",
        "fl2000_data_completion",
        "fl2000_stop_stream",
        "fl2000_streaming_free_buffers",
        "fl2000_streaming_prepare",
        "fl2000_streaming_mode_set",
        "fl2000_streaming_enable",
    )
]
parts.append(structure(drm, "fl2000_plane_state"))
parts += [
    function(drm, name)
    for name in (
        "fl2000_plane_state",
        "fl2000_prepare_fb",
        "fl2000_cleanup_fb",
    )
]
parts.append(function(drm, "fl2000_copy_frame"))
parts.append(function(drm, "fl2000_plane_atomic_update"))
parts += [
    function(i2c, name)
    for name in (
        "fl2000_i2c_read_dword",
        "fl2000_i2c_write_dword",
        "fl2000_i2c_xfer_valid",
        "fl2000_i2c_xfer",
    )
]
parts.append(
    "static int i2c_transfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num) { return detect_short ? 0 : fl2000_i2c_xfer(adapter, msgs, num); }"
)
parts.append(function(i2c, "fl2000_bridge_detect"))
parts += [function(registers, name) for name in ("fl2000_reg_read", "fl2000_reg_write")]
parts += [
    function(registers, name)
    for name in ("fl2000_enable_interrupts", "fl2000_set_pll", "fl2000_usb_magic")
]
parts.append(function(drm, "fl2000_init_clock"))
parts.append(checks)
with tempfile.TemporaryDirectory(prefix="fl2000-tests-") as temp:
    source = Path(temp) / "test.c"
    executable = Path(temp) / "test"
    variants = [("generic", [], [])]
    if os.uname().machine == "x86_64" and "ssse3" in Path("/proc/cpuinfo").read_text():
        simd = (
            (ROOT / "frame/ssse3.c")
            .read_text()
            .replace("#include <linux/types.h>", "")
            .replace('#include "ssse3.h"', '#include "frame/ssse3.h"')
        )
        wrapper = (ROOT / "frame/x86.c").read_text()
        wrapper = re.sub(r"^#include.*$", "", wrapper, flags=re.MULTILINE)
        mock = """
#define X86_FEATURE_SSSE3 0
#define boot_cpu_has(x) (simd_cpu_available && __builtin_cpu_supports("ssse3"))
#define irq_fpu_usable() simd_context_available
static bool simd_cpu_available = true, simd_context_available = true;
static int fpu_depth;
#define kernel_fpu_begin() assert(!fpu_depth++)
#define kernel_fpu_end() assert(fpu_depth-- == 1)
"""
        variants.append(
            ("ssse3", ["-DTEST_SIMD", "-mssse3", "-mno-avx"], [mock, simd, wrapper])
        )
    if os.uname().machine == "x86_64" and "bmi2" in Path("/proc/cpuinfo").read_text():
        bmi = (ROOT / "frame/bmi2.c").read_text()
        bmi = re.sub(r"^#include <linux/.*$", "", bmi, flags=re.MULTILINE)
        bmi = bmi.replace('#include "bmi2.h"', '#include "frame/bmi2.h"')
        wrapper = (ROOT / "frame/x86_bmi2.c").read_text()
        wrapper = re.sub(r"^#include.*$", "", wrapper, flags=re.MULTILINE)
        mock = """
#define X86_FEATURE_BMI2 1
#define boot_cpu_has(x) (simd_cpu_available && __builtin_cpu_supports("bmi2"))
static bool simd_cpu_available = true, simd_context_available = true;
static int fpu_depth;
"""
        variants.append(("bmi2", ["-DTEST_SIMD", "-mbmi2"], [mock, bmi, wrapper]))
    for name, flags, backend in variants:
        source.write_text("\n\n".join([parts[0], *backend, *parts[1:]]))
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-no-pie",
                *flags,
                "-I",
                str(ROOT),
                str(source),
                "-o",
                str(executable),
            ],
            check=True,
        )
        print("Frame backend:", name, flush=True)
        subprocess.run([str(executable)], check=True)
