// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <drm.h>
#include <drm_fourcc.h>
static void check(int result, const char *what)
{
	if (result < 0) {
		perror(what);
		exit(1);
	}
}
static double seconds(clockid_t clock)
{
	struct timespec t;
	clock_gettime(clock, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static void profile(int enabled)
{
	int fd = open("/sys/kernel/debug/tracing/function_profile_enabled", O_WRONLY);
	if (fd >= 0) {
		if (write(fd, enabled ? "1" : "0", 1) != 1)
			perror("profile");
		close(fd);
	}
}
/* CPU accounting runs separately from ftrace, whose overhead is measurable. */
struct cpu_sample {
	unsigned long long busy, irq, softirq, steal;
};
static struct cpu_sample sample(void)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		perror("/proc/stat");
		exit(1);
	}
	unsigned long long v[8] = { 0 };
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
		   &v[4], &v[5], &v[6], &v[7]) != 8)
		exit(1);
	fclose(f);
	return (struct cpu_sample){ v[0] + v[1] + v[2] + v[5] + v[6], v[5], v[6], v[7] };
}
static void account(const char *format, const char *phase, struct cpu_sample before, double start,
		    double cpu)
{
	struct cpu_sample after = sample();
	double elapsed = seconds(CLOCK_MONOTONIC) - start, hz = sysconf(_SC_CLK_TCK);
	printf("CPU %s %s wall=%.3f guest_busy=%.3f%% irq=%.3f%% softirq=%.3f%% steal=%.3f%% process=%.6fs\n",
	       format, phase, elapsed, 100 * (after.busy - before.busy) / hz / elapsed,
	       100 * (after.irq - before.irq) / hz / elapsed,
	       100 * (after.softirq - before.softirq) / hz / elapsed,
	       100 * (after.steal - before.steal) / hz / elapsed,
	       seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu);
}
static unsigned int duration = 10;
static void workload(int fd, unsigned char *pixels, struct drm_mode_fb_dirty_cmd *dirty,
		     const char *format, const char *phase, int frames)
{
	printf("WORKLOAD_BEGIN %s %s\n", format, phase);
	struct cpu_sample before = sample();
	double start = seconds(CLOCK_MONOTONIC), cpu = seconds(CLOCK_PROCESS_CPUTIME_ID);
	if (!frames)
		sleep(duration);
	for (int frame = 0; frame < frames; frame++) {
		pixels[0] = frame;
		check(ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, dirty), "dirty framebuffer");
		double deadline = start + (frame + 1) / 60.0;
		struct timespec until = { (time_t)deadline,
					  (long)((deadline - (time_t)deadline) * 1e9) };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
	}
	account(format, phase, before, start, cpu);
}
static void report_profile(const char *format)
{
	char buffer[4096];
	FILE *file = fopen("/sys/kernel/debug/tracing/trace_stat/function0", "r");
	if (!file)
		return;
	while (fgets(buffer, sizeof(buffer), file))
		printf("FUNCTION %s %s", format, buffer);
	fclose(file);
}
#ifdef __x86_64__
static void check_sse_state(int fd, struct drm_mode_fb_dirty_cmd *dirty)
{
	unsigned char before[256], after[256];
	for (unsigned int i = 0; i < sizeof(before); i++)
		before[i] = (i * 71U + 13U) & 255;
	long result;
	__asm__ volatile("movdqu 0(%[before]), %%xmm0\n\t"
			 "movdqu 16(%[before]), %%xmm1\n\t"
			 "movdqu 32(%[before]), %%xmm2\n\t"
			 "movdqu 48(%[before]), %%xmm3\n\t"
			 "movdqu 64(%[before]), %%xmm4\n\t"
			 "movdqu 80(%[before]), %%xmm5\n\t"
			 "movdqu 96(%[before]), %%xmm6\n\t"
			 "movdqu 112(%[before]), %%xmm7\n\t"
			 "movdqu 128(%[before]), %%xmm8\n\t"
			 "movdqu 144(%[before]), %%xmm9\n\t"
			 "movdqu 160(%[before]), %%xmm10\n\t"
			 "movdqu 176(%[before]), %%xmm11\n\t"
			 "movdqu 192(%[before]), %%xmm12\n\t"
			 "movdqu 208(%[before]), %%xmm13\n\t"
			 "movdqu 224(%[before]), %%xmm14\n\t"
			 "movdqu 240(%[before]), %%xmm15\n\t"
			 "syscall\n\t"
			 "movdqu %%xmm0, 0(%[after])\n\t"
			 "movdqu %%xmm1, 16(%[after])\n\t"
			 "movdqu %%xmm2, 32(%[after])\n\t"
			 "movdqu %%xmm3, 48(%[after])\n\t"
			 "movdqu %%xmm4, 64(%[after])\n\t"
			 "movdqu %%xmm5, 80(%[after])\n\t"
			 "movdqu %%xmm6, 96(%[after])\n\t"
			 "movdqu %%xmm7, 112(%[after])\n\t"
			 "movdqu %%xmm8, 128(%[after])\n\t"
			 "movdqu %%xmm9, 144(%[after])\n\t"
			 "movdqu %%xmm10, 160(%[after])\n\t"
			 "movdqu %%xmm11, 176(%[after])\n\t"
			 "movdqu %%xmm12, 192(%[after])\n\t"
			 "movdqu %%xmm13, 208(%[after])\n\t"
			 "movdqu %%xmm14, 224(%[after])\n\t"
			 "movdqu %%xmm15, 240(%[after])\n\t"
			 : "=a"(result)
			 : "0"((long)SYS_ioctl), "D"((long)fd), "S"((long)DRM_IOCTL_MODE_DIRTYFB),
			   "d"(dirty), [before] "r"(before), [after] "r"(after)
			 : "rcx", "r11", "cc", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
			   "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
			   "xmm13", "xmm14", "xmm15");
	if (result < 0 || memcmp(before, after, sizeof(before))) {
		fprintf(stderr, "check_sse_state: register preservation failed\n");
		exit(1);
	}
}
static __attribute__((target("avx"))) void check_avx_state(int fd,
							   struct drm_mode_fb_dirty_cmd *dirty)
{
	unsigned char before[256], after[256];
	for (unsigned int i = 0; i < sizeof(before); i++)
		before[i] = (i * 71U + 13U) & 255;
	long result;
	__asm__ volatile("vmovdqu 0(%[before]), %%ymm0\n\t"
			 "vmovdqu 32(%[before]), %%ymm1\n\t"
			 "vmovdqu 64(%[before]), %%ymm2\n\t"
			 "vmovdqu 96(%[before]), %%ymm3\n\t"
			 "vmovdqu 128(%[before]), %%ymm4\n\t"
			 "vmovdqu 160(%[before]), %%ymm5\n\t"
			 "vmovdqu 192(%[before]), %%ymm6\n\t"
			 "vmovdqu 224(%[before]), %%ymm7\n\t"
			 "syscall\n\t"
			 "vmovdqu %%ymm0, 0(%[after])\n\t"
			 "vmovdqu %%ymm1, 32(%[after])\n\t"
			 "vmovdqu %%ymm2, 64(%[after])\n\t"
			 "vmovdqu %%ymm3, 96(%[after])\n\t"
			 "vmovdqu %%ymm4, 128(%[after])\n\t"
			 "vmovdqu %%ymm5, 160(%[after])\n\t"
			 "vmovdqu %%ymm6, 192(%[after])\n\t"
			 "vmovdqu %%ymm7, 224(%[after])\n\t"
			 : "=a"(result)
			 : "0"((long)SYS_ioctl), "D"((long)fd), "S"((long)DRM_IOCTL_MODE_DIRTYFB),
			   "d"(dirty), [before] "r"(before), [after] "r"(after)
			 : "rcx", "r11", "cc", "memory", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4",
			   "ymm5", "ymm6", "ymm7");
	if (result < 0 || memcmp(before, after, sizeof(before))) {
		fprintf(stderr, "check_avx_state: register preservation failed\n");
		exit(1);
	}
}
#endif
int main(int argc, char **argv)
{
	setbuf(stdout, NULL);
	if (argc == 2 && !strcmp(argv[1], "--baseline")) {
		puts("WORKLOAD_BEGIN NO_DRIVER idle");
		struct cpu_sample before = sample();
		double start = seconds(CLOCK_MONOTONIC), cpu = seconds(CLOCK_PROCESS_CPUTIME_ID);
		sleep(10);
		account("NO_DRIVER", "idle", before, start, cpu);
		return 0;
	}
	int quick = 0, full_only = 0, static_only = 0, check_state = 0;
	const char *selected = NULL;
	for (int arg = 1; arg < argc; arg++) {
		if (!strcmp(argv[arg], "--check-fpu"))
			check_state = 1;
		else if (!strcmp(argv[arg], "--quick"))
			quick = 1;
		else if (!strcmp(argv[arg], "--full-only")) {
			quick = 1;
			full_only = 1;
		} else if (!strcmp(argv[arg], "--static-only")) {
			quick = 1;
			static_only = 1;
		} else if (!strncmp(argv[arg], "--format=", 9))
			selected = argv[arg] + 9;
		else if (!strncmp(argv[arg], "--duration=", 11)) {
			char *end;
			unsigned long value = strtoul(argv[arg] + 11, &end, 10);
			if (*end || value < 1 || value > 120)
				return 2;
			duration = value;
		} else if (strcmp(argv[arg], "--hold")) {
			fprintf(stderr, "unknown argument: %s\n", argv[arg]);
			return 2;
		}
	}
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	check(fd, "open DRM");
	if (argc == 2 && !strcmp(argv[1], "--hold")) {
		puts("DRM_HANDLE_OPEN");
		while (access("/tmp/release-hold", F_OK))
			usleep(100000);
		struct drm_mode_card_res after = { 0 };
		errno = 0;
		int result = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &after), error = errno;
		printf("DISCONNECT_IOCTL result=%d errno=%d\n", result, error);
		close(fd);
		return result < 0 && error == ENODEV ? 0 : 1;
	}
	check(ioctl(fd, DRM_IOCTL_SET_MASTER, 0), "DRM master");
	struct drm_mode_card_res resources = { 0 };
	check(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources), "resources count");
	uint32_t crtcs[16] = { 0 }, connectors[16] = { 0 };
	if (resources.count_crtcs > 16 || resources.count_connectors > 16 ||
	    !resources.count_crtcs || !resources.count_connectors)
		return 2;
	resources.crtc_id_ptr = (uintptr_t)crtcs;
	resources.connector_id_ptr = (uintptr_t)connectors;
	resources.count_fbs = resources.count_encoders = 0;
	check(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources), "resources");
	struct drm_mode_get_connector connector = { .connector_id = connectors[0] };
	check(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector), "connector count");
	if (connector.connection != 1 || connector.count_modes > 128 || !connector.count_modes)
		return 3;
	struct drm_mode_modeinfo modes[128];
	connector.modes_ptr = (uintptr_t)modes;
	connector.count_props = connector.count_encoders = 0;
	check(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector), "connector modes");
	struct drm_mode_modeinfo *mode = NULL;
	for (unsigned int i = 0; i < connector.count_modes; i++)
		if (modes[i].hdisplay == 800 && modes[i].vdisplay == 600 &&
		    modes[i].vrefresh == 60) {
			mode = &modes[i];
			break;
		}
	if (!mode)
		mode = &modes[0];
	struct drm_mode_crtc saved = { .crtc_id = crtcs[0] };
	check(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &saved), "save CRTC");
	const uint32_t formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_RGB888, DRM_FORMAT_RGB565 };
	const unsigned int cpp[] = { 4, 3, 2 };
	const char *names[] = { "XRGB8888", "RGB888", "RGB565" };
	int filter = open("/sys/kernel/debug/tracing/set_ftrace_filter", O_WRONLY | O_TRUNC);
	if (filter >= 0) {
		const char *name =
			"fl2000_* it66121_* usb_submit_urb xhci_irq xhci_urb_enqueue usb_hcd_map_urb_for_dma usb_hcd_unmap_urb_for_dma __usb_hcd_giveback_urb\n";
		size_t left = strlen(name);
		while (left) {
			ssize_t done = write(filter, name, left);
			if (done <= 0) {
				perror("filter");
				exit(1);
			}
			name += done;
			left -= done;
		}
		close(filter);
	}
	if (selected && strcmp(selected, "XRGB8888") && strcmp(selected, "RGB888") &&
	    strcmp(selected, "RGB565"))
		return 2;
	for (unsigned int n = 0; n < 3; n++) {
		if (selected && strcmp(selected, names[n]))
			continue;
		struct drm_mode_create_dumb dumb = { .width = mode->hdisplay,
						     .height = mode->vdisplay,
						     .bpp = cpp[n] * 8 };
		check(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb), "create dumb");
		struct drm_mode_map_dumb mapping = { .handle = dumb.handle };
		check(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapping), "map dumb");
		unsigned char *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
					     fd, mapping.offset);
		if (pixels == MAP_FAILED) {
			perror("mmap");
			return 5;
		}
		for (unsigned int y = 0; y < dumb.height; y++)
			for (unsigned int x = 0; x < dumb.width; x++) {
				unsigned int bar = x * 8 / dumb.width;
				unsigned int r = (bar & 1) ? 255 : 0, g = (bar & 2) ? 255 : 0,
					     b = (bar & 4) ? 255 : 0;
				unsigned char *p = pixels + (size_t)y * dumb.pitch + x * cpp[n];
				if (cpp[n] == 2) {
					uint16_t value = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
					memcpy(p, &value, 2);
				} else {
					p[0] = b;
					p[1] = g;
					p[2] = r;
					if (cpp[n] == 4)
						p[3] = 0;
				}
			}
		struct drm_mode_fb_cmd2 fb = { .width = dumb.width,
					       .height = dumb.height,
					       .pixel_format = formats[n],
					       .handles = { dumb.handle },
					       .pitches = { dumb.pitch } };
		check(ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb), "add framebuffer");
		struct drm_mode_crtc active = { .crtc_id = crtcs[0],
						.fb_id = fb.fb_id,
						.set_connectors_ptr = (uintptr_t)connectors,
						.count_connectors = 1,
						.mode_valid = 1,
						.mode = *mode };
		check(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &active), "set mode");
		printf("KMS_ACTIVE %s %ux%u pitch=%u fb=%u\n", names[n], dumb.width, dumb.height,
		       dumb.pitch, fb.fb_id);
		sleep(2);
		profile(0);
		struct drm_clip_rect clip = { .x2 = dumb.width, .y2 = dumb.height };
		struct drm_mode_fb_dirty_cmd dirty = { .fb_id = fb.fb_id,
						       .num_clips = 1,
						       .clips_ptr = (uintptr_t)&clip };
		if (check_state) {
#ifdef __x86_64__
			for (unsigned int trial = 0; trial < 100; trial++) {
				check_sse_state(fd, &dirty);
				if (__builtin_cpu_supports("avx"))
					check_avx_state(fd, &dirty);
			}
			printf("FPU_STATE_PASS %s SSE%s\n", names[n],
			       __builtin_cpu_supports("avx") ? "+AVX" : "");
#else
			puts("FPU_STATE_SKIP unsupported test architecture");
#endif
		}
		if (!quick || static_only)
			workload(fd, pixels, &dirty, names[n], "static", 0);
		if (!static_only)
			workload(fd, pixels, &dirty, names[n], "full_60hz", duration * 60);
		clip.x2 = 64;
		clip.y2 = 64;
		if (!full_only && !static_only)
			workload(fd, pixels, &dirty, names[n], "small_60hz", duration * 60);
		clip.x2 = dumb.width;
		clip.y2 = dumb.height;
		if (!quick) {
			profile(1);
			workload(fd, pixels, &dirty, names[n], "profile_full_60hz", duration * 60);
			profile(0);
			report_profile(names[n]);
		}
		saved.set_connectors_ptr = (uintptr_t)connectors;
		saved.count_connectors = 1;
		check(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &saved), "restore CRTC");
		check(ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id), "remove framebuffer");
		munmap(pixels, dumb.size);
		struct drm_mode_destroy_dumb destroy = { .handle = dumb.handle };
		check(ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy), "destroy dumb");
	}
	puts("KMS_NATIVE_FORMAT_PASS");
	close(fd);
	return 0;
}
