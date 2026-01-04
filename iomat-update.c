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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <linux/iomatrix_ioctl.h>

/* Fixed device node */
static const char *DEFAULT_DEVNODE = "/dev/iomatrix-uapi";

/* Fixed addresses for rts5911 */
#define RTS5911_AUTO_W_ADDR (0x60000000u)
#define RTS5911_ERASE_ADDR  (0x00000000u)

#define CHUNK_SIZE     (4 * 1024) /* 4KB erase and write chunk */
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

typedef enum {
	MODEL_RTS5911 = 0,
	MODEL_RTS5913 = 1, /* reserved for future use */
} rts_model_t;

static void print_usage(const char *prog)
{
	fprintf(stdout,
		"Usage: %s [options] <image.bin>\n"
		"\n"
		"Options:\n"
		"  -h, --help           Show this help and exit\n"
		"  -m, --model <name>   Select model: rts5911 (default) or rts5913 (not supported yet)\n"
		"\n"
		"Example:\n"
		"  %s -m rts5911 firmware.bin\n"
		"  %s -m rts5913 firmware.bin  (will report: not supported yet)\n"
		"\n",
		prog, prog, prog);
}

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	ssize_t r;
	int ret = -1;

	if (fd < 0) {
		perror("open");
		return ret;
	}

	if (fstat(fd, &st) < 0) {
		perror("fstat");
		goto out;
	}

	*len = (size_t)st.st_size;
	if (*len == 0) {
		fprintf(stderr, "Error: file size is 0\n");
		goto out;
	}

	*buf = (uint8_t *)malloc(*len);
	if (!*buf) {
		perror("malloc");
		goto out;
	}

	r = read(fd, *buf, *len);
	if (r < 0 || (size_t)r != *len) {
		perror("read");
		free(*buf);
		*buf = NULL;
		goto out;
	}

	ret = 0;

out:
	if (close(fd) < 0) {
		perror("close");
	}
	return ret;
}

static int erase_fspi(int fd, uint32_t base_reg, size_t img_len)
{
	/* Erase length must be multiple of 4KB */
	size_t erase_len = ALIGN_UP(img_len, CHUNK_SIZE);
	int ret;

	struct iomatrix_uapi_erase_req ereq = {
		.addr = base_reg,
		.len = (uint32_t)erase_len,
		.type = IOMATRIX_ERASE_4K,
	};

	printf("FSPI erase start: %zu bytes at 0x%08x type %d\n", erase_len,
	       base_reg, ereq.type);

	ret = ioctl(fd, IOMATRIX_IOC_ERASE_FSPI, &ereq);
	if (ret < 0) {
		perror("ioctl(IOMATRIX_IOC_ERASE)");
		return -1;
	}

	printf("FSPI erase ok: %zu bytes at 0x%08x type %d\n", erase_len,
	       base_reg, ereq.type);

	return 0;
}

static int write_mems_4k(int fd, uint32_t base_reg, const uint8_t *buf,
			 size_t len)
{
	size_t offset = 0;
	int ret;

	printf("MEMS write start: %zu bytes to 0x%08x\n", len, base_reg);

	while (offset < len) {
		size_t chunk = len - offset;
		if (chunk > CHUNK_SIZE)
			chunk = CHUNK_SIZE;

		struct iomatrix_uapi_write_req wreq = {
			.base_reg = base_reg + (uint32_t)offset,
			.size = (uint32_t)chunk,
			.flags = 1,
			.uptr = (uint32_t)(uintptr_t)(buf + offset),
		};

		ret = ioctl(fd, IOMATRIX_IOC_WRITE_MEMS, &wreq);
		if (ret < 0) {
			fprintf(stderr,
				"ioctl(IOMATRIX_IOC_WRITE) failed at offset=0x%zx, size=%zu: %s\n",
				offset, chunk, strerror(errno));
			return -1;
		}

		offset += chunk;
	}

	printf("MEMS write OK: %zu bytes to 0x%08x\n", len, base_reg);
	return 0;
}

/* Common update flow used by rts5911 */
static int perform_update(const char *imgpath, uint32_t erase_addr,
			  uint32_t write_addr)
{
	uint8_t *buf = NULL;
	size_t len = 0;
	int fd = -1;
	int ret = -1;

	if (read_file(imgpath, &buf, &len) < 0) {
		fprintf(stderr, "Failed to read image: %s\n", imgpath);
		goto out;
	}

	fd = open(DEFAULT_DEVNODE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open devnode");
		goto out;
	}

	/* 1) Erase FSPI in 4KB blocks */
	if (erase_fspi(fd, erase_addr, len) < 0) {
		goto out;
	}

	/* Fixed 0.5s delay between erase and write */
	usleep(500000);

	/* 2) Write MEMS in 4KB chunks */
	if (write_mems_4k(fd, write_addr, buf, len) < 0) {
		goto out;
	}

	ret = 0;

out:
	if (fd >= 0)
		close(fd);
	if (buf)
		free(buf);
	return ret;
}

static int rts5911_update(const char *imgpath)
{
	return perform_update(imgpath, RTS5911_ERASE_ADDR, RTS5911_AUTO_W_ADDR);
}

static int parse_model(const char *s, rts_model_t *out)
{
	if (strcasecmp(s, "rts5911") == 0) {
		*out = MODEL_RTS5911;
		return 0;
	}
	if (strcasecmp(s, "rts5913") == 0) {
		*out = MODEL_RTS5913;
		return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *imgpath = NULL;
	rts_model_t model = MODEL_RTS5911;

	static const struct option long_opts[] = {
		{ "help", no_argument, 0, 'h' },
		{ "model", required_argument, 0, 'm' },
		{ 0, 0, 0, 0 }
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "hm:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			print_usage(argv[0]);
			return 0;
		case 'm':
			if (parse_model(optarg, &model) < 0) {
				fprintf(stderr,
					"Error: invalid model '%s', use rts5911 or rts5913\n",
					optarg);
				return 1;
			}
			break;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Error: missing <image.bin>\n");
		print_usage(argv[0]);
		return 1;
	}
	imgpath = argv[optind];

	int ret;
	switch (model) {
	case MODEL_RTS5911:
		ret = rts5911_update(imgpath);
		break;
	case MODEL_RTS5913:
		fprintf(stderr,
			"Error: rts5913 update is not supported yet. Framework reserved.\n");
		ret = 1;
		break;
	default:
		fprintf(stderr, "Error: unknown model\n");
		ret = 1;
		break;
	}

	return ret == 0 ? 0 : 1;
}
