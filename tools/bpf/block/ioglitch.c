// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Copyright (C) 2025 KylinSoft Co., Ltd. All rights reserved.
//

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <argp.h>
#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <time.h>
#include <signal.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bits/types.h>

#include "ioglitch.skel.h"
#include "ioglitch.h"

#define TIMEOUT_MS		1000
#define DEFAULT_THRESHOLD_A2C	"100000"
#define VERSION		"V2025.0820.0001"

#define min(x, y)                             \
	({                                     \
		typeof(x) _min1 = (x);         \
		typeof(y) _min2 = (y);         \
		(void)(&_min1 == &_min2);      \
		_min1 < _min2 ? _min1 : _min2; \
	})

static volatile sig_atomic_t exiting;

static struct env {
	bool verbose;
	bool isdelete;
	int interval;
	char *disk;
	char *threshold;
} env = {
	.interval = 1,
	.threshold = DEFAULT_THRESHOLD_A2C,
	.verbose = false,
};

const char *argp_program_doc =
	"Summarize block device I/O glitch detection as a histogram.\n"
	"\n"
	"USAGE: ioglitch [--help] [-d DISK]\n"
	"\n"
	"Stage Correspondence in the I/O Stack:\n"
	"A2T   -> Delay from BIO alloc to IO throttling completion.\n"
	"T2R   -> Delay from IO throttling completion to RQS processing completion.\n"
	"R2G   -> Delay from RQS completion to request alloc completion.\n"
	"G2I   -> Delay from request alloc completion to insertion scheduling, during which PLUG/UNPLUG operations occur.\n"
	"I2D   -> Delay from request insertion scheduling to dispatch initiation.\n"
	"G2D   -> Latency from request reception to dispatch.\n"
	"D2C   -> Delay from dispatch initiation to request completion.\n"
	"A2C   -> The latency from BIO allocation to request completion.\n"
	"RQS:  Request Queue Structures, including the wbt and blk_iolatency features.\n"
	"\n"
	"EXAMPLES:\n"
	"    ioglitch -d /dev/sda		# Trace sda only\n"
	"    ioglitch -d /dev/sda -t 100000	# Only trace sda and set the A2C detection threshold to 100ms\n"
	"    ioglitch -r -d /dev/sda		# Delete old data every time\n";

static const struct argp_option opts[] = {
	{ "verbose", 'v', NULL, 0, "Verbose debug output", 0 },
	{ "isdelete", 'r', NULL, 0, "The histogram will delete the displayed data", 0 },
	{ "disk", 'd', "DISK", 0, "Trace this disk only", 0 },
	{ "threshold", 't', "THRESHOLD", 0, "Set A2C glitch detection threshold", 0},
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help", 0 },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'v':
		env.verbose = true;
		break;
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'r':
		env.isdelete = true;
		break;
	case 'd':
		env.disk = arg;
		if (strlen(arg) + 1 > DISK_PATH_LEN) {
			perror("Invalid disk name: too long\n");
			argp_usage(state);
		}
		break;
	case 't':
		env.threshold = arg;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static void print_latency_info(struct latency_info *data)
{
	int i;
	char *stage;

	for (i = A2T; i < LATENCY_NR; i++) {
		if (!strcmp(latency_name[i], latency_name[A2C]))
			stage = "max_latency";
		else
			stage = "latency";

		if (data[i].max_latency)
			printf("%s: %s: %uus, rw: %s, sector: %u, nr_sector: %u\n",
				latency_name[i], stage, data[i].max_latency,
				data[i].rwbs, data[i].sector, data[i].nr_sector);
	}
}

static void print_stars(unsigned int val, unsigned int val_max, int width)
{
	int num_stars, num_spaces, i;
	bool need_plus;

	num_stars = min(val, val_max) * width / val_max;
	num_spaces = width - num_stars;
	need_plus = val > val_max;

	for (i = 0; i < num_stars; i++)
		printf("*");
	for (i = 0; i < num_spaces; i++)
		printf(" ");
	if (need_plus)
		printf("+");
}

void print_log2_hist(unsigned int *vals, int vals_size, const char *val_type)
{
	int stars_max = 40, idx_max = -1;
	unsigned int val, val_max = 0;
	unsigned long long low, high;
	int stars, width, i;

	for (i = 0; i < vals_size; i++) {
		val = vals[i];
		if (val > 0)
			idx_max = i;
		if (val > val_max)
			val_max = val;
	}

	if (idx_max < 0)
		return;

	printf("%*s%-*s : count    distribution\n", idx_max <= 32 ? 5 : 15, "",
		idx_max <= 32 ? 19 : 29, val_type);

	if (idx_max <= 32)
		stars = stars_max;
	else
		stars = stars_max / 2;

	for (i = 0; i <= idx_max; i++) {
		low = (1ULL << (i + 1)) >> 1;
		high = (1ULL << (i + 1)) - 1;
		if (low == high)
			low -= 1;
		val = vals[i];
		width = idx_max <= 32 ? 10 : 20;
		printf("%*lld -> %-*lld : %-8d |", width, low, width, high, val);
		print_stars(val, val_max, stars);
		printf("|\n");
	}
}

static int print_hists(struct bpf_map *hists)
{
	struct hist_key lookup_key = { .dev = -1 }, next_key;
	int err, fd = bpf_map__fd(hists);
	struct hist hist;

	while (!bpf_map_get_next_key(fd, &lookup_key, &next_key)) {
		err = bpf_map_lookup_elem(fd, &next_key, &hist);
		if (err < 0) {
			printf("Failed to lookup hist: %d\n", err);
			return -1;
		}

		printf("\n");
		print_log2_hist(hist.alloc_to_throend, MAX_SLOTS, "A2T_usecs");
		print_log2_hist(hist.throend_to_rqsend, MAX_SLOTS, "T2R_usecs");
		print_log2_hist(hist.rqsend_to_getrq, MAX_SLOTS, "R2G_usecs");
		print_log2_hist(hist.getrq_to_insert, MAX_SLOTS, "G2I_usecs");
		print_log2_hist(hist.insert_to_issue, MAX_SLOTS, "I2D_usecs");
		print_log2_hist(hist.getrq_to_issue, MAX_SLOTS, "G2D_usecs");
		print_log2_hist(hist.issue_to_complete, MAX_SLOTS, "D2C_usecs");
		print_log2_hist(hist.alloc_to_complete, MAX_SLOTS, "A2C_usecs");

		printf("\n");
		print_latency_info(hist.latency_info);

		lookup_key = next_key;
	}

	if (env.isdelete) {
		lookup_key.dev = -1;
		while (!bpf_map_get_next_key(fd, &lookup_key, &next_key)) {
			err = bpf_map_delete_elem(fd, &next_key);
			if (err < 0) {
				printf("Failed to cleanup hist : %d\n", err);
				return -1;
			}
			lookup_key = next_key;
		}
	}

	return 0;
}

static __always_inline __u32 encode_dev(__u32 dev)
{
	unsigned major = MAJOR(dev);
	unsigned minor = MINOR(dev);
	return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}

static int get_dev(__u32 *dev)
{
	int fd;

	fd = open(env.disk, O_RDONLY);
	if (fd < 0) {
		perror(env.disk);
		return 1;
	}

	struct stat statbuf = {0};
	if (fstat(fd, &statbuf) < 0) {
		perror("fstat");
		goto cleanup;
	}

	if (!S_ISBLK(statbuf.st_mode)) {
		printf("%s Not a block device\n", env.disk);
		goto cleanup;
	}

	*dev = encode_dev(statbuf.st_rdev);
	__u32 major = MAJOR(*dev);
	__u32 minor = MINOR(*dev);
	printf("dev: %u, major: %u, minor:%u\n", *dev, major, minor);

	close(fd);
	return 0;

cleanup:
	close(fd);
	return 1;
}

#define BUFF_LEN 18
static int set_threshold(void)
{
	char path[] = "/sys/kernel/debug/block/ /blk_glitch_detection/threshold";
	char *insert = basename(env.disk);
	char *space_pos;
	long long threshold;
	int len = strlen(env.threshold);

	/* max size needed by to express S64
	 * DEC: "9223372036854775807" --> 19
	 */
	if (len > BUFF_LEN) {
		printf("Failed to set threshold, Exceeding the U64 range.\n");
		return 1;
	}

	space_pos = strstr(path, " /blk_glitch_detection");
	if (!space_pos || !insert)
		return 1;

	/*
	 * Calculate the number of characters to move (from after the
	 * space to the null terminator), with +1 to include
	 * the null terminator '\0'.
	 */
	size_t move_len = strlen(space_pos + 1) + 1;

	memmove(space_pos + strlen(insert), space_pos + 1, move_len);
	/* insert disk. */
	memcpy(space_pos, insert, strlen(insert));
	printf("path: %s\n", path);

	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	write(fd, env.threshold, len);
	close(fd);

	threshold = atoll(env.threshold);

	printf("Set the threshold for A2C to: %lldus \n", threshold);

	return 0;
}

static void sig_handler(int sig)
{
	exiting = 1;
}

int main(int argc, char *argv[])
{
	struct ioglitch_bpf *obj;
	int err;
	__u32 dev;
	const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	if (env.verbose) {
		printf("version: %s\n", VERSION);
		return 0;
	}

	if (!env.disk) {
		perror("No disk requires tracing. blkiodelaydetection --help.\n");
		return 1;
	}

	obj = ioglitch_bpf__open();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return 1;
	}

	if (get_dev(&dev)) {
		perror("Failed to get_dev!!!\n");
		return 1;
	}
	obj->rodata->target_dev = dev;

	if (set_threshold()) {
		perror("Failed set_threshold.\n");
		goto cleanup;
	}

	err = ioglitch_bpf__load(obj);
	if (err) {
		fprintf(stderr, "failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	err = ioglitch_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	signal(SIGINT, sig_handler);

	printf("\nIO glitch detection tool is now running.\n\n");

	while (!exiting) {
		sleep(env.interval);

		print_hists(obj->maps.hists);

		if (exiting)
			break;
	}

cleanup:
	ioglitch_bpf__destroy(obj);

	return err != 0;
}
