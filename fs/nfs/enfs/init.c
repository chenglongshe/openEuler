/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: enfs client init
 * Author: y00583252
 * Create: 2023-07-31
 */

#include "init.h"
#include "enfs_log.h"
#include "enfs_multipath.h"
#include "enfs_tp_common.h"
#include "mgmt_init.h"
#include "dns_internal.h"
#include "shard.h"

struct enfs_init_entry {
	char *name;
	int (*init)(void);
	void (*final)(void);
};

static inline void init_helper_finalize(struct enfs_init_entry *job, int idx)
{
	struct enfs_init_entry *entry = NULL;

	while (idx > 0) {
		idx = idx - 1;
		entry = &job[idx];
		if (entry->final != NULL) {
			entry->final();
			enfs_log_error("final %s.\n", entry->name);
		}
	}
}

static inline int init_helper_init(struct enfs_init_entry *job, int size)
{
	int ret;
	int i;
	struct enfs_init_entry *entry = NULL;

	for (i = 0; i < size; i++) {
		entry = &job[i];
		ret = entry->init();
		if (ret) {
			enfs_log_error("init step(%d) init(%s) fail.\n", i, entry->name);
			goto init_err;
		}
	}

	return 0;

init_err:
	init_helper_finalize(job, i);
	return -1;
}

static struct enfs_init_entry init_entry[] = {
	{"multipath", enfs_multipath_init, enfs_multipath_exit},
#ifdef NFS_CLIENT_DEBUG
	{"tracepoit", enfs_tracepoint_init, enfs_tracepoint_exit},
#endif  // NFS_CLIENT_DEBUG
	{"shard", enfs_shard_init, enfs_shard_exit},
	{"mgmt", mgmt_init, mgmt_fini},
	{"dns", enfs_dns_init, enfs_dns_exit},
};

int32_t enfs_init(void)
{
	return init_helper_init(init_entry, ARRAY_SIZE(init_entry));
}

void enfs_fini(void)
{
	init_helper_finalize(init_entry, ARRAY_SIZE(init_entry));
}