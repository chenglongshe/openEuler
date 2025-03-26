/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __S1861_HIL_REG0_PS3_REQUEST_QUEUE_REG_H__
#define __S1861_HIL_REG0_PS3_REQUEST_QUEUE_REG_H__
#include "s1861_global_baseaddr.h"
#ifndef __S1861_HIL_REG0_PS3_REQUEST_QUEUE_REG_MACRO__
#define HIL_REG0_PS3_REQUEST_QUEUE_PS3_REQUEST_QUEUE_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x0)
#define HIL_REG0_PS3_REQUEST_QUEUE_PS3_REQUEST_QUEUE_RST   (0xFFFFFFFFFFFFFFFF)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOERRCNT_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOERRCNT_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x10)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_RST   (0x0000000C1FFF0000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELCONFIG_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x18)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELCONFIG_RST   (0x0000000300000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFORST_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x20)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFORST_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOIOCNT_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x28)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOIOCNT_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOFLOWCNT_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x30)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOFLOWCNT_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_STATUS_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x38)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_STATUS_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_SET_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x40)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_SET_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_CLR_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x48)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_CLR_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_MASK_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x50)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_INT_MASK_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_CNT_CLR_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x58)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_CNT_CLR_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOORDERERROR_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x60)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOORDERERROR_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFODINSHIFT_ADDR(_n)   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x68 + (_n)*0x8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFODINSHIFT_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFODOUTSHIFT_ADDR(_n)   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x88 + (_n)*0x8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFODOUTSHIFT_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_MAXLEVEL_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xa8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_MAXLEVEL_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOINIT_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xb0)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOINIT_RST   (0x0000000000000002)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOINIT_EN_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xb8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOINIT_EN_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOINIT_MAX_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xc0)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOINIT_MAX_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_ECC_CNT_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xc8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_ECC_CNT_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_ECC_ADDR_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xd0)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOSTATUS_ECC_ADDR_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_DECODER_OVERFLOW_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xd8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_DECODER_OVERFLOW_RST   (0x000000000000003F)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_ECC_BAD_PROJECT_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xe0)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFO_ECC_BAD_PROJECT_RST   (0x0000000000000001)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOOVERFLOW_WORD_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xe8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOOVERFLOW_WORD_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORCTL_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xf0)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORCTL_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORCNTCLR_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0xf8)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORCNTCLR_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORLOW_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x100)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORLOW_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORMID_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x108)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORMID_RST   (0x0000000000000000)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORHIGH_ADDR   (HIL_REG0_PS3_REQUEST_QUEUE_BASEADDR + 0x110)
#define HIL_REG0_PS3_REQUEST_QUEUE_FIFOLEVELMONITORHIGH_RST   (0x0000000000000000)
#endif

#ifndef __S1861_HIL_REG0_PS3_REQUEST_QUEUE_REG_STRUCT__
union HilReg0Ps3RequestQueuePs3RequestQueue {

	U64 val;
	struct{

		U64 port                           : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifoErrCnt {

	U64 val;
	struct{

		U64 waddrerr                       : 32;
		U64 reserved1                      : 32;
	} reg;
};

union HilReg0Ps3RequestQueueFifoStatus {

	U64 val;
	struct{

		U64 filled                         : 16;
		U64 fifoDepth                      : 16;
		U64 almostfull                     : 1;
		U64 full                           : 1;
		U64 almostempty                    : 1;
		U64 empty                          : 1;
		U64 reserved6                      : 28;
	} reg;
};

union HilReg0Ps3RequestQueueFifoLevelConfig {

	U64 val;
	struct{

		U64 cfgAempty                      : 16;
		U64 cfgAfull                       : 16;
		U64 emptyProtect                   : 1;
		U64 fullProtect                    : 1;
		U64 reserved4                      : 30;
	} reg;
};

union HilReg0Ps3RequestQueueFifoRst {

	U64 val;
	struct{

		U64 resetPls                       : 1;
		U64 reserved1                      : 63;
	} reg;
};

union HilReg0Ps3RequestQueueFifoIOCnt {

	U64 val;
	struct{

		U64 wr                             : 32;
		U64 rd                             : 32;
	} reg;
};

union HilReg0Ps3RequestQueueFifoFlowCnt {

	U64 val;
	struct{

		U64 overflow                       : 32;
		U64 underflow                      : 32;
	} reg;
};

union HilReg0Ps3RequestQueueFifoIntStatus {

	U64 val;
	struct{

		U64 overflowStatus                 : 1;
		U64 underflowStatus                : 1;
		U64 nemptyStatus                   : 1;
		U64 eccBadStatus                   : 1;
		U64 reserved4                      : 60;
	} reg;
};

union HilReg0Ps3RequestQueueFifoIntSet {

	U64 val;
	struct{

		U64 overflowSet                    : 1;
		U64 underflowSet                   : 1;
		U64 nemptySet                      : 1;
		U64 eccBadSet                      : 1;
		U64 reserved4                      : 60;
	} reg;
};

union HilReg0Ps3RequestQueueFifoIntClr {

	U64 val;
	struct{

		U64 overflowClr                    : 1;
		U64 underflowClr                   : 1;
		U64 nemptyClr                      : 1;
		U64 eccBadClr                      : 1;
		U64 reserved4                      : 60;
	} reg;
};

union HilReg0Ps3RequestQueueFifoIntMask {

	U64 val;
	struct{

		U64 overflowMask                   : 1;
		U64 underflowMask                  : 1;
		U64 nemptyMask                     : 1;
		U64 eccBadMask                     : 1;
		U64 reserved4                      : 60;
	} reg;
};

union HilReg0Ps3RequestQueueFifoCntClr {

	U64 val;
	struct{

		U64 fifowrcntClr                   : 1;
		U64 fifordcntClr                   : 1;
		U64 fifoerrcntClr                  : 1;
		U64 fifoordererrwrcntClr           : 1;
		U64 fifoordererrrdcntClr           : 1;
		U64 fifobit1errcntClr              : 1;
		U64 fifobit2errcntClr              : 1;
		U64 reserved7                      : 57;
	} reg;
};

union HilReg0Ps3RequestQueueFifoOrderError {

	U64 val;
	struct{

		U64 wrcnt                          : 32;
		U64 rdcnt                          : 32;
	} reg;
};

union HilReg0Ps3RequestQueueFifoDinShift {

	U64 val;
	struct{

		U64 val                            : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifoDoutShift {

	U64 val;
	struct{

		U64 val                            : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifostatusMaxlevel {

	U64 val;
	struct{

		U64 val                            : 16;
		U64 reserved1                      : 48;
	} reg;
};

union HilReg0Ps3RequestQueueFifoInit {

	U64 val;
	struct{

		U64 stat                           : 2;
		U64 reserved1                      : 62;
	} reg;
};

union HilReg0Ps3RequestQueueFifoinitEn {

	U64 val;
	struct{

		U64 start                          : 1;
		U64 reserved1                      : 63;
	} reg;
};

union HilReg0Ps3RequestQueueFifoinitMax {

	U64 val;
	struct{

		U64 num                            : 16;
		U64 reserved1                      : 48;
	} reg;
};

union HilReg0Ps3RequestQueueFifostatusEccCnt {

	U64 val;
	struct{

		U64 bit1Err                        : 32;
		U64 bit2Err                        : 32;
	} reg;
};

union HilReg0Ps3RequestQueueFifostatusEccAddr {

	U64 val;
	struct{

		U64 errPoint                       : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifoDecoderOverflow {

	U64 val;
	struct{

		U64 rCmdwordEmpty                  : 1;
		U64 rPortindexEmpty                : 1;
		U64 rCmdbackEmpty                  : 1;
		U64 wCmdwordEmpty                  : 1;
		U64 wPortindexEmpty                : 1;
		U64 wCmdbackEmpty                  : 1;
		U64 rCmdwordFull                   : 1;
		U64 rPortindexFull                 : 1;
		U64 rCmdbackFull                   : 1;
		U64 wCmdwordFull                   : 1;
		U64 wPortindexFull                 : 1;
		U64 wCmdbackFull                   : 1;
		U64 reserved12                     : 52;
	} reg;
};

union HilReg0Ps3RequestQueueFifoEccBadProject {

	U64 val;
	struct{

		U64 en                             : 1;
		U64 reserved1                      : 63;
	} reg;
};

union HilReg0Ps3RequestQueueFifooverflowWord {

	U64 val;
	struct{

		U64 record                         : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifoLevelMonitorCtl {

	U64 val;
	struct{

		U64 low                            : 16;
		U64 high                           : 16;
		U64 en                             : 1;
		U64 reserved3                      : 31;
	} reg;
};

union HilReg0Ps3RequestQueueFifoLevelMonitorCntClr {

	U64 val;
	struct{

		U64 en                             : 1;
		U64 reserved1                      : 63;
	} reg;
};

union HilReg0Ps3RequestQueueFifoLevelMonitorLow {

	U64 val;
	struct{

		U64 cnt                            : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifoLevelMonitorMid {

	U64 val;
	struct{

		U64 cnt                            : 64;
	} reg;
};

union HilReg0Ps3RequestQueueFifoLevelMonitorHigh {

	U64 val;
	struct{

		U64 cnt                            : 64;
	} reg;
};

struct HilReg0Ps3RequestQueue {

	union HilReg0Ps3RequestQueuePs3RequestQueue    ps3RequestQueue;
	union HilReg0Ps3RequestQueueFifoErrCnt         fifoErrCnt;
	union HilReg0Ps3RequestQueueFifoStatus         fifoStatus;
	union HilReg0Ps3RequestQueueFifoLevelConfig    fifoLevelConfig;
	union HilReg0Ps3RequestQueueFifoRst            fifoRst;
	union HilReg0Ps3RequestQueueFifoIOCnt          fifoIOCnt;
	union HilReg0Ps3RequestQueueFifoFlowCnt        fifoFlowCnt;
	union HilReg0Ps3RequestQueueFifoIntStatus      fifoIntStatus;
	union HilReg0Ps3RequestQueueFifoIntSet         fifoIntSet;
	union HilReg0Ps3RequestQueueFifoIntClr         fifoIntClr;
	union HilReg0Ps3RequestQueueFifoIntMask        fifoIntMask;
	union HilReg0Ps3RequestQueueFifoCntClr         fifoCntClr;
	union HilReg0Ps3RequestQueueFifoOrderError     fifoOrderError;
	union HilReg0Ps3RequestQueueFifoDinShift       fifoDinShift[4];
	union HilReg0Ps3RequestQueueFifoDoutShift      fifoDoutShift[4];
	union HilReg0Ps3RequestQueueFifostatusMaxlevel   fifoStatusMaxLevel;
	union HilReg0Ps3RequestQueueFifoInit           fifoInit;
	union HilReg0Ps3RequestQueueFifoinitEn         fifoinitEn;
	union HilReg0Ps3RequestQueueFifoinitMax        fifoinitMax;
	union HilReg0Ps3RequestQueueFifostatusEccCnt   fifoStatusEccCnt;
	union HilReg0Ps3RequestQueueFifostatusEccAddr   fifoStatusEccAddr;
	union HilReg0Ps3RequestQueueFifoDecoderOverflow   fifoDecoderOverflow;
	union HilReg0Ps3RequestQueueFifoEccBadProject   fifoEccBadProject;
	union HilReg0Ps3RequestQueueFifooverflowWord   fifoOverFlowWord;
	union HilReg0Ps3RequestQueueFifoLevelMonitorCtl   fifoLevelMonitorCtl;
	union HilReg0Ps3RequestQueueFifoLevelMonitorCntClr   fifoLevelMonitorCntClr;
	union HilReg0Ps3RequestQueueFifoLevelMonitorLow   fifoLevelMonitorLow;
	union HilReg0Ps3RequestQueueFifoLevelMonitorMid   fifoLevelMonitorMid;
	union HilReg0Ps3RequestQueueFifoLevelMonitorHigh   fifoLevelMonitorHigh;
};
#endif
#endif
