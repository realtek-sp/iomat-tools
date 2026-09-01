/*
 * SPDX-License-Identifier: LicenseRef-Realtek-Proprietary
 *
 * Copyright (c) 2025, Realtek Semiconductor Corp. All rights reserved.
 *
 * This software is a confidential and proprietary property of Realtek
 * Semiconductor Corp. Disclosure, reproduction, redistribution, in
 * whole or in part, of this work and its derivatives without express
 * permission is prohibited.
 *
 * Realtek Semiconductor Corp. reserves the right to update, modify, or
 * discontinue this software at any time without notice. This software is
 * provided "as is" and any express or implied warranties, including, but
 * not limited to, the implied warranties of merchantability and fitness for
 * a particular purpose are disclaimed. In no event shall Realtek
 * Semiconductor Corp. be liable for any direct, indirect, incidental,
 * special, exemplary, or consequential damages (including, but not limited
 * to, procurement of substitute goods or services; loss of use, data, or
 * profits; or business interruption) however caused and on any theory of
 * liability, whether in contract, strict liability, or tort (including
 * negligence or otherwise) arising in any way out of the use of this software,
 * even if advised of the possibility of such damage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>

#define FW_CLASS_PATH    "/sys/class/firmware"
#define FW_NAME_PREFIX   "rts591x-ec"
#define DEFAULT_IMAGE_PATH "/usr/share/iomat-tools/rts5979.bin"
#define POLL_INTERVAL_US 100000
#define UPDATE_TIMEOUT_SEC 1200

static volatile sig_atomic_t cancel_requested;
static char fw_path[PATH_MAX];

static void print_usage(const char *prog)
{
	fprintf(stdout,
		"Usage: %s [options] [image.bin]\n"
		"\n"
		"Options:\n"
		"  -h, --help           Show this help and exit\n"
		"  -d, --device <name>  firmware_upload name (for example rts591x-ec0)\n"
		"\n"
		"If no image is specified, %s is used.\n"
		"If -d is omitted, exactly one %s* device must exist under %s.\n",
		prog, DEFAULT_IMAGE_PATH, FW_NAME_PREFIX, FW_CLASS_PATH);
}

static void signal_handler(int signo)
{
	(void)signo;
	cancel_requested = 1;
}

static int make_attr_path(char *path, size_t size, const char *attr)
{
	int len = snprintf(path, size, "%s/%s", fw_path, attr);

	if (len < 0 || (size_t)len >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int write_all(int fd, const void *buf, size_t size)
{
	const char *data = buf;

	while (size) {
		ssize_t written = write(fd, data, size);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		data += written;
		size -= (size_t)written;
	}
	return 0;
}

static int write_attr(const char *attr, const char *value)
{
	char path[PATH_MAX];
	int fd;
	int ret;

	if (make_attr_path(path, sizeof(path), attr))
		return -1;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = write_all(fd, value, strlen(value));
	if (close(fd) && !ret)
		ret = -1;
	return ret;
}

static int read_attr(const char *attr, char *buf, size_t size)
{
	char path[PATH_MAX];
	ssize_t len;
	int fd;

	if (size < 2) {
		errno = EINVAL;
		return -1;
	}
	if (make_attr_path(path, sizeof(path), attr))
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		len = read(fd, buf, size - 1);
	} while (len < 0 && errno == EINTR);
	if (close(fd) && len >= 0) {
		errno = EIO;
		return -1;
	}
	if (len < 0)
		return -1;

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		len--;
	buf[len] = '\0';
	return 0;
}

static int validate_fw_device(void)
{
	static const char *const attrs[] = {
		"loading", "data", "status", "error", "remaining_size", "cancel",
	};
	char path[PATH_MAX];
	size_t i;

	for (i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
		if (make_attr_path(path, sizeof(path), attrs[i]))
			return -1;
		if (access(path, F_OK))
			return -1;
	}
	return 0;
}

static int select_fw_device(const char *name)
{
	struct dirent *entry;
	DIR *dir;
	unsigned int count = 0;
	int len;

	if (name) {
		if (strchr(name, '/')) {
			fprintf(stderr, "Error: -d expects a firmware_upload name, not a path\n");
			return -1;
		}
		len = snprintf(fw_path, sizeof(fw_path), "%s/%s", FW_CLASS_PATH, name);
		if (len < 0 || (size_t)len >= sizeof(fw_path)) {
			errno = ENAMETOOLONG;
			perror("firmware_upload device");
			return -1;
		}
		if (validate_fw_device()) {
			perror("firmware_upload device");
			return -1;
		}
		return 0;
	}

	dir = opendir(FW_CLASS_PATH);
	if (!dir) {
		perror(FW_CLASS_PATH);
		return -1;
	}
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, FW_NAME_PREFIX,
			    strlen(FW_NAME_PREFIX)))
			continue;
		len = snprintf(fw_path, sizeof(fw_path), "%s/%s", FW_CLASS_PATH,
			       entry->d_name);
		if (len < 0 || (size_t)len >= sizeof(fw_path)) {
			closedir(dir);
			errno = ENAMETOOLONG;
			perror("firmware_upload device");
			return -1;
		}
		count++;
	}
	closedir(dir);

	if (count != 1) {
		fprintf(stderr, "Error: found %u %s* devices; use -d to select one\n",
			count, FW_NAME_PREFIX);
		return -1;
	}
	if (validate_fw_device()) {
		perror("firmware_upload device");
		return -1;
	}
	return 0;
}

static int copy_image(const char *image)
{
	char path[PATH_MAX];
	char buf[65536];
	ssize_t len;
	int in;
	int out;
	int ret = -1;

	in = open(image, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	if (make_attr_path(path, sizeof(path), "data"))
		goto out_in;
	out = open(path, O_WRONLY | O_CLOEXEC);
	if (out < 0)
		goto out_in;

	while ((len = read(in, buf, sizeof(buf))) != 0) {
		if (len < 0) {
			if (errno == EINTR)
				continue;
			goto out_both;
		}
		if (write_all(out, buf, (size_t)len))
			goto out_both;
	}
	ret = 0;

out_both:
	if (close(out) && !ret)
		ret = -1;
out_in:
	if (close(in) && !ret)
		ret = -1;
	return ret;
}

static int wait_for_idle(void)
{
	char previous[32] = "";
	char status[32];
	char remaining[32];
	char error[128];
	unsigned int elapsed = 0;
	bool cancel_sent = false;

	for (;;) {
		if (cancel_requested && !cancel_sent) {
			if (write_attr("cancel", "1"))
				perror("cancel");
			else
				fprintf(stderr, "Cancellation requested\n");
			cancel_sent = true;
		}
		if (read_attr("status", status, sizeof(status)) ||
		    read_attr("remaining_size", remaining, sizeof(remaining)))
			return -1;
		if (strcmp(previous, status)) {
			printf("status=%s remaining=%s\n", status, remaining);
			strncpy(previous, status, sizeof(previous) - 1);
			previous[sizeof(previous) - 1] = '\0';
		}
		if (!strcmp(status, "idle"))
			break;
		if (elapsed >= UPDATE_TIMEOUT_SEC * 10U) {
			fprintf(stderr, "Error: update timed out after %u seconds\n",
				UPDATE_TIMEOUT_SEC);
			if (!cancel_sent)
				write_attr("cancel", "1");
			errno = ETIMEDOUT;
			return -1;
		}
		usleep(POLL_INTERVAL_US);
		elapsed++;
	}

	if (read_attr("error", error, sizeof(error)))
		return -1;
	if (cancel_sent) {
		fprintf(stderr, "Error: update was canceled\n");
		errno = ECANCELED;
		return -1;
	}
	if (*error) {
		fprintf(stderr, "Error: firmware update failed: %s\n", error);
		errno = EIO;
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ 0, 0, 0, 0 }
	};
	const char *device = NULL;
	const char *image;
	struct sigaction sa = { 0 };
	struct stat st;
	char status[32];
	int opt;

	while ((opt = getopt_long(argc, argv, "d:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}
	if (optind + 1 < argc) {
		print_usage(argv[0]);
		return 1;
	}
	image = optind < argc ? argv[optind] : DEFAULT_IMAGE_PATH;

	if (geteuid()) {
		fprintf(stderr, "Error: this tool must run as root\n");
		return 1;
	}
	if (stat(image, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		perror("image");
		return 1;
	}
	if (select_fw_device(device))
		return 1;
	if (read_attr("status", status, sizeof(status))) {
		perror("status");
		return 1;
	}
	if (strcmp(status, "idle")) {
		fprintf(stderr, "Error: firmware_upload device is busy: %s\n", status);
		return 1;
	}

	printf("Firmware device: %s\n", fw_path);
	printf("Image: %s\n", image);
	printf("Image size: %lld bytes\n", (long long)st.st_size);
	printf("WARNING: this updates the active EC boot flash.\n");
	printf("Power loss, cancellation, or an invalid image may make the EC unbootable.\n");

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (write_attr("loading", "1")) {
		perror("loading=1");
		return 1;
	}
	if (copy_image(image)) {
		int saved_errno = errno;

		write_attr("loading", "-1");
		errno = saved_errno;
		perror("firmware data");
		return 1;
	}
	if (cancel_requested) {
		write_attr("loading", "-1");
		fprintf(stderr, "Aborted\n");
		return 1;
	}
	if (write_attr("loading", "0")) {
		perror("loading=0");
		return 1;
	}
	if (wait_for_idle())
		return 1;

	printf("SUCCESS: EC firmware update via firmware_upload passed\n");
	printf("Reset or power-cycle the EC to activate the image.\n");
	return 0;
}
