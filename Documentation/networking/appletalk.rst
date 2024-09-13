# SPDX-License-Identifier: GPL-2.0

AppleTalk Protocol Suite Implementation
========================================

This document describes the implementation of the AppleTalk protocol suite, including the AppleTalk Address Resolution Protocol (AARP), Datagram Delivery Protocol (DDP), device interface, and system control interfaces.

AARP (AppleTalk Address Resolution Protocol)
--------------------------------------------

**Function Description**

The ``aarp.c`` file implements the AppleTalk Address Resolution Protocol (AARP) for Ethernet networks, specifically for Ethernet Local Area Protocol (ELAP). Key functionalities include:

- **AARP Entry Structure**: The ``aarp_entry`` structure encapsulates details about each AARP entry, such as the last transmission time (``last_sent``), a queue of packets awaiting resolution (``packet_queue``), status (``status``), expiration time (``expires_at``), target AppleTalk address (``target_addr``), associated network device (``dev``), hardware address (``hwaddr``), transmission count (``xmit_count``), and a pointer to the next entry in the chain (``next``).

- **Hash Tables Management**: Three hash tables (``resolved``, ``unresolved``, and ``proxies``) manage entries based on their resolution status.

- **Lock Protection**: A read-write lock (``aarp_lock``) is defined to protect the entire AARP structure from concurrent access issues.

- **Timer**: The ``aarp_timer`` is utilized to periodically traverse the list and purge expired entries or perform other necessary operations.

**Source Code**

.. code-block:: c

    // AARP: An implementation of the AppleTalk AARP protocol for
    // Ethernet 'ELAP'.
    ...
    struct aarp_entry {
        /* These first two are only used for unresolved entries */
        unsigned long    last_sent;
        struct sk_buff_head packet_queue;
        int     status;
        unsigned long    expires_at;
        struct atalk_addr  target_addr;
        struct net_device  *dev;
        char      hwaddr[ETH_ALEN];
        unsigned short   xmit_count;
        struct aarp_entry  *next;
    };
    ...
    static struct aarp_entry *resolved[AARP_HASH_SIZE];
    static struct aarp_entry *unresolved[AARP_HASH_SIZE];
    static struct aarp_entry *proxies[AARP_HASH_SIZE];
    static int unresolved_count;
    /* One lock protects it all. */
    static DEFINE_RWLOCK(aarp_lock);
    /* Used to walk the list and purge/kick entries. */
    static struct timer_list aarp_timer;

DDP (Datagram Delivery Protocol)
--------------------------------

**Function Description**

The ``ddp.c`` file implements the Datagram Delivery Protocol (DDP) for AppleTalk, which is responsible for the delivery of datagrams. Its key features include:

- **Socket Management**: The ``atalk_sockets`` hash chain stores all AppleTalk sockets, and the ``atalk_sockets_lock`` lock ensures thread safety during insertion and removal operations.

- **Insertion and Removal of Sockets**: Functions like ``__atalk_insert_socket`` and ``atalk_remove_socket`` are used to add or remove sockets from the list, respectively.

- **Socket Search**: ``atalk_search_socket`` is employed to find a socket based on the specified destination address within the socket list.

**Source Code**

.. code-block:: c

    // DDPE: An implementation of the AppleTalk DDP protocol for
    // Ethernet 'ELAP'.
    ...
    struct datalink_proto *ddp_dl, *aarp_dl;
    static const struct proto_ops atalk_dgram_ops;
    ...
    static inline void __atalk_insert_socket(struct sock *sk)
    {
        sk_add_node(sk, &atalk_sockets);
    }
    static inline void atalk_remove_socket(struct sock *sk)
    {
        write_lock_bh(&atalk_sockets_lock);
        sk_del_node_init(sk);
        write_unlock_bh(&atalk_sockets_lock);
    }
    static struct sock *atalk_search_socket(struct sockaddr_at *to,
                                            struct atalk_iface *atif)
    {
        struct sock *s;
        read_lock_bh(&atalk_sockets_lock);
        sk_for_each(s, &atalk_sockets) {
            ...
        }
    }

Device Interface
----------------

**Function Description**

The ``dev.c`` file defines the implementation of the AppleTalk network device interface, particularly focusing on actions performed when a device is brought up or taken down.

- **Initialization of LocalTalk Device**: The ``ltalk_setup`` function fills in the device structure with LocalTalk-specific values.

- **Allocation of LocalTalk Device**: The ``alloc_ltalkdev`` function allocates memory for a LocalTalk device and calls ``ltalk_setup`` to initialize it.

**Source Code**

.. code-block:: c

    // Moved here from drivers/net/net_init.c, which is:
    // Written 1993,1994,1995 by Donald Becker.
    ...
    static void ltalk_setup(struct net_device *dev)
    {
        /* Fill in the fields of the device structure with localtalk-generic values. */
        dev->type = ARPHRD_LOCALTLK;
        dev->hard_header_len = LTALK_HLEN;
        dev->mtu = LTALK_MTU;
        dev->addr_len = LTALK_ALEN;
        dev->tx_queue_len = 10;
        dev->broadcast[0] = 0xFF;
        dev->flags = IFF_BROADCAST|IFF_MULTICAST|IFF_NOARP;
    }
    ...
    struct net_device *alloc_ltalkdev(int sizeof_priv)
    {
        return alloc_netdev(sizeof_priv, "lt%d", NET_NAME_UNKNOWN,
                            ltalk_setup);
    }

System Control
--------------

**Function Description**

The ``sysctl_net_atalk.c`` file provides system control interfaces, enabling users to adjust AppleTalk-related system parameters through the ``sysctl`` command.

- **Sysctl Table Entries**: The ``atalk_table`` array defines several kernel parameters that can be modified or queried by user-space programs, such as the AARP expiry time and tick time.

**Source Code**

.. code-block:: c

    // sysctl_net_atalk.c: sysctl interface to net AppleTalk subsystem.
    ...
    static struct ctl_table atalk_table[] = {
        {
            .procname = "aarp-expiry-time",
            .data = &sysctl_aarp_expiry_time,
            .maxlen = sizeof(int),
            .mode = 0644,
            .proc_handler = proc_dointvec_jiffies,
        },
        {
            .procname = "aarp-tick-time",
            .data = &sysctl_aarp_tick_time,
            .maxlen = sizeof(int),
            .mode = 0644,
            .proc_handler = proc_dointvec_jiffies,
        },
        ...
    }

This document provides a structured overview of the functionalities in the AppleTalk protocol suite, detailing the AARP, DDP, device interface, and system control interfaces, along with relevant source code excerpts.