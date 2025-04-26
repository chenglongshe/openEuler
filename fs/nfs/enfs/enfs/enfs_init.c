#include
#include
#include
#include
#include
#include
#include
#include
#include "enfs.h"
#include "enfs_multipath_parse.h"
#include "enfs_multipath_client.h"
#include "enfs_remount.h"
#include "init.h"
#include "enfs_lookup_cache.h"
#include "enfs_rpc_init.h"

unsigned int enfs_debug;
module_param_named(enfs_debug, enfs_debug, uint, 0600);
MODULE_PARM_DESC(enfs_debug, "enfs debugging mask");

struct enfs_adapter_ops enfs_adapter = {
	.name = "enfs",
	.owner = THIS_MODULE,
	//.alloc_mount_option      = nfs_multipath_alloc_options,
	.parse_mount_options = nfs_multipath_parse_options,
	.free_mount_options = nfs_multipath_free_options,
	//.dup_mount_options       = nfs_multipath_dup_options,
	.client_info_init = nfs_multipath_client_info_init,
	.client_info_free = nfs_multipath_client_info_free,
	.client_info_match = nfs_multipath_client_info_match,
	.nfs4_client_info_match = nfs4_multipath_client_info_match,
	.client_info_show = nfs_multipath_client_info_show,
	// .client_info_clone       = nfs_multipath_client_info_clone,
	// .get_best_conn           = nfs_multipath_get_best_conn,
	// .conn_set_unavailable    = nfs_multipath_set_conn_disconnect,
	.remount_ip_list = enfs_remount,
	.set_mount_data = enfs_set_mount_data,
	.trigger_get_capability = enfs_trigger_get_capability,
};

static int __init init_enfs(void)
{
	int ret;
	ret = enfs_adapter_register(&enfs_adapter);
	if (ret) {
		printk(KERN_ERR "regist enfs_adapter fail. ret %d\n", ret);
		return -1;
	}

	ret = enfs_init();
	if (ret) {
		enfs_adapter_unregister(&enfs_adapter);
		return -1;
	}

	ret = enfs_rpc_init();
	if (ret) {
		enfs_adapter_unregister(&enfs_adapter);
		return -1;
	}

	return 0;
}

static void __exit exit_enfs(void)
{
	enfs_lookupcache_fini();
	enfs_fini();
	enfs_adapter_unregister(&enfs_adapter);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_DESCRIPTION("Nfs client router");
MODULE_VERSION("1.0");

module_init(init_enfs);
module_exit(exit_enfs);
