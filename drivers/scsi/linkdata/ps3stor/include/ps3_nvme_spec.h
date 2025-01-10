/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVME_SPEC_H
#define _NVME_SPEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ps3_types.h>

#define NVME_REG32_ERR_VAL ((unsigned int)(-1))
#define NVME_REG64_ERR_VAL ((unsigned long long)(-1))
#define NVME_MAX_IO_QUEUES (65536)
#define NVME_MAX_NUM_ADMIN_QD (4096)
#define NVME_MAX_NUM_IO_QD (65536)
#define NVME_MIN_NUM_ADMIN_QD (2)
#define NVME_CMD_SET_SUPPORT BIT(0)
#define NVME_INVALID_CID (0xFFFF)
#define NVME_MAX_WRITE_ZEROES (0xFFFF)
#define NVME_NSID_MASK (0xFFFFFFFF)

#define NVME_GET_SECTOR_SIZE(data)                                             \
	(1 << (data)->lbaf[(data)->flbas.format].lbads)

#define NVME_GET_SQ_DOOR_BELL_OFFSET(qid, strd)                                \
	(0x1000 + 2 * (qid) * (4 << (strd)))

#define NVME_GET_CQ_DOOR_BELL_OFFSET(qid, strd)                                \
	(0x1000 + (2 * (qid) + 1) * (4 << (strd)))

#define NVME_SANITIZE_MAX_PROG 0xFFFF

#define NVME_CAP_CSS_NVM 1

#define NVME_MPS_TO_PAGESIZE(mpsMin) (1u << (12 + (mpsMin)))

#define NVME_PAGESIZE_TO_MPS(pageSize) (nvmeU64Log2(pageSize) - 12)

union NvmeCapReg {
	unsigned long long rawVal;
	struct {
		unsigned int mqes : 16;
		unsigned int cqr : 1;
		unsigned int ams : 2;
		unsigned int rsvd : 5;
		unsigned int timeout : 8;
		unsigned int dbStride : 4;

		unsigned int nssrSupport : 1;
		unsigned int cmdSetSupport : 8;
		unsigned int bootPartSupport : 1;
		unsigned int rsvd1 : 2;
		unsigned int mpsMin : 4;
		unsigned int mpsMax : 4;
		unsigned int pmrSupport : 1;
		unsigned int cmbSupport : 1;
		unsigned int rsvd3 : 6;
	};
};

union NvmeVsReg {
	unsigned int rawVal;
	struct {
		unsigned char vsTerity;
		unsigned char vsMinorNum;
		unsigned short vsMajorNum;
	};
};

#define NVME_VERSION(mjr, mnr, ter)                                            \
	(((uint32_s)(mjr) << 16) | ((uint32_s)(mnr) << 8) | (uint32_s)(ter))

#define NVME_IO_SQ_ENTRY_SIZE 6

#define NVME_IO_CQ_ENTRY_SIZE 4

#define NVME_SHUT_DOWN_NO_EFFECT 0x0

#define NVME_SHUT_DOWN_NOTIFY 0x1

#define NVME_SHUT_DOWN_ABRUT_NOTIFY 0x2

#define NVME_CSS_NVM_CMD_SET 0x0

union NvmeCCReg {
	unsigned int rawVal;
	struct {
		unsigned int en : 1;
		unsigned int rsvd : 3;
		unsigned int css : 3;
		unsigned int mps : 4;
		unsigned int ams : 3;
		unsigned int shutdownNotify : 2;

		unsigned int ioSqEntrySz : 4;
		unsigned int ioCqEntrySz : 4;
		unsigned int rsvd1 : 8;
	};
};

#define NVME_SHST_NORMAL 0x0
#define NVME_SHST_OCCURRING 0x1
#define NVME_SHST_COMPLETE 0x2

union NvmeCstsReg {
	unsigned int rawVal;
	struct {
		unsigned int rdy : 1;
		unsigned int cfs : 1;
		unsigned int shutdownStatus : 2;
		unsigned int nssrOccur : 1;
		unsigned int proccessPaused : 1;
		unsigned int rsvd : 26;
	};
};

union NvmeAqaReg {
	unsigned int rawVal;
	struct {
		unsigned int adminSqSz : 12;
		unsigned int rsvd : 4;
		unsigned int adminCqSz : 12;
		unsigned int rsvd1 : 4;
	};
};

union NvmeCmbLocReg {
	unsigned int rawVal;
	struct {
		unsigned int bir : 3;
		unsigned int cmbQMixMemSupport : 1;
		unsigned int cmbQPhyDiscontiSupport : 1;
		unsigned int cmbDataPtrMixLocSupport : 1;
		unsigned int cmbDataPtrCmdIndentLocSupport : 1;
		unsigned int cmbDataMetaMemMixSupport : 1;
		unsigned int cmbQDwordAlignSupport : 1;
		unsigned int rsvd : 3;
		unsigned int offset : 20;
	};
};

union NvmeCmbSzReg {
	unsigned int rawVal;
	struct {
		unsigned int sqSupport : 1;
		unsigned int cqSupport : 1;
		unsigned int prpListSupport : 1;
		unsigned int readSupport : 1;
		unsigned int writeSupport : 1;
		unsigned int rsvd : 3;
		unsigned int szUnit : 4;
		unsigned int size : 20;
	};
};

struct NvmeControllerReg {
	union NvmeCapReg cap;
	union NvmeVsReg vs;
	unsigned int intmskSet;
	unsigned int intmskClr;
	union NvmeCCReg cc;
	unsigned int rsvd0;
	union NvmeCstsReg csts;
	unsigned int nssrCtrl;
	union NvmeAqaReg aqa;
	unsigned long long asq;
	unsigned long long acq;
	union NvmeCmbLocReg cmbLoc;
	union NvmeCmbSzReg cmbSz;

};

struct DBEntry {
	unsigned int sqT;
	unsigned int cqH;
};

enum NvmeSglDescSubType {
	NVME_SGL_SUBTYPE_ADDRESS = 0x0,
	NVME_SGL_SUBTYPE_OFFSET = 0x1,
	NVME_SGL_SUBTYPE_TRANSPORT = 0xa,
};

struct GenericSglDesc {
	unsigned int rsvd;
	unsigned int rsvd1 : 24;
	unsigned int subType : 4;
	unsigned int type : 4;
};

struct UnKeyedSglDesc {
	unsigned int len;
	unsigned int rsvd : 24;
	unsigned int subType : 4;
	unsigned int type : 4;
};

struct KeyedSglDesc {
	unsigned long long len : 24;
	unsigned long long key : 32;
	unsigned long long subType : 4;
	unsigned long long type : 4;
};

struct NvmeSglDesc {
	unsigned long long addr;
	union {
		struct GenericSglDesc genSgl;
		struct UnKeyedSglDesc unkeyedSgl;
		struct KeyedSglDesc keyedSgl;
	};
};

struct NvmeSanitize {
	unsigned int sanact : 3;

	unsigned int ause : 1;

	unsigned int owpass : 4;

	unsigned int oipbp : 1;

	unsigned int ndas : 1;

	unsigned int reserved : 22;
};

enum NvmeSanitizeAction {

	NVME_SANITIZE_EXIT_FAILURE_MODE = 0x1,

	NVME_SANITIZE_BLOCK_ERASE = 0x2,

	NVME_SANITIZE_OVERWRITE = 0x3,

	NVME_SANITIZE_CRYPTO_ERASE = 0x4,
};

struct NvmeCommonCmdEntry {
	unsigned short opc : 8;
	unsigned short fuse : 2;
	unsigned short rsvd : 4;
	unsigned short psdt : 2;
	unsigned short cid;

	unsigned int nsId;

	unsigned long long rsvd1;

	unsigned long long metaPtr;

	union {
		struct {
			unsigned long long prp1;
			union {
				unsigned long long prp2;
				struct {
					unsigned int metaDataLen;
					unsigned int dataLen;
				};
			};
		};
		struct NvmeSglDesc sgl;
	};

	union {
		struct {
			unsigned long long slba;
			unsigned int nlba : 16;
			unsigned int res1 : 8;
			unsigned int stc : 1;
			unsigned int res2 : 1;
			unsigned int prinfo : 4;
			unsigned int fua : 1;
			unsigned int lr : 1;
		};

		struct {
			struct NvmeSanitize sanitizeDw10;
			unsigned int overwritePattern;
		};
		struct {
			unsigned int dw10;
			unsigned int dw11;
			unsigned int dw12;
			unsigned int dw13;
			unsigned int dw14;
			unsigned int dw15;
		};
	};
};

#ifndef _WINDOWS
struct __packed NvmeCmdStatus {
#else
#pragma pack(1)
struct NvmeCmdStatus {
#endif
	union {
		struct {
			unsigned short p : 1;
			unsigned short sc : 8;
			unsigned short sct : 3;
			unsigned short crd : 2;
			unsigned short m : 1;
			unsigned short dnr : 1;
		};
		unsigned short cmdStatus;
	};
};

#ifdef _WINDOWS
#pragma pack()
#endif

#define NVME_GET_IO_SQ_NUM(cmdSpec) (((cmdSpec)&0xFFFF) + 1)

#define NVME_GET_IO_CQ_NUM(cmdSpec) (((cmdSpec) >> 16) + 1)

#define NVME_GET_POWER_STATE(cmdSpec) (((cmdSpec)&0X1F))

struct NvmeCplEntry {
	unsigned int cmdSpec;

	unsigned int rsvd;

	unsigned short sqHead;
	unsigned short sqId;

	unsigned short cid;
	struct NvmeCmdStatus status;
};

enum NvmeAdminOpcode {
	NVME_OPC_DELETE_IO_SQ = 0x00,
	NVME_OPC_CREATE_IO_SQ = 0x01,
	NVME_OPC_GET_LOG_PAGE = 0x02,

	NVME_OPC_DELETE_IO_CQ = 0x04,
	NVME_OPC_CREATE_IO_CQ = 0x05,
	NVME_OPC_IDENTIFY = 0x06,

	NVME_OPC_ABORT = 0x08,
	NVME_OPC_SET_FEATURES = 0x09,
	NVME_OPC_GET_FEATURES = 0x0a,

	NVME_OPC_ASYNC_EVENT_REQUEST = 0x0c,
	NVME_OPC_NS_MANAGEMENT = 0x0d,

	NVME_OPC_FIRMWARE_COMMIT = 0x10,
	NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD = 0x11,
	NVME_OPC_DEVICE_SELF_TEST = 0x14,
	NVME_OPC_NS_ATTACHMENT = 0x15,
	NVME_OPC_KEEP_ALIVE = 0x18,
	NVME_OPC_DIRECTIVE_SEND = 0x19,
	NVME_OPC_DIRECTIVE_RECEIVE = 0x1a,
	NVME_OPC_VIRTUALIZATION_MANAGEMENT = 0x1c,
	NVME_OPC_NVME_MI_SEND = 0x1d,
	NVME_OPC_NVME_MI_RECEIVE = 0x1e,
	NVME_OPC_DOORBELL_BUFFER_CONFIG = 0x7c,
	NVME_OPC_FORMAT_NVM = 0x80,
	NVME_OPC_SECURITY_SEND = 0x81,
	NVME_OPC_SECURITY_RECEIVE = 0x82,
	NVME_OPC_SANITIZE = 0x84,
};

enum NvmeIOOpcode {
	NVME_OPC_FLUSH = 0x00,
	NVME_OPC_WRITE = 0x01,
	NVME_OPC_READ = 0x02,

	NVME_OPC_WRITE_UNCORRECTABLE = 0x04,
	NVME_OPC_COMPARE = 0x05,

	NVME_OPC_WRITE_ZEROES = 0x08,
	NVME_OPC_DATASET_MANAGEMENT = 0x09,
	NVME_OPC_VERIFY = 0x0C,

	NVME_OPC_RESERVATION_REGISTER = 0x0d,
	NVME_OPC_RESERVATION_REPORT = 0x0e,

	NVME_OPC_RESERVATION_ACQUIRE = 0x11,
	NVME_OPC_RESERVATION_RELEASE = 0x15,
};

enum NvmeStatusCodeType {
	NVME_SCT_GENERIC = 0x0,
	NVME_SCT_COMMAND_SPECIFIC = 0x1,
	NVME_SCT_MEDIA_ERROR = 0x2,
	NVME_SCT_PATH = 0x3,

	NVME_SCT_VENDOR_SPECIFIC = 0x7,
};

enum NvmeGenCmdSC {
	NVME_SC_SUCCESS = 0x00,
	NVME_SC_INVALID_OPCODE = 0x01,
	NVME_SC_INVALID_FIELD = 0x02,
	NVME_SC_COMMAND_ID_CONFLICT = 0x03,
	NVME_SC_DATA_TRANSFER_ERROR = 0x04,
	NVME_SC_ABORTED_POWER_LOSS = 0x05,
	NVME_SC_INTERNAL_DEVICE_ERROR = 0x06,
	NVME_SC_ABORTED_BY_REQUEST = 0x07,
	NVME_SC_ABORTED_SQ_DELETION = 0x08,
	NVME_SC_ABORTED_FAILED_FUSED = 0x09,
	NVME_SC_ABORTED_MISSING_FUSED = 0x0a,
	NVME_SC_INVALID_NAMESPACE_OR_FORMAT = 0x0b,
	NVME_SC_COMMAND_SEQUENCE_ERROR = 0x0c,
	NVME_SC_INVALID_SGL_SEG_DESCRIPTOR = 0x0d,
	NVME_SC_INVALID_NUM_SGL_DESCIRPTORS = 0x0e,
	NVME_SC_DATA_SGL_LENGTH_INVALID = 0x0f,
	NVME_SC_METADATA_SGL_LENGTH_INVALID = 0x10,
	NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID = 0x11,
	NVME_SC_INVALID_CONTROLLER_MEM_BUF = 0x12,
	NVME_SC_INVALID_PRP_OFFSET = 0x13,
	NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED = 0x14,
	NVME_SC_OPERATION_DENIED = 0x15,
	NVME_SC_INVALID_SGL_OFFSET = 0x16,

	NVME_SC_HOSTID_INCONSISTENT_FORMAT = 0x18,
	NVME_SC_KEEP_ALIVE_EXPIRED = 0x19,
	NVME_SC_KEEP_ALIVE_INVALID = 0x1a,
	NVME_SC_ABORTED_PREEMPT = 0x1b,
	NVME_SC_SANITIZE_FAILED = 0x1c,
	NVME_SC_SANITIZE_IN_PROGRESS = 0x1d,
	NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID = 0x1e,
	NVME_SC_COMMAND_INVALID_IN_CMB = 0x1f,
	NVME_SC_NAMESPACE_IS_WRITE_PROTECTED = 0x20,
	NVME_SC_COMMAND_INTERRUPTED = 0x21,
	NVME_SC_TRANSIENT_TRANSPORT_ERROR = 0x22,

	NVME_SC_LBA_OUT_OF_RANGE = 0x80,
	NVME_SC_CAPACITY_EXCEEDED = 0x81,
	NVME_SC_NAMESPACE_NOT_READY = 0x82,
	NVME_SC_RESERVATION_CONFLICT = 0x83,
	NVME_SC_FORMAT_IN_PROGRESS = 0x84,
};

enum NvmeCmdSpecSC {
	NVME_CSC_COMPLETION_QUEUE_INVALID = 0x00,
	NVME_CSC_INVALID_QUEUE_IDENTIFIER = 0x01,
	NVME_CSC_MAXIMUM_QUEUE_SIZE_EXCEEDED = 0x02,
	NVME_CSC_ABORT_COMMAND_LIMIT_EXCEEDED = 0x03,

	NVME_CSC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED = 0x05,
	NVME_CSC_INVALID_FIRMWARE_SLOT = 0x06,
	NVME_CSC_INVALID_FIRMWARE_IMAGE = 0x07,
	NVME_CSC_INVALID_INTERRUPT_VECTOR = 0x08,
	NVME_CSC_INVALID_LOG_PAGE = 0x09,
	NVME_CSC_INVALID_FORMAT = 0x0a,
	NVME_CSC_FIRMWARE_REQ_CONVENTIONAL_RESET = 0x0b,
	NVME_CSC_INVALID_QUEUE_DELETION = 0x0c,
	NVME_CSC_FEATURE_ID_NOT_SAVEABLE = 0x0d,
	NVME_CSC_FEATURE_NOT_CHANGEABLE = 0x0e,
	NVME_CSC_FEATURE_NOT_NAMESPACE_SPECIFIC = 0x0f,
	NVME_CSC_FIRMWARE_REQ_NVM_RESET = 0x10,
	NVME_CSC_FIRMWARE_REQ_RESET = 0x11,
	NVME_CSC_FIRMWARE_REQ_MAX_TIME_VIOLATION = 0x12,
	NVME_CSC_FIRMWARE_ACTIVATION_PROHIBITED = 0x13,
	NVME_CSC_OVERLAPPING_RANGE = 0x14,
	NVME_CSC_NAMESPACE_INSUFFICIENT_CAPACITY = 0x15,
	NVME_CSC_NAMESPACE_ID_UNAVAILABLE = 0x16,

	NVME_CSC_NAMESPACE_ALREADY_ATTACHED = 0x18,
	NVME_CSC_NAMESPACE_IS_PRIVATE = 0x19,
	NVME_CSC_NAMESPACE_NOT_ATTACHED = 0x1a,
	NVME_CSC_THINPROVISIONING_NOT_SUPPORTED = 0x1b,
	NVME_CSC_CONTROLLER_LIST_INVALID = 0x1c,
	NVME_CSC_DEVICE_SELF_TEST_IN_PROGRESS = 0x1d,
	NVME_CSC_BOOT_PARTITION_WRITE_PROHIBITED = 0x1e,
	NVME_CSC_INVALID_CTRLR_ID = 0x1f,
	NVME_CSC_INVALID_SECONDARY_CTRLR_STATE = 0x20,
	NVME_CSC_INVALID_NUM_CTRLR_RESOURCES = 0x21,
	NVME_CSC_INVALID_RESOURCE_ID = 0x22,

	NVME_CSC_CONFLICTING_ATTRIBUTES = 0x80,
	NVME_CSC_INVALID_PROTECTION_INFO = 0x81,
	NVME_CSC_ATTEMPTED_WRITE_TO_RO_RANGE = 0x82,
};

enum NvmeMediaErrSC {
	NVME_MSC_WRITE_FAULTS = 0x80,
	NVME_MSC_UNRECOVERED_READ_ERROR = 0x81,
	NVME_MSC_GUARD_CHECK_ERROR = 0x82,
	NVME_MSC_APPLICATION_TAG_CHECK_ERROR = 0x83,
	NVME_MSC_REFERENCE_TAG_CHECK_ERROR = 0x84,
	NVME_MSC_COMPARE_FAILURE = 0x85,
	NVME_MSC_ACCESS_DENIED = 0x86,
	NVME_MSC_DEALLOCATED_OR_UNWRITTEN_BLOCK = 0x87,
};

enum NvmeStatusCodes {
	NVME_SUCCESS = 0x0000,
	NVME_INVALID_OPCODE = 0x0001,
	NVME_INVALID_FIELD = 0x0002,
	NVME_CID_CONFLICT = 0x0003,
	NVME_DATA_TRAS_ERROR = 0x0004,
	NVME_POWER_LOSS_ABORT = 0x0005,
	NVME_INTERNAL_DEV_ERROR = 0x0006,
	NVME_CMD_ABORT_REQ = 0x0007,
	NVME_CMD_ABORT_SQ_DEL = 0x0008,
	NVME_CMD_ABORT_FAILED_FUSE = 0x0009,
	NVME_CMD_ABORT_MISSING_FUSE = 0x000a,
	NVME_INVALID_NSID = 0x000b,
	NVME_CMD_SEQ_ERROR = 0x000c,
	NVME_INVALID_SGL_SEG_DESCR = 0x000d,
	NVME_INVALID_NUM_SGL_DESCRS = 0x000e,
	NVME_DATA_SGL_LEN_INVALID = 0x000f,
	NVME_MD_SGL_LEN_INVALID = 0x0010,
	NVME_SGL_DESCR_TYPE_INVALID = 0x0011,
	NVME_INVALID_USE_OF_CMB = 0x0012,
	NVME_INVALID_PRP_OFFSET = 0x0013,
	NVME_CMD_SET_CMB_REJECTED = 0x002b,
	NVME_INVALID_CMD_SET = 0x002c,
	NVME_LBA_RANGE = 0x0080,
	NVME_CAP_EXCEEDED = 0x0081,
	NVME_NS_NOT_READY = 0x0082,
	NVME_NS_RESV_CONFLICT = 0x0083,
	NVME_FORMAT_IN_PROGRESS = 0x0084,
	NVME_INVALID_CQID = 0x0100,
	NVME_INVALID_QID = 0x0101,
	NVME_MAX_QSIZE_EXCEEDED = 0x0102,
	NVME_ACL_EXCEEDED = 0x0103,
	NVME_RESERVED = 0x0104,
	NVME_AER_LIMIT_EXCEEDED = 0x0105,
	NVME_INVALID_FW_SLOT = 0x0106,
	NVME_INVALID_FW_IMAGE = 0x0107,
	NVME_INVALID_IRQ_VECTOR = 0x0108,
	NVME_INVALID_LOG_ID = 0x0109,
	NVME_INVALID_FORMAT = 0x010a,
	NVME_FW_REQ_RESET = 0x010b,
	NVME_INVALID_QUEUE_DEL = 0x010c,
	NVME_FID_NOT_SAVEABLE = 0x010d,
	NVME_FEAT_NOT_CHANGEABLE = 0x010e,
	NVME_FEAT_NOT_NS_SPEC = 0x010f,
	NVME_FW_REQ_SUSYSTEM_RESET = 0x0110,
	NVME_NS_ALREADY_ATTACHED = 0x0118,
	NVME_NS_PRIVATE = 0x0119,
	NVME_NS_NOT_ATTACHED = 0x011A,
	NVME_NS_CTRL_LIST_INVALID = 0x011C,
	NVME_CONFLICTING_ATTRS = 0x0180,
	NVME_INVALID_PROT_INFO = 0x0181,
	NVME_WRITE_TO_RO = 0x0182,
	NVME_CMD_SIZE_LIMIT = 0x0183,
	NVME_ZONE_BOUNDARY_ERROR = 0x01b8,
	NVME_ZONE_FULL = 0x01b9,
	NVME_ZONE_READ_ONLY = 0x01ba,
	NVME_ZONE_OFFLINE = 0x01bb,
	NVME_ZONE_INVALID_WRITE = 0x01bc,
	NVME_ZONE_TOO_MANY_ACTIVE = 0x01bd,
	NVME_ZONE_TOO_MANY_OPEN = 0x01be,
	NVME_ZONE_INVAL_TRANSITION = 0x01bf,
	NVME_WRITE_FAULT = 0x0280,
	NVME_UNRECOVERED_READ = 0x0281,
	NVME_E2E_GUARD_ERROR = 0x0282,
	NVME_E2E_APP_ERROR = 0x0283,
	NVME_E2E_REF_ERROR = 0x0284,
	NVME_CMP_FAILURE = 0x0285,
	NVME_ACCESS_DENIED = 0x0286,
	NVME_DULB = 0x0287,
	NVME_MORE = 0x2000,
	NVME_DNR = 0x4000,
	NVME_NO_COMPLETE = 0xffff,
};

enum NvmeQprio {
	NVME_QPRIO_URGENT = 0x0,
	NVME_QPRIO_HIGH = 0x1,
	NVME_QPRIO_MEDIUM = 0x2,
	NVME_QPRIO_LOW = 0x3
};

enum NvmeDataTransfer {

	NVME_DATA_NONE = 0,

	NVME_DATA_HOST_TO_CONTROLLER = 1,

	NVME_DATA_CONTROLLER_TO_HOST = 2,

	NVME_DATA_BIDIRECTIONAL = 3
};

static inline enum NvmeDataTransfer NvmeOpcGetDataTransfer(unsigned char opc)
{
	return (enum NvmeDataTransfer)(opc & 3);
}

enum NvmeFeat {

	NVME_FEAT_ARBITRATION = 0x01,

	NVME_FEAT_POWER_MANAGEMENT = 0x02,

	NVME_FEAT_LBA_RANGE_TYPE = 0x03,

	NVME_FEAT_TEMPERATURE_THRESHOLD = 0x04,

	NVME_FEAT_ERROR_RECOVERY = 0x05,

	NVME_FEAT_VOLATILE_WRITE_CACHE = 0x06,

	NVME_FEAT_NUMBER_OF_QUEUES = 0x07,

	NVME_FEAT_INTERRUPT_COALESCING = 0x08,

	NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION = 0x09,

	NVME_FEAT_WRITE_ATOMICITY = 0x0A,

	NVME_FEAT_ASYNC_EVENT_CONFIGURATION = 0x0B,

	NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION = 0x0C,

	NVME_FEAT_HOST_MEM_BUFFER = 0x0D,
	NVME_FEAT_TIMESTAMP = 0x0E,

	NVME_FEAT_KEEP_ALIVE_TIMER = 0x0F,

	NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT = 0x10,

	NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG = 0x11,

	NVME_FEAT_SOFTWARE_PROGRESS_MARKER = 0x80,

	NVME_FEAT_HOST_IDENTIFIER = 0x81,
	NVME_FEAT_HOST_RESERVE_MASK = 0x82,
	NVME_FEAT_HOST_RESERVE_PERSIST = 0x83,

};

enum NvmeGetFeatSel {
	NVME_GET_FEA_SEL_CURRENT = 0x0,
	NVME_GET_FEA_SEL_DEFAULT = 0x1,
	NVME_GET_FEA_SEL_SAVED = 0x2,
	NVME_GET_FEA_SEL_SUP_CAPABILITY = 0x3,
};

enum NvmeDsmAttribute {
	NVME_DSM_ATTR_INTEGRAL_READ = 0x1,
	NVME_DSM_ATTR_INTEGRAL_WRITE = 0x2,
	NVME_DSM_ATTR_DEALLOCATE = 0x4,
};

struct NvmePowerState {
	unsigned short mp;

	unsigned char reserved1;

	unsigned char mps : 1;
	unsigned char nops : 1;
	unsigned char reserved2 : 6;

	unsigned int enlat;
	unsigned int exlat;

	unsigned char rrt : 5;
	unsigned char reserved3 : 3;

	unsigned char rrl : 5;
	unsigned char reserved4 : 3;

	unsigned char rwt : 5;
	unsigned char reserved5 : 3;

	unsigned char rwl : 5;
	unsigned char reserved6 : 3;

	unsigned char reserved7[16];
};

enum NvmeIdentifyCns {

	NVME_IDENTIFY_NS = 0x00,

	NVME_IDENTIFY_CTRLR = 0x01,

	NVME_IDENTIFY_ACTIVE_NS_LIST = 0x02,

	NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST = 0x03,

	NVME_IDENTIFY_ALLOCATED_NS_LIST = 0x10,

	NVME_IDENTIFY_NS_ALLOCATED = 0x11,

	NVME_IDENTIFY_NS_ATTACHED_CTRLR_LIST = 0x12,

	NVME_IDENTIFY_CTRLR_LIST = 0x13,

	NVME_IDENTIFY_PRIMARY_CTRLR_CAP = 0x14,

	NVME_IDENTIFY_SECONDARY_CTRLR_LIST = 0x15,

	NVME_IDENTIFY_NS_GRANULARITY_LIST = 0x16,
};

enum NvmfCtrlrModel {

	NVMF_CTRLR_MODEL_DYNAMIC = 0,

	NVMF_CTRLR_MODEL_STATIC = 1,
};

#define NVME_CTRLR_SN_LEN 20
#define NVME_CTRLR_MN_LEN 40
#define NVME_CTRLR_FR_LEN 8

enum NvmeSglsSupported {

	NVME_SGLS_NOT_SUPPORTED = 0,

	NVME_SGLS_SUPPORTED = 1,

	NVME_SGLS_SUPPORTED_DWORD_PCIE_ALIGNED = 2,
};

enum NvmeFlushBroadcast {

	NVME_FLUSH_BROADCAST_NOT_INDICATED = 0,

	NVME_FLUSH_BROADCAST_NOT_SUPPORTED = 2,

	NVME_FLUSH_BROADCAST_SUPPORTED = 3
};
#ifndef _WINDOWS
struct __packed NvmeCtrlrData {
#else
#pragma pack(1)
struct NvmeCtrlrData {
#endif

	unsigned short vid;

	unsigned short ssvid;

	char sn[NVME_CTRLR_SN_LEN];

	char mn[NVME_CTRLR_MN_LEN];

	unsigned char fr[NVME_CTRLR_FR_LEN];

	unsigned char rab;

	unsigned char ieee[3];

	unsigned char cmic;

	unsigned char mdts;

	unsigned short cntlid;

	union NvmeVsReg ver;

	unsigned int rtd3r;

	unsigned int rtd3e;

	unsigned int oaes;

	unsigned int ctratt;

	unsigned char reserved_100[12];

	unsigned char fguid[16];

	unsigned char reserved_128[128];

	unsigned short oacs;

	unsigned char acl;

	unsigned char aerl;

	unsigned char frmw;

	unsigned char lpe;

	unsigned char elpe;

	unsigned char npss;

	unsigned char avscc;

	unsigned char apsta;

	unsigned short wctemp;

	unsigned short cctemp;

	unsigned short mtfa;

	unsigned int hmpre;

	unsigned int hmmin;

	unsigned long long tnvmcap[2];

	unsigned long long unvmcap[2];

	unsigned int rpmbs;

	unsigned short edstt;

	unsigned char dsto;

	unsigned char fwug;

	unsigned short kas;

	unsigned short hctma;

	unsigned short mntmt;

	unsigned short mxtmt;

	union {
		unsigned int sanicap;
		struct {
			unsigned int ces : 1;
			unsigned int bes : 1;
			unsigned int ows : 1;
			unsigned int snicapRsvd1 : 26;
			unsigned int ndi : 1;
			unsigned int nodmmas : 2;
		};
	};

	unsigned char reserved3[180];

	unsigned char sqes;

	unsigned char cqes;

	unsigned short maxcmd;

	unsigned int nn;

	unsigned short oncs;

	unsigned short fuses;

	unsigned char fna;

	unsigned char vwc;

	unsigned short awun;

	unsigned short awupf;

	unsigned char nvscc;

	unsigned char reserved531;

	unsigned short acwu;

	unsigned short reserved534;

	unsigned int sgls;

	unsigned char reserved4[228];

	unsigned char subnqn[256];

	unsigned char reserved5[768];

	struct {
		unsigned int ioccsz;

		unsigned int iorcsz;

		unsigned short icdoff;

		unsigned char ctrattr;

		unsigned char msdbd;

		unsigned char reserved[244];
	};

	struct NvmePowerState psd[32];

	unsigned char vs[1024];
};
#ifdef _WINDOWS
#pragma pack()
#endif

#ifndef _WINDOWS
struct __packed NvmePrimaryCtrlCapabilities {
#else
#pragma pack(1)
struct NvmePrimaryCtrlCapabilities {
#endif

	unsigned short cntlid;

	unsigned short portid;

	unsigned char crt;
	unsigned char reserved[27];

	unsigned int vqfrt;

	unsigned int vqrfa;

	unsigned short vqrfap;

	unsigned short vqprt;

	unsigned short vqfrsm;

	unsigned short vqgran;
	unsigned char reserved1[16];

	unsigned int vifrt;

	unsigned int virfa;

	unsigned short virfap;

	unsigned short viprt;

	unsigned short vifrsm;

	unsigned short vigran;
	unsigned char reserved2[4016];
};

#ifdef _WINDOWS
#pragma pack()
#endif
#ifndef _WINDOWS
struct __packed NvmeSecondaryCtrlEntry {
#else
#pragma pack(1)
struct NvmeSecondaryCtrlEntry {
#endif

	unsigned short scid;

	unsigned short pcid;

	unsigned char scs;
	unsigned char reserved[3];

	unsigned short vfn;

	unsigned short nvq;

	unsigned short nvi;
	unsigned char reserved1[18];
};

#ifdef _WINDOWS
#pragma pack()
#endif
#ifndef _WINDOWS
struct __packed NvmeSecondaryCtrlList {
#else
#pragma pack(1)
struct NvmeSecondaryCtrlList {
#endif

	unsigned char number;
	unsigned char reserved[31];
	struct NvmeSecondaryCtrlEntry entries[127];
};

#ifdef _WINDOWS
#pragma pack()
#endif
struct NvmeNsData {
	unsigned long long nsze;

	unsigned long long ncap;

	unsigned long long nuse;

	unsigned char nsfeat;

	unsigned char nlbaf;
	union {
		struct {
			unsigned char format : 4;
			unsigned char extended : 1;
			unsigned char reserved2 : 3;
		} flbas;
		unsigned char flbasRaw;
	};

	unsigned char mc;

	union {
		struct {
			unsigned char type1 : 1;
			unsigned char type2 : 1;
			unsigned char type3 : 1;
			unsigned char piBeforeMeta : 1;
			unsigned char piAfterMeta : 1;
			unsigned char reserved2 : 3;
		};
		unsigned char rawData;
	} dpc;

	union {
		struct {
			unsigned char type : 3;
			unsigned char isPiBefore : 1;
			unsigned char reserved2 : 4;
		};
		unsigned char rawData;
	} dps;

	unsigned char nmic;

	unsigned char nsrescap;

	unsigned char fpi;

	unsigned char dlfeat;

	unsigned short nawun;

	unsigned short nawupf;

	unsigned short nacwu;

	unsigned short nabsn;

	unsigned short nabo;

	unsigned short nabspf;

	unsigned short noiob;

	unsigned long long nvmcap[2];

	unsigned char reserved64[40];

	unsigned char nguid[16];

	unsigned long long eui64;

	struct {
		unsigned int ms : 16;
		unsigned int lbads : 8;
		unsigned int rp : 2;
		unsigned int reserved6 : 6;
	} lbaf[16];

	unsigned char reserved6[192];

	unsigned char vendorSpecific[3712];
};

enum NvmeDeallocLogicalBlockReadValue {

	NVME_DEALLOC_NOT_REPORTED = 0,

	NVME_DEALLOC_READ_00 = 1,

	NVME_DEALLOC_READ_FF = 2,
};

enum NvmeReservationType {

	NVME_RESERVE_WRITE_EXCLUSIVE = 0x1,

	NVME_RESERVE_EXCLUSIVE_ACCESS = 0x2,

	NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY = 0x3,

	NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY = 0x4,

	NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS = 0x5,

	NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS = 0x6,

};

struct NvmeReservationAcquireData {
	unsigned long long crkey;

	unsigned long long prkey;
};

enum NvmeReservationAcquireAction {
	NVME_RESERVE_ACQUIRE = 0x0,
	NVME_RESERVE_PREEMPT = 0x1,
	NVME_RESERVE_PREEMPT_ABORT = 0x2,
};
#ifndef _WINDOWS
struct __packed NvmeReservationStatusData {
#else
#pragma pack(1)
struct NvmeReservationStatusData {
#endif

	unsigned int gen;

	unsigned char rtype;

	unsigned short regctl;
	unsigned short reserved1;

	unsigned char ptpls;
	unsigned char reserved[14];
};
#ifdef _WINDOWS
#pragma pack()
#endif

#ifndef _WINDOWS
struct __packed NvmeReservationStatusExtendedData {
#else
#pragma pack(1)
struct NvmeReservationStatusExtendedData {
#endif
	struct NvmeReservationStatusData data;
	unsigned char reserved[40];
};

#ifdef _WINDOWS
#pragma pack()
#endif
#ifndef _WINDOWS
struct __packed NvmeRegisteredCtrlrData {
#else
#pragma pack(1)
struct NvmeRegisteredCtrlrData {
#endif

	unsigned short cntlid;
	unsigned char rcsts;
	unsigned char reserved2[5];

	unsigned long long hostid;

	unsigned long long rkey;
};

#ifdef _WINDOWS
#pragma pack()
#endif
#ifndef _WINDOWS
struct __packed NvmeRegisteredCtrlrExtendedData {
#else
#pragma pack(1)
struct NvmeRegisteredCtrlrExtendedData {
#endif

	unsigned short cntlid;

	unsigned char rcsts;
	unsigned char reserved2[5];

	unsigned long long rkey;

	unsigned char hostid[16];
	unsigned char reserved3[32];
};

#ifdef _WINDOWS
#pragma pack()
#endif

enum NvmeReservationRegisterCptpl {
	NVME_RESERVE_PTPL_NO_CHANGES = 0x0,
	NVME_RESERVE_PTPL_CLEAR_POWER_ON = 0x2,
	NVME_RESERVE_PTPL_PERSIST_POWER_LOSS = 0x3,
};

enum NvmeReservationRegisterAction {
	NVME_RESERVE_REGISTER_KEY = 0x0,
	NVME_RESERVE_UNREGISTER_KEY = 0x1,
	NVME_RESERVE_REPLACE_KEY = 0x2,
};

struct NvmeReservationRegisterData {
	unsigned long long crkey;

	unsigned long long nrkey;
};

struct NvmeReservationKeyData {
	unsigned long long crkey;
};

enum NvmeReservationReleaseAction {
	NVME_RESERVE_RELEASE = 0x0,
	NVME_RESERVE_CLEAR = 0x1,
};

enum NvmeReservationNotificationLogPageType {
	NVME_RESERVATION_LOG_PAGE_EMPTY = 0x0,
	NVME_REGISTRATION_PREEMPTED = 0x1,
	NVME_RESERVATION_RELEASED = 0x2,
	NVME_RESERVATION_PREEMPTED = 0x3,
};

struct NvmeReservationNotificationLog {
	unsigned long long logPageCount;

	unsigned char type;

	unsigned char numAvailLogPages;
	unsigned char reserved[2];
	unsigned int nsid;
	unsigned char reserved1[48];
};

#define NVME_REGISTRATION_PREEMPTED_MASK (1U << 1)

#define NVME_RESERVATION_RELEASED_MASK (1U << 2)

#define NVME_RESERVATION_PREEMPTED_MASK (1U << 3)

#define NVME_LOG_PAGE_DW10_SPICE(dw10, lid, logLen)                            \
	((dw10) = ((((logLen) >> 2) - 1) << 16) | (lid))
#define NVME_LOG_PAGE_RETAIN_BIT_SET(dw10) ((dw10) |= 1UL << 15)

enum NvmeLogPage {

	NVME_LOG_ERROR = 0x01,

	NVME_LOG_HEALTH_INFORMATION = 0x02,

	NVME_LOG_FIRMWARE_SLOT = 0x03,

	NVME_LOG_CHANGED_NS_LIST = 0x04,

	NVME_LOG_COMMAND_EFFECTS_LOG = 0x05,

	NVME_LOG_DEVICE_SELF_TEST = 0x06,

	NVME_LOG_TELEMETRY_HOST_INITIATED = 0x07,

	NVME_LOG_TELEMETRY_CTRLR_INITIATED = 0x08,

	NVME_LOG_DISCOVERY = 0x70,

	NVME_LOG_RESERVATION_NOTIFICATION = 0x80,

	NVME_LOG_SANITIZE_STATUS = 0x81,

};

enum {
	NVME_NO_LOG_LSP = 0x0,
	NVME_NO_LOG_LPO = 0x0,
	NVME_LOG_ANA_LSP_RGO = 0x1,
	NVME_TELEM_LSP_CREATE = 0x1,
};

struct NvmeErrorInformationEntry {
	unsigned long long errorCount;
	unsigned short sqid;
	unsigned short cid;
	struct NvmeCmdStatus status;
	unsigned short errorLocation;
	unsigned long long lba;
	unsigned int nsid;
	unsigned char vendorSpecific;
	unsigned char trtype;
	unsigned char reserved30[2];
	unsigned long long commandSpecific;
	unsigned short trtypeSpecific;
	unsigned char reserved42[22];
};

union NvmeCriticalWarningState {
	unsigned char raw;

	struct {
		unsigned char availableSpare : 1, temperature : 1, deviceReliability : 1,
			readOnly : 1, volatileMemoryBackup : 1,
			persistentMemRO : 1, reserved : 2;
	};
};

#ifndef _WINDOWS
struct __packed NvmeHealthInformationPage {
#else
#pragma pack(1)
struct NvmeHealthInformationPage {
#endif
	union NvmeCriticalWarningState criticalWarning;

	unsigned short temperature;
	unsigned char availableSpare;
	unsigned char availableSpareThreshold;
	unsigned char percentageUsed;
	unsigned char enduGrpCritWarnSumry;

	unsigned char reserved[25];

	unsigned long long dataUnitsRead[2];

	unsigned long long dataUnitsWritten[2];

	unsigned long long hostReadCommands[2];
	unsigned long long hostWriteCommands[2];

	unsigned long long controllerBusyTime[2];
	unsigned long long powerCycles[2];
	unsigned long long powerOnHours[2];
	unsigned long long unsafeShutdowns[2];
	unsigned long long mediaErrors[2];
	unsigned long long numErrorInfoLogEntries[2];

	unsigned int warningTempTime;
	unsigned int criticalTempTime;
	unsigned short tempSensor[8];

	unsigned char reserved2[296];
};

#ifdef _WINDOWS
#pragma pack()
#endif

struct NvmeCmdsAndEffectEntry {
	unsigned short csupp : 1;

	unsigned short lbcc : 1;

	unsigned short ncc : 1;

	unsigned short nic : 1;

	unsigned short ccc : 1;

	unsigned short reserved1 : 11;

	unsigned short cse : 3;

	unsigned short reserved2 : 13;
};

struct NvmeCmdsAndEffectLogPage {
	struct NvmeCmdsAndEffectEntry adminCmdsSupported[256];

	struct NvmeCmdsAndEffectEntry ioCmdsSupported[256];

	unsigned char reserved0[2048];
};

#ifndef _WINDOWS
struct __packed NvmeSelfTestResEntry {
#else
#pragma pack(1)
struct NvmeSelfTestResEntry {
#endif
	unsigned char operationRes : 4, selfTestCode : 4;
	unsigned char segmentNum;
	unsigned char nsIdValid : 1, flbaValid : 1, sctValid : 1, scValid : 1,
		reserved1 : 4;
	unsigned char reserved2;
	unsigned long long poh;
	unsigned int nsId;
	unsigned long long failingLba;
	unsigned char sct : 3, reserved3 : 5;
	unsigned char sc;
	unsigned short vendorSpecific;
};
#ifdef _WINDOWS
#pragma pack()
#endif
#ifndef _WINDOWS
struct __packed NvmeSelfTestLogPage {
#else
#pragma pack(1)
struct NvmeSelfTestLogPage {
#endif

	unsigned char currtOpt : 4, reserved1 : 4;
	unsigned char currtComplt : 7, reserved2 : 1;
	unsigned short reserved3;
	struct NvmeSelfTestResEntry newSelfTestRes[20];
};
#ifdef _WINDOWS
#pragma pack()
#endif

struct NvmeTelemetryLogPageHdr {
	unsigned char lpi;
	unsigned char rsvd[4];
	unsigned char ieeeOui[3];

	unsigned short dalb1;

	unsigned short dalb2;

	unsigned short dalb3;
	unsigned char rsvd1[368];

	unsigned char ctrlrAvail;

	unsigned char ctrlrGen;

	unsigned char rsnident[128];
	unsigned char telemetryDatablock[0];
};

enum NvmeSanitizeStatusType {
	NVME_NEVER_BEEN_SANITIZED = 0x0,
	NVME_RECENT_SANITIZE_SUCCESSFUL = 0x1,
	NVME_SANITIZE_IN_PROGRESS = 0x2,
	NVME_SANITIZE_FAILED = 0x3,
};

struct NvmeSanitizeStatusSstat {
	unsigned short status : 3;
	unsigned short completePass : 5;
	unsigned short globalDataErase : 1;
	unsigned short reserved : 7;
};

struct NvmeSanitizeStatusLogPage {
	unsigned short sprog;

	struct NvmeSanitizeStatusSstat sstat;

	unsigned int scdw10;

	unsigned int etOverwrite;

	unsigned int etBlockErase;

	unsigned int etCryptoErase;
	unsigned char reserved[492];
};

enum NvmeAsyncEventType {

	NVME_ASYNC_EVENT_TYPE_ERROR = 0x0,

	NVME_ASYNC_EVENT_TYPE_SMART = 0x1,

	NVME_ASYNC_EVENT_TYPE_NOTICE = 0x2,

	NVME_ASYNC_EVENT_TYPE_IO = 0x6,

	NVME_ASYNC_EVENT_TYPE_VENDOR = 0x7,
};

enum NvmeAsyncEventInfoError {

	NVME_ASYNC_EVENT_WRITE_INVALID_DB = 0x0,

	NVME_ASYNC_EVENT_INVALID_DB_WRITE = 0x1,

	NVME_ASYNC_EVENT_DIAGNOSTIC_FAILURE = 0x2,

	NVME_ASYNC_EVENT_PERSISTENT_INTERNAL = 0x3,

	NVME_ASYNC_EVENT_TRANSIENT_INTERNAL = 0x4,

	NVME_ASYNC_EVENT_FW_IMAGE_LOAD = 0x5,

};

enum NvmeAsyncEventInfoSmart {

	NVME_ASYNC_EVENT_SUBSYSTEM_RELIABILITY = 0x0,

	NVME_ASYNC_EVENT_TEMPERATURE_THRESHOLD = 0x1,

	NVME_ASYNC_EVENT_SPARE_BELOW_THRESHOLD = 0x2,

};

enum NvmeAsyncEventInfoNotice {

	NVME_ASYNC_EVENT_NS_ATTR_CHANGED = 0x0,

	NVME_ASYNC_EVENT_FW_ACTIVATION_START = 0x1,

	NVME_ASYNC_EVENT_TELEMETRY_LOG_CHANGED = 0x2,

};

enum NvmeAsyncEventInfoNvmCommandSet {

	NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL = 0x0,

	NVME_ASYNC_EVENT_SANITIZE_COMPLETED = 0x1,

};

union NvmeAsyncEventCompletion {
	unsigned int raw;
	struct {
		unsigned int asyncEventType : 3;
		unsigned int reserved1 : 5;
		unsigned int asyncEventInfo : 8;
		unsigned int logPageIdentifier : 8;
		unsigned int reserved2 : 8;
	};
};

union NvmeFeatArbitration {
	unsigned int raw;
	struct {
		unsigned int ab : 3;

		unsigned int reserved : 5;

		unsigned int lpw : 8;

		unsigned int mpw : 8;

		unsigned int hpw : 8;
	};
};

union NvmeFeatPowerManagement {
	unsigned int raw;
	struct {
		unsigned int ps : 5;

		unsigned int wh : 3;

		unsigned int reserved : 24;
	};
};

union NvmeFeatLbaRangeType {
	unsigned int raw;
	struct {
		unsigned int num : 6;

		unsigned int reserved : 26;
	};
};

union NvmeFeatTemperatureThreshold {
	unsigned int raw;
	struct {
		unsigned int tmpth : 16;

		unsigned int tmpsel : 4;

		unsigned int thsel : 2;

		unsigned int reserved : 10;
	};
};

union NvmeFeatErrorRecovery {
	unsigned int raw;
	struct {
		unsigned int tler : 16;

		unsigned int dulbe : 1;

		unsigned int reserved : 15;
	};
};

union NvmeFeatVolatileWriteCache {
	unsigned int raw;
	struct {
		unsigned int wce : 1;

		unsigned int reserved : 31;
	};
};

union NvmeFeatNumberOfQueues {
	unsigned int raw;
	struct {
		unsigned int nsqr : 16;

		unsigned int ncqr : 16;
	};
};

union NvmeFeatInterruptCoalescing {
	unsigned int raw;
	struct {
		unsigned int thr : 8;

		unsigned int time : 8;

		unsigned int reserved : 16;
	};
};

union NvmeFeatInterruptVectorConfiguration {
	unsigned int raw;
	struct {
		unsigned int iv : 16;

		unsigned int cd : 1;

		unsigned int reserved : 15;
	};
};

union NvmeFeatWriteAtomicity {
	unsigned int raw;
	struct {
		unsigned int dn : 1;

		unsigned int reserved : 31;
	};
};

union NvmeFeatAsyncEventConfiguration {
	unsigned int raw;
	struct {
		union NvmeCriticalWarningState critWarn;
		unsigned int nsAttrNotice : 1;
		unsigned int fwActivationNotice : 1;
		unsigned int telemetryLogNotice : 1;
		unsigned int reserved : 21;
	};
};

#define NvmeAsyncEventConfig NvmeFeatAsyncEventConfiguration

union NvmeFeatAutonomousPowerStateTransition {
	unsigned int raw;
	struct {
		unsigned int apste : 1;

		unsigned int reserved : 31;
	};
};

union NvmeFeatHostMemBuffer {
	unsigned int raw;
	struct {
		unsigned int ehm : 1;

		unsigned int mr : 1;

		unsigned int reserved : 30;
	};
};

union NvmeFeatKeepAliveTimer {
	unsigned int kato;
};

union NvmeFeatHostControlledThermalManagement {
	unsigned int raw;
	struct {
		unsigned int tmt2 : 16;

		unsigned int tmt1 : 16;
	};
};

union NvmeFeatNonOperationalPowerStateConfig {
	unsigned int raw;
	struct {
		unsigned int noppme : 1;

		unsigned int reserved : 31;
	};
};

union NvmeFeatSoftwareProgressMarker {
	unsigned int raw;
	struct {
		unsigned int pbslc : 8;
		unsigned int reserved : 24;
	};
};

union NvmeFeatHostIdentifier {
	unsigned int raw;
	struct {
		unsigned int exhid : 1;

		unsigned int reserved : 31;
	};
};

struct NvmeFirmwarePage {
	unsigned char afi;

	unsigned char reserved[7];
	unsigned char revision[7][8];
	unsigned char reserved2[448];
};

enum NvmeNsAttachType {

	NVME_NS_CTRLR_ATTACH = 0x0,

	NVME_NS_CTRLR_DETACH = 0x1,

};

enum NvmeNsManagementType {

	NVME_NS_MANAGEMENT_CREATE = 0x0,

	NVME_NS_MANAGEMENT_DELETE = 0x1,

};

struct NvmeNsList {
	unsigned int nsList[1024];
};

enum NvmeNidt {

	NVME_NIDT_EUI64 = 0x01,

	NVME_NIDT_NGUID = 0x02,

	NVME_NIDT_UUID = 0x03,
};

struct NvmeNsIdDesc {
	unsigned char nidt;

	unsigned char nidl;

	unsigned char reserved2;
	unsigned char reserved3;

	unsigned char nid[];
};

struct NvmeCtrlrList {
	unsigned short ctrlrCount;
	unsigned short ctrlrList[2047];
};

enum NvmeSecureEraseSetting {
	NVME_FMT_NVM_SES_NO_SECURE_ERASE = 0x0,
	NVME_FMT_NVM_SES_USER_DATA_ERASE = 0x1,
	NVME_FMT_NVM_SES_CRYPTO_ERASE = 0x2,
};

enum NvmePiLocation {
	NVME_FMT_NVM_PROTECTION_AT_TAIL = 0x0,
	NVME_FMT_NVM_PROTECTION_AT_HEAD = 0x1,
};

enum NvmePiType {
	NVME_FMT_NVM_PROTECTION_DISABLE = 0x0,
	NVME_FMT_NVM_PROTECTION_TYPE1 = 0x1,
	NVME_FMT_NVM_PROTECTION_TYPE2 = 0x2,
	NVME_FMT_NVM_PROTECTION_TYPE3 = 0x3,
};

enum NvmeMetadataSetting {
	NVME_FMT_NVM_METADATA_TRANSFER_AS_BUFFER = 0x0,
	NVME_FMT_NVM_METADATA_TRANSFER_AS_LBA = 0x1,
};

struct NvmeFormat {
	unsigned int lbaf : 4;
	unsigned int ms : 1;
	unsigned int pi : 3;
	unsigned int pil : 1;
	unsigned int ses : 3;
	unsigned int reserved : 20;
};

struct NvmeProtectionInfo {
	unsigned short guard;
	unsigned short appTag;
	unsigned int refTag;
};

enum NvmeFwCommitAction {
	NVME_FW_COMMIT_REPLACE_IMG = 0x0,
	NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG = 0x1,
	NVME_FW_COMMIT_ENABLE_IMG = 0x2,
	NVME_FW_COMMIT_RUN_IMG = 0x3,
};

enum NvmeDeviceSelfTestCode {
	NVME_SHORT_DEVICE_SELF_TEST_OPT = 0x1,
	NVME_EXTENDED_DEVICE_SELF_TEST_OPT = 0x2,
	NVME_ABORT_DEVICE_SELF_TEST_OPT = 0xF,
};

struct NvmeFwCommit {
	unsigned int fs : 3;
	unsigned int ca : 3;
	unsigned int reserved : 26;
};

#define NVME_CPL_IS_SUCCESS(cpl) (!NVME_CMD_STATUS_NOT_SUCCESS(cpl))

#define NVME_CMD_STATUS_NOT_SUCCESS(cpl)                                       \
	((cpl)->status.sc != NVME_SC_SUCCESS ||                                \
	 (cpl)->status.sct != NVME_SCT_GENERIC)

#define NVME_STATUS_CMD_ABORT_REQUEST(cpl)                                     \
	((cpl)->status.sc == NVME_SC_ABORTED_BY_REQUEST &&                     \
	 (cpl)->status.sct == NVME_SCT_GENERIC)

#define NVME_CPL_PI_IS_ERROR(cpl)                                              \
	((cpl)->status.sct == NVME_SCT_MEDIA_ERROR &&                          \
	 ((cpl)->status.sc == NVME_SC_GUARD_CHECK_ERROR ||                     \
	  (cpl)->status.sc == NVME_SC_APPLICATION_TAG_CHECK_ERROR ||           \
	  (cpl)->status.sc == NVME_SC_REFERENCE_TAG_CHECK_ERROR))

#define NVME_IO_FLAGS_PRCHK_REFTAG (1U << 26)

#define NVME_IO_FLAGS_PRCHK_APPTAG (1U << 27)

#define NVME_IO_FLAGS_PRCHK_GUARD (1U << 28)

#define NVME_IO_FLAGS_PRACT (1U << 29)
#define NVME_IO_FLAGS_FORCE_UNIT_ACCESS (1U << 30)
#define NVME_IO_FLAGS_LIMITED_RETRY (1U << 31)

#define NVME_FORMAT_SES_BIT_MASK 0x7UL
#define NVME_FORMAT_SES_BIT_SHIFT 9
#define NVME_LBAF_MASK 0xFUL
#define NVME_FORMAT_SES_TYPE_NONE 0x0
#define NVME_FORMAT_SES_TYPE_USER_DATA_ERASE 0x1
#define NVME_FORMAT_SES_TYPE_CRYPTO_ERASE 0x2
union NvmeFormatCmdDw10 {
	struct {
		unsigned int lbaf : 4;
		unsigned int mset : 1;
		unsigned int pi : 3;
		unsigned int pil : 1;
		unsigned int ses : 1;
		unsigned int reserved : 3;
	};
	unsigned int rawVal;
};

#ifdef __cplusplus
}
#endif

#endif
