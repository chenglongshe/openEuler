/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PS3_MGR_EVT__
#define __PS3_MGR_EVT__

#include "ps3_mgr_evt_raidhba.h"
#include "ps3_mgr_evt_swexp.h"
#include "ps3lib/ps3lib_event.h"

#pragma pack(1)

#if (defined(PS3_PRODUCT_EXPANDER) || defined(PS3_PRODUCT_SWITCH))
#define PS3_EVT_ATTR(evtcode) (PS3_EVT_ATTR_EXTEND(evtcode))
#else
#define PS3_EVT_ATTR(evtcode)                                                  \
	(ps3EvtCodeExtendToNormal(PS3_EVT_ATTR_EXTEND(evtcode)))
#endif

#define MAX_VD_NAME_BYTES (16)

enum {
	MGR_EVT_FUNCTION0 = 0,
	MGR_EVT_FUNCTION1 = 1,
	MGR_EVT_FUNCTION_COMMON = 250,
};

#define MGR_CTRL_AUTOCONFIG_EVTDATA_SIZE PS3LIB_CTRL_AUTOCONFIG_EVTDATA_SIZE

#define FGI_MODE_LEN PS3LIB_FGI_MODE_LEN

#define BBM_ERRTBL_NAME_LEN PS3LIB_BBM_ERRTBL_NAME_LEN

enum Ps3HardResetEvtMoudle {
	HARD_RESET_BY_HOST = 0,
	HARD_RESET_BY_BACKEND,
	HARD_RESET_BY_FRONTEND,
};

enum SporStatus {
	SPOR_DUMP_SUCCESS,
	SPOR_DUMP_RUNNING,
	SPOR_DUMP_NVSRAM_INIT_FAILED,
	SPOR_DUMP_NVSRAM_WR_FAILED,
	SPOR_DUMP_ONF_INIT_FAILED,
	SPOR_DUMP_MM_INIT_FAILED,
	SPOR_DUMP_CM_INIT_FAILED,
	SPOR_DUMP_FAILED,
	SPOR_LOAD_SUCCESS,
	SPOR_LOAD_NVSRAM_INIT_FAILED,
	SPOR_LOAD_NVSRAM_WR_FAILED,
	SPOR_LOAD_ONF_INIT_FAILED,
	SPOR_LOAD_MM_INIT_FAILED,
	SPOR_LOAD_CM_INIT_FAILED,
	SPOR_LOAD_FAILED,
	SPOR_STATUS_NULL = 0xFF,
};

#pragma pack()

enum MgrEventModule {
	PS3_EVT_HOST_DRV_X2 = 1,
	PS3_EVT_HOST_DRV_X16,
	PS3_EVT_HOST_DRV_VD_X2,
	PS3_EVT_HOST_DRV_VD_X16,
	PS3_EVT_METADATA,
	PS3_EVT_DEV_MANAGE,
	PS3_EVT_IOC_ALARM_PWM,
	PS3_EVT_IOC_ALARM_LED,
	PS3_EVT_IOC_ALARM,
	PS3_EVT_IOC_MGR,
	PS3_EVT_HOST_SIM,
	PS3_EVT_HOST_SIM_X2,
	PS3_EVT_HOST_SIM_X16,
	PS3_EVT_MODULE_MAX,
};

struct PS3EventFilter {
	unsigned char eventType;
	unsigned char eventCodeCnt;
	unsigned char reserved[6];
	unsigned short eventCodeTable[0];
};

int mgrEvtCancleSubscribe(unsigned char moduleId);

int mgrDrvEvtWebSubsCancel(unsigned char funcType);

#endif
