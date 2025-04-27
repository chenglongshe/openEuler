#include "enfs_lookup_cache.h"

int enfs_rpc_init(void)
{
	int ret = 0;
	ret = enfs_lookupcache_init();

	return ret;
}


