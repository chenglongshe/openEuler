#ifndef VIRTCCA_MIG_H
#define VIRTCCA_MIG_H

struct virtcca_bind_info {
    int16_t version;
    bool premig_done;
};

typedef struct virtcca_dst_host_info {
    char dst_ip[16];
    uint16_t dst_port;
	uint8_t version;
} virtcca_dst_host_info_t;

struct virtcca_mig_mbmd_data {  /* both kvm and tmm can access */
	__u16 size;
	__u16 mig_version;
	__u16 migs_index;  /* corresponding stream idx */
	__u8  mb_type;
	__u8  rsvd0; /* reserve bit */
	__u32 mb_counter;
	__u32 mig_epoch;
	__u64 iv_counter;
	__u8  type_specific_info[];
} __packed;

struct virtcca_mig_mbmd {
	struct virtcca_mig_mbmd_data *data;
	uint64_t hpa_and_size; /* Host physical address and size of the mbmd */
};

#define VIRTCCA_MIG_EPOCH_START_TOKEN 0xffffffff

/*
 * The buffer list specifies a list of 4KB pages to be used by TDH_EXPORT_MEM
 * and TDH_IMPORT_MEM to export and import guest memory pages. Each entry
 * is 64-bit and points to a physical address of a 4KB page used as buffer. The
 * list itself is a 4KB page, so it can hold up to 512 entries.
 */
union virtcca_mig_buf_list_entry {
	uint64_t val;
	struct {
		uint64_t rsvd0		: 12;
		uint64_t pfn		: 40;
		uint64_t rsvd1		: 11;
		uint64_t invalid	: 1;
	};
};

struct virtcca_mig_buf_list {
	union virtcca_mig_buf_list_entry *entries;
    // uint64_t *entries;
	hpa_t hpa;
};

/*
 * The page list specifies a list of 4KB pages to be used by the non-memory
 * states export and import, i.e. TDH_EXPORT_STATE_* and TDH_IMPORT_STATE_*.
 * Each entry is 64-bit and specifies the physical address of a 4KB buffer.
 * The list itself is a 4KB page, so it can hold up to 512 entries.
 */
union virtcca_mig_page_list_info {
	uint64_t val;
	struct {
		uint64_t rsvd0		: 12;
		uint64_t pfn	: 40;
        uint64_t rsvd1		: 3;
        uint64_t last_entry	: 9;
	};
};

struct virtcca_mig_page_list {
	hpa_t *entries;
	union virtcca_mig_page_list_info info;
};

union virtcca_mig_gpa_list_entry {
	uint64_t val;
	struct{
		uint64_t level          : 2;   /* Bits 1:0  :  Mapping level */
		uint64_t pending        : 1;   /* Bit 2     :  Page is pending */
		uint64_t reserved_0     : 4;   /* Bits 6:3 */
		uint64_t l2_map         : 3;   /* Bits 9:7  :  L2 mapping flags */
		uint64_t mig_type       : 2;   /* Bits 11:10:  Migration type */
		uint64_t gfn            : 40;  /* Bits 51:12 */
#define GPA_LIST_OP_NOP		0
#define GPA_LIST_OP_EXPORT	1
#define GPA_LIST_OP_CANCEL	2
		uint64_t operation      : 2;   /* Bits 53:52 */
		uint64_t reserved_1     : 2;   /* Bits 55:54 */
#define GPA_LIST_S_SUCCESS	0
		uint64_t status         : 5;   /* Bits 56:52 */
		uint64_t reserved_2     : 3;   /* Bits 63:61 */
	};
};

#define TMM_MAX_DIRTY_BITMAP_LEN 8
/*
 * The GPA list specifies a list of GPAs to be used by TDH_EXPORT_MEM and
 * TDH_IMPORT_MEM, TDH_EXPORT_BLOCKW, and TDH_EXPORT_RESTORE. The list itself
 * is 4KB, so it can hold up to 512 such 64-bit entries.
 */
union virtcca_mig_ipa_list_info {
	uint64_t val;
	struct {
		uint64_t rsvd0		: 3;
		uint64_t first_entry	: 9;
		uint64_t pfn		: 40;
		uint64_t rsvd1		: 3;
		uint64_t last_entry	: 9;
		
	};
};

struct virtcca_mig_gpa_list {
	union virtcca_mig_gpa_list_entry *entries;
	union virtcca_mig_ipa_list_info info;
};

/*
 * A MAC list specifies a list of MACs over 4KB migrated pages and their GPA
 * entries. It is used by TDH_EXPORT_MEM and TDH_IMPORT_MEM. Each entry is
 * 128-bit containing a single AES-GMAC-256 of a migrated page. The list itself
 * is a 4KB page, so it can hold up to 256 entries. To support the export and
 * import of 512 pages, two such MAC lists are needed to be passed to the TDX
 * module.
 */
struct virtcca_mig_mac_list {
	void *entries;
	hpa_t hpa;
};

union virtcca_mig_stream_info {
	uint64_t val;
	struct {
		uint64_t index	: 16;
		uint64_t rsvd	: 47;
		uint64_t resume	: 1;
	};
	struct {
		uint64_t rsvd1	  : 63;
		uint64_t in_order : 1;
	};
};

struct virtcca_mig_stream {
	uint16_t idx; /* stream id */
	uint32_t buf_list_pages;  /* ns memory page number of buf_list 5<n<512 */

	struct virtcca_mig_mbmd mbmd;   /* ns memory */
    /* for export status and mem*/
	/* List of buffers to export/import the private memory data */
	struct virtcca_mig_buf_list mem_buf_list; 
	/* List of buffers to export/miport the non-memory state data */
	struct virtcca_mig_page_list page_list; 
	/* List of GPA entries used when export/import private memory */
	struct virtcca_mig_gpa_list gpa_list;
	/* List of MACs used when export/import private memory */
	struct virtcca_mig_mac_list mac_list[2]; 
	/* List of dest private pages */
	struct virtcca_mig_buf_list dst_buf_list;
	/*
	 * List of buffers grabbed either from the private_fd allocated pages
	 * for in-place import or from mem_buf_list for non-in-place import.
	 */
	struct virtcca_mig_buf_list import_mem_buf_list;
	/*
	 * Bitmap to get if a gpa in the gpa_list to import needs first-time
	 * import, i.e. the leaf entry has not been set up in sept tables.
	 * Support up to 512 pages in a batch.
	 */
	uint64_t first_time_import_bitmap[8];
};

struct virtcca_mig_state {
	/* Number of streams created */
	atomic_t streams_created;
	/*
	 * Array to store physical addresses of the migration stream context
	 * pages that have been added to the TDX module. The pages can be
	 * reclaimed from TDX when TD is torn down.
	 */
	hpa_t *migsc_paddrs;
	struct virtcca_mig_gpa_list blockw_gpa_list;
	struct virtcca_mig_stream *default_stream;
	/* Backward stream used on migration abort during post-copy */
	struct virtcca_mig_stream backward_stream;
	hpa_t backward_migsc_paddr;
	bool bugged;
	/* Index of the next vCPU to export the state */
	uint32_t vcpu_export_next_idx;
	uint32_t mig_src;
};

struct virtcca_mig_capabilities {
	uint32_t max_migs;
	uint32_t nonmem_state_pages;
};

struct migcvm_agent_listen_cids {
	uint64_t cid;
};

struct tmi_mig_mem_data {
	uint64_t    gpa_list_info;
	uint64_t    mig_buff_list_pa;
    uint64_t    mig_cmd;
    uint64_t    mbmd_hpa_and_size;
	uint64_t    mac_pa0;
	uint64_t    mac_pa1;
};

struct tmi_mig_mem {
	struct tmi_mig_mem_data *data;
	uint64_t addr_and_size;
};

struct tmi_mig_memslot_data {
	uint64_t	dirty_bitmap_list[TMM_MAX_DIRTY_BITMAP_LEN];
	uint64_t	base_gfn;
	uint64_t	npages;
	short		memslot_id;
};

struct tmi_mig_memslot {
	struct tmi_mig_memslot_data *data;
	uint64_t addr_and_size;
};

typedef struct virtCCAMigInfo {
	uint64_t swiotlb_start;
	uint64_t swiotlb_end;
} virtCCAMigInfo;

enum slot_status {
	SLOT_IS_EMPTY = 0,
	SLOT_NOT_BINDED,
	SLOT_IS_BINDED,
	SLOT_IS_READY
};

/* vsock port for migvm */
#define MIGCVM_AGENT_PORT_SRC 9000
#define MIGCVM_AGENT_PORT_DST 9001

#define PAYLOAD_TYPE_ALL      0
#define PAYLOAD_TYPE_CHAR     1
#define PAYLOAD_TYPE_ULL      2
#define VSOCK_MSG_ACK         0xb
#define MAX_PAYLOAD_SIZE	  256

/* 1 bit aligned forced */
#pragma pack(push, 1)
typedef struct socket_payload {
    char            char_payload[MAX_PAYLOAD_SIZE];
    unsigned long long ull_payload;
} socket_payload_t;
typedef struct bind_msg_s {
    socket_payload_t payload;
	unsigned long long session_id;
    char            cmd[16];
    unsigned int    payload_type;
    unsigned int    payload_len;
	unsigned int    success;
} bind_msg_t;
#pragma pack(pop)

struct mig_cvm_update_info {
	uint64_t swiotlb_start;
	uint64_t swiotlb_end;
	uint64_t ipa_start;
};

#define SEC_MEM "secure"
#define NON_SEC_MEM "non-secure"

typedef struct {
	uint64_t ipa_start;
	uint64_t ipa_end;
	uint64_t granularity;
	bool is_secure;
	bool enabled;
} crc_config_t;

int kvm_virtcca_mig_stream_ops_init(void);
void kvm_virtcca_mig_stream_ops_exit(void);
int virtcca_mig_capabilities_setup(struct virtcca_cvm *cvm);
bool virtcca_is_migration_source(struct virtcca_cvm *cvm);
void virtcca_mig_state_release(struct virtcca_cvm *cvm);
int virtcca_mig_state_create(struct virtcca_cvm *cvm);
int virtcca_save_migvm_cid(struct kvm *guest_kvm, struct kvm_virtcca_mig_cmd *cmd);
int virtcca_migvm_agent_ratstls(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd);
int virtcca_migvm_agent_ratstls_dst(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd);
int virtcca_get_bind_info(struct kvm *kvm, struct kvm_virtcca_mig_cmd *cmd);
void virtcca_dump_checksum(struct kvm *kvm);
void crc32_init(void);
void virtcca_enable_log_dirty(struct kvm *kvm, uint64_t start, uint64_t end);
int virtcca_mig_export_abort(struct kvm *kvm);
#endif /* __KVM_VIRTCCA_MIG_H */