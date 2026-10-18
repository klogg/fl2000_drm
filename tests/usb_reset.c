// SPDX-License-Identifier: GPL-2.0
/* Request a USB reset only inside the prepared test guest. */
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (access("/fl2000-test-guest", F_OK) || argc != 2) {
		fprintf(stderr, "Usage in prepared guest: %s /dev/bus/usb/BUS/DEVICE\n", argv[0]);
		return 2;
	}
	int fd = open(argv[1], O_WRONLY | O_CLOEXEC);

	if (fd < 0) {
		perror("open USB device");
		return 1;
	}
	int ret = ioctl(fd, USBDEVFS_RESET, 0);

	if (ret < 0)
		perror("USBDEVFS_RESET");
	close(fd);
	return ret < 0;
}
