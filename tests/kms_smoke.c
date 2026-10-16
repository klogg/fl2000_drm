// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
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
static void report_profile(const char *format)
{
	char buffer[4096];
	for (int cpu = 0; cpu < 2; cpu++) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/kernel/debug/tracing/trace_stat/function%d",
			 cpu);
		FILE *file = fopen(path, "r");
		if (!file)
			continue;
		while (fgets(buffer, sizeof(buffer), file))
			if (strstr(buffer, "fl2000_streaming_update"))
				printf("PROFILE %s CPU%d %s", format, cpu, buffer);
		fclose(file);
	}
}
/* Test atomic validation and completion through the real DRM ioctl path. */
static uint32_t property_id(int fd, uint32_t object, uint32_t type, const char *name)
{
	uint32_t ids[64];
	uint64_t values[64];
	struct drm_mode_obj_get_properties props = {
		.obj_id = object,
		.obj_type = type,
	};

	check(ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props), "property count");
	if (props.count_props > 64)
		exit(1);
	props.props_ptr = (uintptr_t)ids;
	props.prop_values_ptr = (uintptr_t)values;
	check(ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props), "properties");
	for (unsigned int i = 0; i < props.count_props; i++) {
		struct drm_mode_get_property prop = { .prop_id = ids[i] };

		check(ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop), "property name");
		if (!strcmp(prop.name, name))
			return ids[i];
	}
	fprintf(stderr, "Missing property: %s\n", name);
	exit(1);
}

static int atomic_property(int fd, uint32_t object, uint32_t property, uint64_t value,
			   uint32_t flags, uint64_t cookie)
{
	uint32_t count = 1;
	struct drm_mode_atomic request = {
		.flags = flags,
		.count_objs = 1,
		.objs_ptr = (uintptr_t)&object,
		.count_props_ptr = (uintptr_t)&count,
		.props_ptr = (uintptr_t)&property,
		.prop_values_ptr = (uintptr_t)&value,
		.user_data = cookie,
	};

	return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &request);
}

static void atomic_checks(int fd, uint32_t crtc, uint32_t fb, unsigned int width,
			  unsigned int height)
{
	uint32_t planes[16];
	struct drm_mode_get_plane_res resources = { 0 };

	check(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources), "plane count");
	if (resources.count_planes != 1)
		exit(1);
	resources.plane_id_ptr = (uintptr_t)planes;
	check(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources), "planes");
	uint32_t plane = planes[0];
	uint32_t fb_property = property_id(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
	uint32_t active = property_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
	check(atomic_property(fd, plane, fb_property, fb, DRM_MODE_ATOMIC_TEST_ONLY, 0),
	      "atomic framebuffer validation");
	/* These requests must fail without changing the active display. */
	const char *names[] = { "SRC_X", "SRC_W", "CRTC_X", "CRTC_W", "CRTC_H" };
	const uint64_t invalid[] = { 1, (uint64_t)(width - 1) << 16, 1, width - 1, height - 1 };
	const int errors[] = { ENOSPC, ERANGE, EINVAL, ERANGE, ERANGE };
	for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t prop = property_id(fd, plane, DRM_MODE_OBJECT_PLANE, names[i]);

		errno = 0;
		if (atomic_property(fd, plane, prop, invalid[i], DRM_MODE_ATOMIC_TEST_ONLY, 0) !=
			    -1 ||
		    errno != errors[i]) {
			fprintf(stderr, "Invalid %s: expected errno %d, got %d\n", names[i],
				errors[i], errno);
			exit(1);
		}
	}
	/* Both plane flips and CRTC-only commits need software completion events. */
	for (uint64_t cookie = 1; cookie <= 8; cookie++) {
		bool crtc_only = cookie & 1;
		check(atomic_property(fd, crtc_only ? crtc : plane,
				      crtc_only ? active : fb_property, crtc_only ? 1 : fb,
				      DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, cookie),
		      "atomic page flip");
		struct pollfd ready = { .fd = fd, .events = POLLIN };
		if (poll(&ready, 1, 3000) != 1 || !(ready.revents & POLLIN)) {
			fprintf(stderr, "Atomic page-flip event timed out\n");
			exit(1);
		}
		struct drm_event_vblank event;
		ssize_t size = read(fd, &event, sizeof(event));
		if (size != sizeof(event) || event.base.type != DRM_EVENT_FLIP_COMPLETE ||
		    event.base.length != sizeof(event) || event.user_data != cookie) {
			fprintf(stderr, "Invalid atomic page-flip event\n");
			exit(1);
		}
	}
	puts("KMS_ATOMIC_PASS: geometry validation, plane flips, CRTC-only commits, events");
}

int main(int argc, char **argv)
{
	setbuf(stdout, NULL);
	const char *device = getenv("DRM_DEVICE");
	int fd = open(device ? device : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
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
	struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
	check(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap), "atomic client capability");
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
		const char *name = "fl2000_streaming_update\n";
		if (write(filter, name, strlen(name)) != (ssize_t)strlen(name))
			perror("filter");
		close(filter);
	}
	for (unsigned int n = 0; n < 3; n++) {
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
		atomic_checks(fd, crtcs[0], fb.fb_id, dumb.width, dumb.height);
		sleep(1);
		profile(1);
		struct drm_clip_rect clip = { .x2 = dumb.width, .y2 = dumb.height };
		struct drm_mode_fb_dirty_cmd dirty = { .fb_id = fb.fb_id,
						       .num_clips = 1,
						       .clips_ptr = (uintptr_t)&clip };
		double start = seconds(CLOCK_MONOTONIC), cpu = seconds(CLOCK_PROCESS_CPUTIME_ID);
		for (int frame = 0; frame < 120; frame++) {
			pixels[0] = frame;
			check(ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty), "dirty framebuffer");
			struct timespec until = { (time_t)start, 0 };
			double deadline = start + (frame + 1) / 60.0;
			until.tv_sec = (time_t)deadline;
			until.tv_nsec = (long)((deadline - until.tv_sec) * 1e9);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
		}
		printf("KMS_UPDATES %s 120 updates wall=%.3fs process_cpu=%.3fs\n", names[n],
		       seconds(CLOCK_MONOTONIC) - start, seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu);
		sleep(1);
		profile(0);
		report_profile(names[n]);
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
