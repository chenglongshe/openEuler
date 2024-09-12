Ceph Code Explanation
=====================

This document provides a detailed breakdown of the functionalities in the provided source code snippets, covering various aspects of Ceph including CRUSH algorithm, authentication mechanisms, buffer management, hash functions, messaging protocols, monitor client session management, key management, and message handling.

CRUSH Module
------------

**Function Description**

CRUSH (Controlled Replication Under Scalable Hashing) is a core algorithm in Ceph used for data distribution and redundancy strategies. The ``crush_get_bucket_item_weight`` function retrieves the weight of an item within a bucket. It calculates and returns the weight of an item within a bucket based on the bucket type (uniform, list, tree, straw, or improved straw).


.. code-block:: c

    /**
     * crush_get_bucket_item_weight - Get weight of an item in given bucket
     * @b: bucket pointer
     * @p: item index in bucket
     */
    int crush_get_bucket_item_weight(const struct crush_bucket *b, int p)
    {
        if ((__u32)p >= b->size)
            return 0;
        switch (b->alg) {
        case CRUSH_BUCKET_UNIFORM:
            return ((struct crush_bucket_uniform *)b)->item_weight;
        case CRUSH_BUCKET_LIST:
            return ((struct crush_bucket_list *)b)->item_weights[p];
        case CRUSH_BUCKET_TREE:
            return ((struct crush_bucket_tree *)b)->node_weights[crush_calc_tree_node(p)];
        case CRUSH_BUCKET_STRAW:
            return ((struct crush_bucket_straw *)b)->item_weights[p];
        case CRUSH_BUCKET_STRAW2:
            return ((struct crush_bucket_straw2 *)b)->item_weights[p];
        }
        return 0;
    }

    void crush_destroy_bucket_uniform(struct crush_bucket_uniform *b)
    {
        kfree(b->h.items);
        kfree(b);
    }

    void crush_destroy_bucket_list(struct crush_bucket_list *b)
    {
        kfree(b->item_weights);
        kfree(b->sum_weights);
        kfree(b->h.items);
        kfree(b);
    }

    void crush_destroy_bucket_tree(struct crush_bucket_tree *b)
    {
        kfree(b->h.items);
        kfree(b->node_weights);
        kfree(b);
    }

    void crush_destroy_bucket_straw(struct crush_bucket_straw *b)
    {
        kfree(b->straws);
        kfree(b->item_weights);
        kfree(b->h.items);
        kfree(b);
    }

    void crush_destroy_bucket_straw2(struct crush_bucket_straw2 *b)
    {
        kfree(b->item_weights);
        kfree(b->h.items);
        kfree(b);
    }


Authentication Module
---------------------

**Function Description**

The authentication module provides a mechanism to verify a client's identity, ensuring only authorized users can access storage resources. The ``ceph_auth_init_protocol`` function initializes a specific protocol's authentication handler. The ``ceph_auth_init`` function sets up the initial work for authenticating clients, including allocating memory and initializing mutexes.


.. code-block:: c

    static u32 supported_protocols[] = {
        CEPH_AUTH_NONE,
        CEPH_AUTH_CEPHX
    };

    static int ceph_auth_init_protocol(struct ceph_auth_client *ac, int protocol)
    {
        switch (protocol) {
        case CEPH_AUTH_NONE:
            return ceph_auth_none_init(ac);
        case CEPH_AUTH_CEPHX:
            return ceph_x_init(ac);
        default:
            return -ENOENT;
        }
    }

    struct ceph_auth_client *ceph_auth_init(const char *name, const struct ceph_crypto_key *key)
    {
        struct ceph_auth_client *ac;
        int ret;
        dout("auth_init name '%s'\n", name);
        ret = -ENOMEM;
        ac = kzalloc(sizeof(*ac), GFP_NOFS);
        if (!ac)
            goto out;
        mutex_init(&ac->mutex);
        ac->negotiating = true;
        if (name)
            ac->name = name;
        else
            ac->name = CEPH_AUTH_NAME_DEFAULT;
        dout("auth_init name %s\n", ac->name);
        ac->key = key;
        return ac;
    out:
        return ERR_PTR(ret);
    }


Buffer Management Module
------------------------

**Function Description**

The buffer management module provides functions for creating, releasing, and decoding memory buffers, ensuring efficient data transmission over the network. The ``ceph_buffer_new`` function creates a new buffer, while ``ceph_buffer_release`` releases a buffer resource.


.. code-block:: c

    struct ceph_buffer *ceph_buffer_new(size_t len, gfp_t gfp)
    {
        struct ceph_buffer *b;
        b = kmalloc(sizeof(*b), gfp);
        if (!b)
            return NULL;
        b->vec.iov_base = ceph_kvmalloc(len, gfp);
        if (!b->vec.iov_base) {
            kfree(b);
            return NULL;
        }
        kref_init(&b->kref);
        b->alloc_len = len;
        b->vec.iov_len = len;
        dout("buffer_new %p\n", b);
        return b;
    }

    void ceph_buffer_release(struct kref *kref)
    {
        struct ceph_buffer *b = container_of(kref, struct ceph_buffer, kref);
        dout("buffer_release %p\n", b);
        kvfree(b->vec.iov_base);
        kfree(b);
    }


Ceph Hash Functions
-------------------

**Function Description**

Ceph uses hash functions to process strings, where ``ceph_str_hash`` returns the hash value of a string based on the specified hash type.


.. code-block:: c

    unsigned int ceph_str_hash(int type, const char *s, unsigned int len)
    {
        switch (type) {
        case CEPH_STR_HASH_LINUX:
            return ceph_str_hash_linux(s, len);
        case CEPH_STR_HASH_RJENKINS:
            return ceph_str_hash_rjenkins(s, len);
        default:
            return -1;
        }
    }

    const char *ceph_str_hash_name(int type)
    {
        switch (type) {
        case CEPH_STR_HASH_LINUX:
            return "linux";
        case CEPH_STR_HASH_RJENKINS:
            return "rjenkins";
        default:
            return "unknown";
        }
    }


Messenger Module
----------------

**Function Description**

The messaging module is responsible for sending and receiving messages between Ceph components. The ``osd_sign_message`` function is used to sign a message, while ``osd_check_message_signature`` verifies a message's signature.


.. code-block:: c

    static int osd_sign_message(struct ceph_msg *msg)
    {
        struct ceph_osd *o = msg->con->private;
        struct ceph_auth_handshake *auth = &o->o_auth;
        return ceph_auth_sign_message(auth, msg);
    }

    static int osd_check_message_signature(struct ceph_msg *msg)
    {
        struct ceph_osd *o = msg->con->private;
        struct ceph_auth_handshake *auth = &o->o_auth;
        return ceph_auth_check_message_signature(auth, msg);
    }


Mon Client Module
-----------------

**Function Description**

The Ceph monitor client maintains an active session with a monitor at all times to receive timely MDS map updates. If the connection breaks, the client will randomly search for a new monitor and resend any outstanding requests.


.. code-block:: c

    // Code related to maintaining an active session with the monitor
    // ...
    We maintain an open, active session with a monitor at all times in order to
    receive timely MDSMap updates. We periodically send a keepalive byte on the
    TCP socket to ensure we detect a failure. If the connection does break, we
    randomly hunt for a new monitor. Once the connection is reestablished, we
    resend any outstanding requests.


Key Management
--------------

**Function Description**

The key management module handles operations such as loading, pre-parsing, and destroying keys.


.. code-block:: c

    static int ceph_key_preparse(struct key_preparsed_payload *prep)
    {
        struct ceph_crypto_key *ckey;
        size_t datalen = prep->datalen;
        int ret;
        void *p;
        ret = -EINVAL;
        if (datalen <= 0 || datalen > 32767 || !prep->data)
            goto err;
        ret = -ENOMEM;
        ckey = kmalloc(sizeof(*ckey), GFP_KERNEL);
        if (!ckey)
            goto err;
        /* TODO ceph_crypto_key_decode should really take const input */
        p = (void *)prep->data;
        ret = ceph_crypto_key_decode(ckey, &p, (char*)prep->data+datalen);
        if (ret < 0)
            goto err_ckey;
        prep->payload.data[0] = ckey;
        prep->quotalen = datalen;
        return 0;
    err_ckey:
        kfree(ckey);
    err:
        return ret;
    }

    static void ceph_key_free_preparse(struct key_preparsed_payload *prep)
    {
        struct ceph_crypto_key *ckey = prep->payload.data[0];
        ceph_crypto_key_destroy(ckey);
        kfree(ckey);
    }

    static void ceph_key_destroy(struct key *key)
    {
        struct ceph_crypto_key *ckey = key->payload.data[0];
        ceph_crypto_key_destroy(ckey);
        kfree(ckey);
    }


Message Handling
----------------

**Function Description**

The message handling mechanism ensures secure message transmission, including preparing messages, reading messages, and processing messages.


.. code-block:: c

    static void prepare_write_message(struct ceph_connection *con)
    {
        struct ceph_msg *m;
        u32 crc;
        con_out_kvec_reset(con);
        con->out_msg_done = false;
        /* Sneak an ack in there first? If we can get it into the same
           TCP packet that's a good thing. */
        if (con->in_seq > con->in_seq_acked) {
            con->in_seq_acked = con->in_seq;
            con_out_kvec_add(con, sizeof (tag_ack), &tag_ack);
            con->out_temp_ack = cpu_to_le64(con->in_seq_acked);
            con_out_kvec_add(con, sizeof (con->out_temp_ack),
                             &con->out_temp_ack);
        }
        BUG_ON(list_empty(&con->out_queue));
        m = list_first_entry(&con->out_queue, struct ceph_msg, list_head);
        con->out_msg = m;
        BUG_ON(m->con != con);
        /* put message on sent list */
        ceph_msg_get(m);
        list_move_tail(&m->list_head, &con->out_sent);
        /*
         * only assign outgoing seq # if we haven't sent this message
         * yet. if it is requeued, resend with its original seq.
         */
        if (m->needs_out_seq) {
            m->hdr.seq = cpu_to_le64(++con->out_seq);
            m->needs_out_seq = false;
            if (con->ops->reencode_message)
                con->ops->reencode_message(m);
        }
        dout("prepare_write_message %p seq %lld type %d len %d+%d+%zd\n",
             m, con->out_seq, le16_to_cpu(m->hdr.type),
             le32_to_cpu(m->hdr.front_len), le32_to_cpu(m->hdr.middle_len),
             m->data_length);
        WARN_ON(m->front.iov_len != le32_to_cpu(m->hdr.front_len));
        WARN_ON(m->data_length != le32_to_cpu(m->hdr.data_len));
        /* tag + hdr + front + middle */
        con_out_kvec_add(con, sizeof (tag_msg), &tag_msg);
        con_out_kvec_add(con, sizeof(con->out_hdr), &con->out_hdr);
        con_out_kvec_add(con, m->front.iov_len, m->front.iov_base);
        if (m->middle)
            con_out_kvec_add(con, m->middle->vec.iov_len,
                             m->middle->vec.iov_base);
        /* fill in hdr crc and finalize hdr */
        crc = crc32c(0, &m->hdr, offsetof(struct ceph_msg_header, crc));
        con->out_msg->hdr.crc = cpu_to_le32(crc);
        memcpy(&con->out_hdr, &con->out_msg->hdr, sizeof(con->out_hdr));
        /* fill in front and middle crc, footer */
        crc = crc32c(0, m->front.iov_base, m->front.iov_len);
        con->out_msg->footer.front_crc = cpu_to_le32(crc);
        if (m->middle) {
            crc = crc32c(0, m->middle->vec.iov_base,
                         m->middle->vec.iov_len);
            con->out_msg->footer.middle_crc = cpu_to_le32(crc);
        } else
            con->out_msg->footer.middle_crc = 0;
        dout("%s front_crc %u middle_crc %u\n", __func__,
             le32_to_cpu(con->out_msg->footer.front_crc),
             le32_to_cpu(con->out_msg->footer.middle_crc));
        con->out_msg->footer.flags = 0;
    }