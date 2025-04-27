/* 
 * Copy from net/dns_resolver, please see the original file.
 */
#ifndef _DNS_INTERNAL_H_
#define _DNS_INTERNAL_H_

#include 
#include 
#include 

#include "enfs_log.h"
 
struct multipath_mount_options;

/*
 * Layout of key payload words.
 */
enum {
	enfs_dns_key_data,
	enfs_dns_key_error,
};

/*
 * dns_key.c
 */
extern const struct cred *enfs_dns_resolver_cache;

/*
 * debug tracing
 */
extern unsigned int enfs_dns_resolver_debug;

#define kdebug(FMT, ...)                                                           \
	do {                                                                           \
		if (unlikely(enfs_dns_resolver_debug))                                     \
			printk(KERN_DEBUG "[%-6.6s] " FMT "\n", current->comm, ##__VA_ARGS__); \
	} while (0)

#define kenter(FMT, ...) kdebug("==> %s(" FMT ")", __func__, ##__VA_ARGS__)
#define kleave(FMT, ...) kdebug("<== %s()" FMT "", __func__, ##__VA_ARGS__)

#if (defined(ENFS_EULER_5_10) || defined(ENFS_OPENEULER_660))
	int enfs_euler_dns_query(struct net *net,
	      const char *type, const char *name, size_t namelen,
	      const char *options, char **_result, time64_t *_expiry,
	      bool invalidate);
#else
	int enfs_dns_query(const char *type, const char *name, size_t namelen,
                   const char *options, char **_result, time64_t *_expiry);
#endif

int init_dns_resolver(void);
void exit_dns_resolver(void);

void enfs_add_domain_name(struct multipath_mount_options *opt);
void enfs_debug_print_name_list(void);

int enfs_dns_init(void);
void enfs_dns_exit(void);

#endif  // _DNS_INTERNAL_H_
