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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/iomatrix_ioctl.h>

/* Modify default DEVNODE */
#define DEFAULT_DEVNODE "/dev/iomatrix-uapi0"
#define ALIGN32		(4)

static void usage(const char *prog)
{
	fprintf(stdout,
		"Usage:\n"
		"  Read:  %s [-d device] <addr>\n"
		"  Write: %s [-d device] <addr> <value>\n"
		"\n"
		"Options:\n"
		"  -d <device>  Specify device node (default: %s)\n"
		"  -h           Show this help message\n"
		"\n"
		"Notes:\n"
		"  - <addr> and <value> must be hexadecimal with 0x/0X prefix\n"
		"  - 32-bit access only; <addr> must be 4-byte aligned\n"
		"\n",
		prog, prog, DEFAULT_DEVNODE);
}

/* Parse a hex string with 0x/0X prefix into a 32-bit value */
static int parse_hex_u32(const char *s, uint32_t *out)
{
	const char *p;
	unsigned long long v;

	if (!s || strlen(s) < 3 ||
	    !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
		return -EINVAL;

	p = s + 2;
	if (*p == '\0')
		return -EINVAL;

	/* Validate hex digits only */
	for (const char *q = p; *q; q++) {
		if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') ||
		      (*q >= 'A' && *q <= 'F')))
			return -EINVAL;
	}

	errno = 0;
	v = strtoull(p, NULL, 16);
	if (errno)
		return -errno;
	if (v > 0xFFFFFFFFULL)
		return -ERANGE;

	*out = (uint32_t)v;
	return 0;
}

/* Perform 32-bit read via ioctl */
static int do_read(int fd, uint32_t addr, uint32_t *out_val)
{
	struct iomatrix_uapi_rw_req req;

	if (addr & (ALIGN32 - 1)) {
		fprintf(stderr, "Error: address 0x%08x is not 4-byte aligned\n",
			addr);
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	req.addr = addr;

	if (ioctl(fd, IOMATRIX_IOC_READ_MEM, &req) < 0)
		return -errno;

	*out_val = req.val;
	return 0;
}

/* Perform 32-bit write via ioctl */
static int do_write(int fd, uint32_t addr, uint32_t val)
{
	struct iomatrix_uapi_rw_req req;

	if (addr & (ALIGN32 - 1)) {
		fprintf(stderr, "Error: address 0x%08x is not 4-byte aligned\n",
			addr);
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	req.addr = addr;
	req.val = val;

	if (ioctl(fd, IOMATRIX_IOC_WRITE_MEM, &req) < 0)
		return -errno;

	return 0;
}

int main(int argc, char **argv)
{
	int fd, ret;
	uint32_t addr, val, rval;
	const char *devnode = DEFAULT_DEVNODE;
	int opt;
	int args_left;

	/* Support --help explicitly before getopt processing */
	if (argc >= 2 && !strcmp(argv[1], "--help")) {
		usage(argv[0]);
		return 0;
	}

	/* Parse options: -d for device, -h for help */
	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		switch (opt) {
		case 'd':
			devnode = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/* Calculate remaining positional arguments */
	args_left = argc - optind;

	/* Expect either: read (1 arg) or write (2 args) */
	if (args_left != 1 && args_left != 2) {
		usage(argv[0]);
		return 1;
	}

	/* Parse Address (first positional arg) */
	ret = parse_hex_u32(argv[optind], &addr);
	if (ret) {
		fprintf(stderr,
			"Error: invalid addr '%s' (must be hex with 0x/0X): %s\n",
			argv[optind], strerror(-ret));
		return 1;
	}

	fd = open(devnode, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Error: open(%s) failed: %s\n", devnode,
			strerror(errno));
		return 1;
	}

	if (args_left == 1) {
		/* Read */
		ret = do_read(fd, addr, &rval);
		if (ret) {
			fprintf(stderr, "Error: read addr 0x%08x failed: %s\n",
				addr, strerror(-ret));
			goto out_close_err;
		}

		/* Print hex value (devmem-like) */
		printf("0x%08x\n", rval);
	} else {
		/* Write (second positional arg is value) */
		ret = parse_hex_u32(argv[optind + 1], &val);
		if (ret) {
			fprintf(stderr,
				"Error: invalid value '%s' (must be hex with 0x/0X): %s\n",
				argv[optind + 1], strerror(-ret));
			goto out_close_err;
		}

		ret = do_write(fd, addr, val);
		if (ret) {
			fprintf(stderr, "Error: write addr 0x%08x failed: %s\n",
				addr, strerror(-ret));
			goto out_close_err;
		}

		/* Silent on success to mimic devmem minimal output */
	}

	close(fd);
	return 0;

out_close_err:
	close(fd);
	return 1;
}
