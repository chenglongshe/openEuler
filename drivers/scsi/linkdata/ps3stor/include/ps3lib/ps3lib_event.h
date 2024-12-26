/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PS3LIB_EVENT_H__
#define __PS3LIB_EVENT_H__

#if defined(__cplusplus)
extern "C" {
#endif

#define PS3LIB_MAX_VD_NAME_BYTES (16)
#define PS3LIB_CTRL_AUTOCONFIG_EVTDATA_SIZE (8)
#define PS3LIB_BBM_ERRTBL_NAME_LEN (6)
#define PS3LIB_EVT_DESC_MAX_LEN (4096)
#define PS3LIB_FGI_MODE_LEN (5)
#define PS3LIB_EVT_LOG_INFO_MAX_SIZE (116)
#define PS3LIB_EXP_EVENT_DATA_COLLECT_MAX_NUM (256)
#define PS3LIB_MAX_EVENT_REG_CNT (128)

enum {
	PS3LIB_EVT_LOG_OLDEST,
	PS3LIB_EVT_LOG_LATEST,
	PS3LIB_EVT_LOG_LAST_CLEAR,
	PS3LIB_EVT_LOG_LAST_REBOOT,
	PS3LIB_EVT_LOG_LAST_SHUTDOWN,
	PS3LIB_EVT_LOG_FATAL_OLDEST,
	PS3LIB_EVT_LOG_FATAL_LATEST,
	PS3LIB_EVT_LOG_LAST_MAX,
};

enum Ps3LibEpEventLevel {
	PS3LIB_EVT_CLASS_UNKNOWN = 0b0000,
	PS3LIB_EVT_CLASS_DEBUG = 0b0011,
	PS3LIB_EVT_CLASS_PROCESS = 0b0101,
	PS3LIB_EVT_CLASS_INFO = 0b0001,
	PS3LIB_EVT_CLASS_WARNING = 0b0010,
	PS3LIB_EVT_CLASS_CRITICAL = 0b0100,
	PS3LIB_EVT_CLASS_FATAL = 0b1000,
	PS3LIB_EVT_CLASS_MAX,
};

enum {
	PS3LIB_CTRL_EVT_SAS_INFO_LOCAL = 1,
	PS3LIB_CTRL_EVT_PD_COUNT_LOCAL = 2,
	PS3LIB_CTRL_EVT_VD_COUNT_LOCAL = 3,
	PS3LIB_CTRL_EVT_CTRL_INFO_LOCAL = 4,
	PS3LIB_CTRL_EVT_PD_ATTR_LOCAL = 5,
	PS3LIB_CTRL_EVT_VD_ATTR_LOCAL = 6,
	PS3LIB_CTRL_EVT_DG_INFO_LOCAL = 7,
	PS3LIB_CTRL_EVT_BBU_INFO_LOCAL = 8,
	PS3LIB_CTRL_EVT_CONFIG_LOCAL = 9,
	PS3LIB_CTRL_EVT_IO_INFO_LOCAL = 10,
	PS3LIB_CTRL_EVT_UKEY_INFO_LOCAL = 11,
	PS3LIB_CTRL_EVT_HWR_INFO_LOCAL = 12,
	PS3LIB_CTRL_EVT_ALARM_INFO_LOCAL = 13,
	PS3LIB_CTRL_EVT_ECC_INFO_LOCAL = 14,
	PS3LIB_CTRL_EVT_UPGRADE_INFO_LOCAL = 15,
	PS3LIB_CTRL_EVT_TEMP_INFO_LOCAL = 16,
	PS3LIB_CTRL_EVT_PD_ATTR_EXTEND_LOCAL = 17,
	PS3LIB_CTRL_EVT_DEFAULT_UNUSED_LOCAL,
	PS3LIB_CTRL_EVT_MAX_TYPE_LOCAL,
};

struct Ps3LibEvtLogRdEntry {
	unsigned int loopCnt;
	unsigned int seqNum;
	unsigned int offset;
	unsigned int timeStampBySec;
	unsigned int size;
};

struct Ps3LibEvtLogRdInfo {
	struct Ps3LibEvtLogRdEntry persistInfo[PS3LIB_EVT_LOG_LAST_MAX];
};

struct Ps3LibEvtPersistInfo {
	unsigned int regionSz[2];
	struct Ps3LibEvtLogRdInfo persist;
};

struct Ps3LibEvtLogHeader {
	unsigned int magic;
	unsigned int seqNum;
	unsigned int size : 8;
	unsigned int funcType : 2;
	unsigned int conFlag : 1;
	unsigned int evtCode : 12;
	unsigned int level : 4;
	unsigned int type : 5;
	unsigned int timeStampBySec;
};

#pragma pack(1)

struct Ps3LibPdAttrInfo {
	unsigned int checkSum : 8;
	unsigned int enclosureId : 8;
	unsigned int phyId : 8;
	unsigned int evtVersion : 8;
	unsigned short phyDiskID;
	unsigned short softChan : 4;
	unsigned short devID : 12;
	unsigned short slotId;
	unsigned short oldState : 4;
	unsigned short newState : 4;
	unsigned short isEnclPd : 1;
	unsigned short longFault : 1;
	unsigned short reason : 6;
	unsigned short arrayId : 8;
	unsigned short rowId : 8;
	unsigned short prevState : 8;
	unsigned short curState : 8;
	unsigned long long sasAddr;
};

struct Ps3LibSparePdInfo {
	struct Ps3LibPdAttrInfo baseInfo;
	unsigned char dedicatedDgCnt;
	unsigned char reserved[3];
	unsigned short dedicatedDgId[8];
};

struct Ps3LibVdAttrInfo {
	unsigned int magicNum;
	unsigned short virtDiskID;
	unsigned short softChan : 4, devID : 12;
	unsigned short diskGrpId;
	unsigned short locked : 1, pad : 15;
};

struct Ps3LibDiskPFCfgModifyEvtInfo {
	unsigned char modifyCfgDataType;
	unsigned char funcIsEnable : 1;
	unsigned char pad : 7;
	unsigned short preFailPollTimeMin;
};

struct Ps3LibVdBaseSetting {
	unsigned int accessPolicy : 2, hidden : 1, defaultWriteCachePolicy : 2,
		currentWriteCachePolicy : 1, defaultReadCachePolicy : 1,
		currentReadCachePolicy : 1, diskCachePolicy : 2, ioPolicy : 1,
		noBgi : 1, emulationType : 2, unmap : 1, cbSize : 2, cbMode : 3,
		encryption : 1, rebootNoVerify : 1, rsv : 10;
	unsigned char vdName[PS3LIB_MAX_VD_NAME_BYTES];
	unsigned long long size;
};

struct Ps3LibVdPropertiesInfo {
	struct Ps3LibVdAttrInfo baseInfo;
	struct Ps3LibVdBaseSetting oldSetting;
	struct Ps3LibVdBaseSetting newSetting;
};

struct Ps3LibVdStateChangeInfo {
	struct Ps3LibVdAttrInfo baseInfo;
	unsigned char oldVdState;
	unsigned char newVdState;
	unsigned char reserved[2];
};

struct Ps3LibVdCreateEvtInfo {
	struct Ps3LibVdAttrInfo baseInfo;
	struct Ps3LibVdBaseSetting setting;
};

struct Ps3LibCtrlAttrInfo {
	unsigned int supportUnevenSpans : 1;
	unsigned int supportJbodSecure : 1;
	unsigned int supportCrashDump : 1;
	unsigned int supportNvmePassthru : 1;
	unsigned int supportDirectCmd : 1;
	unsigned int supportAcceleration : 1;
	unsigned int supportNcq : 1;
	unsigned int reserved1 : 25;
	unsigned int reserved2[1];
	unsigned long long oldSysTime;
	unsigned long long newSysTIme;
	unsigned long long monoSysTime;
	unsigned int newSysTimeYear;
	unsigned int newSysTimeMon;
	unsigned int newSysTimeDay;
	unsigned int newSysTimeHour;
	unsigned int newSysTimeMin;
	unsigned int newSysTimeSec;
	unsigned long long cfgNum;
	unsigned char *pValue;
	unsigned int len;
};

struct Ps3LibCtrlRebootInfo {
	unsigned short ctrlBootMode;
	unsigned short ctrlShutDownReason;
	unsigned int regBootValue;
};

struct Ps3LibDgAttrInfo {
	unsigned short dgId;
	unsigned short reserved[3];
};

struct Ps3LibExpanderInfo {
	unsigned char EnclId;
	unsigned char port;
	unsigned char reserved[6];
};

struct Ps3LibCfgAttrInfo {
	unsigned short profileId;
	unsigned char reserved[2];
};

struct Ps3LibCfgAutoConfig {
	char cfgName[PS3LIB_CTRL_AUTOCONFIG_EVTDATA_SIZE];
};

struct Ps3LibCtrlPowerMode {
	unsigned char mode;
	unsigned char rsv[3];
};

struct Ps3LibBgtRebuildInfo {
	unsigned short newPDFlatId;
	unsigned short newEnclosureId;
	unsigned short newSlotId;
	unsigned short oldPDFlatId;
	unsigned short oldEnclosureId;
	unsigned short oldSlotId;
	unsigned short virtDiskId;
	unsigned short devId;
	unsigned int remainSecs;
	unsigned long long errorPba;
	unsigned long long errorLba;
	unsigned char progressPercent;
	unsigned char rebuildRate;
	unsigned char enableMoveback;
	unsigned char autoRebuild;
	unsigned char eghs;
	unsigned char enablePdm;
	unsigned char pdmSupportReadyPd;
	unsigned char reserved[1];
	unsigned int pdmTimerInterval;
};

struct Ps3LibBgtInitEvtInfo {
	unsigned int aliveSec;
	unsigned int progressRate;
	unsigned short dgId;
	unsigned short virtDiskId;
	unsigned short pdFlatId;
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned char cpuRate;
	char mode[PS3LIB_FGI_MODE_LEN];
	unsigned long long mediumErrLba;
	unsigned long long mediumErrPba;
	unsigned short MediumErrPdFlatId;
	unsigned short softChan : 4;
	unsigned short devID : 12;
};

struct Ps3LibBgtEraseEvtInfo {
	unsigned int aliveSec;
	unsigned int progressRate;
	unsigned short dgId;
	unsigned short virtDiskId;
	unsigned short pdFlatId;
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned short devId;
};

struct Ps3LibBgtCcEvtInfo {
	unsigned int aliveSecs;
	unsigned char progressPercent;
	unsigned char ccRate;
	unsigned char mode;
	unsigned char resered;
	unsigned short virtDiskID;
	unsigned short diskGroupID;
	unsigned int inconsistStrip;
	unsigned short devId;
	unsigned short faultDiskID;
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned long long pdErrLba;
	unsigned long long vdErrLba;
};

struct Ps3LibBgtPrEvtInfo {
	unsigned int aliveSecs;
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned char progressPercent;
	unsigned char prRate;
	unsigned short virtDiskID;
	unsigned short pdFlatId;
	unsigned short diskGroupID;
	unsigned short dgStatus;
	unsigned char reserved[2];
	unsigned long long errLba;
};

struct Ps3LibPhyEvtInfo {
	unsigned int enclosureId : 8, slotId : 8, phyId : 8, reason : 8;
};

struct Ps3LibVdBbmEvtInfo {
	unsigned long long lba;
	unsigned long long pba;
	unsigned short lbaLen;
	unsigned short dgId;
	unsigned short virtDiskId;
	unsigned short percentErrTbl;
	unsigned short devId;
	char errTblName[PS3LIB_BBM_ERRTBL_NAME_LEN];
	unsigned short pdFlatId;
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned short reserved;
};

struct Ps3LibRwDdtEvtInfo {
	unsigned short virtDiskID;
	unsigned short diskGroupID;
	unsigned int vdLen;
	unsigned long long vdLba;
};

struct Ps3LibFlushEvtInfo {
	unsigned short opcode;
	unsigned short dgId;
	unsigned short virtDiskId;
	unsigned short minVdId;
	unsigned short devId;
	unsigned char reserved[6];
	unsigned long long vdIdMap[3];
};

struct Ps3LibMigrationInfo {
	unsigned char migrRate;
	unsigned char resv;
	unsigned short diskGroupID;
	unsigned int percent;
	unsigned int aliveSec;
	unsigned short currVdId;
	unsigned char reserved[2];
};

struct Ps3LibVdBbmBatchEvtInfo {
	unsigned int count;
	struct Ps3LibVdBbmEvtInfo vdBbmEvtInfo[0];
};

struct Ps3LibVdBatchEvtInfo {
	unsigned int count;
	struct Ps3LibVdAttrInfo vdInfo[0];
};

struct Ps3LibPdBatchEvtInfo {
	unsigned int count;
	struct Ps3LibPdAttrInfo pdInfo[0];
};

struct Ps3LibCtrlBatchEvtInfo {
	unsigned int count;
	struct Ps3LibCtrlAttrInfo ctrlInfo[0];
};

struct Ps3libBatchEvtInfoCommon {
	unsigned int count;
	char batchInfo[0];
};

struct Ps3LibBbuEvtInfo {
	unsigned char absent : 1;
	unsigned char overTemp : 1;
	unsigned char overVol : 1;
	unsigned char overCur : 1;
	unsigned char overLoad : 1;
	unsigned char lifeisOver : 1;
	unsigned char reserved : 2;
	unsigned char status;
	unsigned char chargeStatus;
	unsigned char learnStage;
	short batTemperature;
	unsigned short batVoltage;
	short batCurrent;
	unsigned char reserved1[2];
};

struct Ps3LibUkeyEvtInfo {
	unsigned char ukeyStatus;
	unsigned char reserved[3];
};

struct Ps3LibExpEvtInfo {
	unsigned long long expanderSasAddr;
	unsigned long long attachedSasAddr;
	unsigned char phyId[8];
};

struct Ps3LibEccEvtInfo {
	unsigned int eccErrObj;
	unsigned int eccSingleBitCntInc;
	unsigned char eccErrCntThreshold;
	unsigned char pad[3];
	unsigned short eccClearPeriod;
	unsigned char eccType;
	unsigned char eccErrSubObj;
	unsigned long long eccMutilErrAddr;
	unsigned int eccEvtVersion;
};

struct Ps3LibTempEvtInfo {
	unsigned int tempType;
	int tempErrThreshold[4];
	int temperature;
};

struct Ps3LibIoCmdType {
	unsigned char cmdType;
	unsigned char rsv[9];
};

struct Ps3LibDeviceResetEvtInfo {
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned short phyDiskID;
	unsigned short resetType;
	unsigned long long sasAddress;
};

struct Ps3LibSenseDataEvtInfo {
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned short phyDiskID;
	union {
		unsigned char cdb[10];
		struct Ps3LibIoCmdType ioCmdType;
	};
	unsigned char ioFormat;
	unsigned char palErr;
	unsigned char dataPre;
	unsigned char scsiStatus;
	unsigned char skStatus;
	unsigned char sk;
	unsigned char asc;
	unsigned char ascq;
	unsigned long long path;
};

struct Ps3LibErrSenseEvtInfo {
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned short phyDiskID;
	unsigned char CDBLen;
	unsigned char senseLen;
	unsigned char cdb[32];
	unsigned char senseData[56];
	unsigned char sk;
	unsigned char asc;
	unsigned char ascq;
	unsigned char resv;
	unsigned long long path;
};

struct Ps3LibPdDownloadInfo {
	unsigned int downloadMode;
	int isSuccess;
	unsigned short phyDiskID;
	unsigned short softChan : 4;
	unsigned short devID : 12;
	unsigned short enclosureId;
	unsigned short slotId;
};

struct Ps3LibSanitizeEvtInfo {
	struct Ps3LibPdAttrInfo baseInfo;
	unsigned int aliveSecs;
	unsigned char progressPercent;
	unsigned char pad[3];
};

struct Ps3LibFormatEvtInfo {
	struct Ps3LibPdAttrInfo baseInfo;
	unsigned int aliveSecs;
	unsigned char progressPercent;
	unsigned char pad[3];
};

struct Ps3LibSnapshotEvtInfo {
	unsigned char snapCount;
	unsigned char pad[3];
};

struct Ps3LibPdPreFailInfo {
	unsigned int checkSum : 8;
	unsigned int oldState : 4;
	unsigned int newState : 4;
	unsigned int diskType : 4;
	unsigned int pad : 12;
	unsigned short phyDiskID;
	unsigned short softChan : 4;
	unsigned short devID : 12;
	unsigned short enclosureId;
	unsigned short slotId;
	unsigned int historyErrBitMap;
	unsigned int errBitMap;
	char vendor[8];
	char diskSerialNum[24];
};

struct Ps3LibNvDataInvaildInfo {
	unsigned int nvDataIDBitMap[16];
	unsigned short bitmapSize;
	unsigned short invaildCount;
};

struct Ps3LibSpeedNegoInfo {
	unsigned short enclosureId;
	unsigned short slotId;
	char isPcie;
	char speed;
	unsigned char width;
	unsigned char type;
};

struct Ps3LibInitFailInfo {
	unsigned long long errCode;
	unsigned char initFailCnt;
	unsigned char pad[3];
};

struct Ps3LibOemInfo {
	char oemData[PS3LIB_EVT_LOG_INFO_MAX_SIZE];
};

struct Ps3LibBplaneEvtInfo {
	char bplaneData[PS3LIB_EVT_LOG_INFO_MAX_SIZE];
};

struct Ps3LibTriModeInfo {
	unsigned char connectorId;
	unsigned char subConnectorId;
	unsigned char curMode;
	unsigned char rev;
};

struct Ps3LibPhyChgInfo {
	unsigned char portId;
	unsigned char phyId;
	unsigned char reason;
	unsigned char rev;
};

struct Ps3LibPhyInquiryInfo {
	unsigned int pdId;
	unsigned short slotId;
	unsigned short enclosureId;
	char vendor[9];
	char diskModelNum[41];
	char diskSerialNum[25];
	unsigned char isEnclPd : 1;
	unsigned char rev : 7;
	unsigned short sectorSize;
	unsigned short res;
	unsigned long long physicalSize;
};

struct Ps3LibSmpFailInfo {
	unsigned char enclId;
	unsigned char function;
	unsigned char rev[2];
	unsigned int code;
};

union Ps3LibNvmeEvtInfo {
	struct {
		unsigned char sqe[64];
		unsigned long long path;
		unsigned short enclosureId;
		unsigned short slotId;
		unsigned short phyDiskId;
		unsigned short sf;
		unsigned int cmdSpecfic;
		unsigned int type : 8;
		unsigned int isAdminCmd : 1;
		unsigned int rsvd : 23;
	};
};

struct Ps3LibSasSataLinkSpeedMatchInfo {
	unsigned char channelId;
	unsigned char lPhyId;
	unsigned char linkSpeed;
	unsigned char pad1;
	unsigned int pad2;
};

struct Ps3LibSasSataLNExceptionInfo {
	unsigned char channelId;
	unsigned char lPhyId;
	unsigned short pad1;
	unsigned int type;
	unsigned int status;
	unsigned int pad2;
};

struct Ps3LibSasSataDriverInfo {
	unsigned char channelId;
	unsigned char lPhyId;
	unsigned short pad1;
	unsigned int type;
	unsigned int status;
	unsigned int pad2;
};

union Ps3LibReportEvtData {
	struct Ps3LibPdAttrInfo pdInfo;
	struct Ps3LibSparePdInfo sparePdInfo;
	struct Ps3LibVdAttrInfo vdInfo;
	struct Ps3LibVdPropertiesInfo vdChange;
	struct Ps3LibVdStateChangeInfo vdStateChangeInfo;
	struct Ps3LibVdCreateEvtInfo vdCreate;
	struct Ps3LibCtrlAttrInfo ctrlInfo;
	struct Ps3LibCtrlRebootInfo ctrlRebootInfo;
	struct Ps3LibDgAttrInfo dgInfo;
	struct Ps3LibExpanderInfo expanderInfo;
	struct Ps3LibCfgAttrInfo cfgInfo;
	struct Ps3LibCfgAutoConfig autoConfigInfo;

	struct Ps3LibBgtRebuildInfo bgtRebuildInfo;
	struct Ps3LibBgtInitEvtInfo bgtInitEvtInfo;
	struct Ps3LibBgtEraseEvtInfo bgtEraseEvtInfo;
	struct Ps3LibBgtCcEvtInfo bgtCcInfo;

	struct Ps3LibBgtPrEvtInfo bgtPrInfo;

	struct Ps3LibPhyEvtInfo phyInfo;

	struct Ps3LibVdBbmEvtInfo vdBbmEvtInfo;
	struct Ps3LibRwDdtEvtInfo dataVdInfo;

	struct Ps3LibFlushEvtInfo flushEvtInfo;

	struct Ps3LibMigrationInfo bgtMigrInfo;

	struct Ps3LibVdBatchEvtInfo batchVdInfo;
	struct Ps3LibPdBatchEvtInfo batchPdInfo;
	struct Ps3LibCtrlBatchEvtInfo batchCtrlInfo;
	struct Ps3LibVdBbmBatchEvtInfo batchBbmInfo;
	struct Ps3libBatchEvtInfoCommon *pBatchCommonInfo;
	struct Ps3LibBbuEvtInfo bbuEvtInfo;
	struct Ps3LibUkeyEvtInfo ukeyInfo;

	struct Ps3LibExpEvtInfo expEvtInfo;
	struct Ps3LibOemInfo oemEvtInfo;
	struct Ps3LibBplaneEvtInfo bplaneEvtInfo;
	struct Ps3LibEccEvtInfo eccEvtInfo;
	struct Ps3LibTempEvtInfo tempEvtInfo;
	struct Ps3LibDeviceResetEvtInfo deviceResetEvtInfo;
	struct Ps3LibSenseDataEvtInfo senseDataEvtInfo;
	struct Ps3LibErrSenseEvtInfo errSenseEvtInfo;
	struct Ps3LibPdDownloadInfo pdDldEvtInfo;
	struct Ps3LibSanitizeEvtInfo sanitizeInfo;
	struct Ps3LibFormatEvtInfo formatInfo;
	struct Ps3LibSnapshotEvtInfo snapShotInfo;
	struct Ps3LibPdPreFailInfo pdPrefailInfo;
	struct Ps3LibDiskPFCfgModifyEvtInfo diskPFCfgModifyEvtInfo;
	struct Ps3LibNvDataInvaildInfo nvDataInvaildInfo;
	struct Ps3LibCtrlPowerMode powerMode;
	struct Ps3LibSpeedNegoInfo speedNegoInfo;
	struct Ps3LibInitFailInfo initFailInfo;
	struct Ps3LibTriModeInfo triModeInfo;
	struct Ps3LibPhyChgInfo phyChgInfo;
	struct Ps3LibPhyInquiryInfo phyInquiryInfo;
	struct Ps3LibSmpFailInfo smpFailInfo;
	union Ps3LibNvmeEvtInfo nvmeInfo;
	struct Ps3LibSasSataLinkSpeedMatchInfo sasSataLinkSpeedNoMatchInfo;
	struct Ps3LibSasSataLNExceptionInfo sasSataLNExceptionInfo;
	struct Ps3LibSasSataDriverInfo sasSataDriverInfo;
	unsigned long long value;
	unsigned char data[PS3LIB_EVT_LOG_INFO_MAX_SIZE];
};
#pragma pack()

struct Ps3LibEvtLogEntry {
	unsigned int seqNum;
	struct Ps3LibEvtLogHeader head;
	union Ps3LibReportEvtData evtInfo;
	unsigned int ctrlId;
	unsigned int regCtrlId;
	unsigned int registerId;
	unsigned int pad;
};

struct Ps3LibEvtLogList {
	unsigned int count;
	struct Ps3LibEvtLogEntry evtEntry[0];
};

struct Ps3LibEvtErrDataEntry {
	unsigned int beforeSeqNum;
	unsigned int errDataLen;
	unsigned char *errData;
};

struct Ps3LibEvtLog {
	struct Ps3LibEvtPersistInfo evtPerInfo;
	unsigned int evtCount;
	unsigned int errCount;
	struct Ps3LibEvtLogEntry *evtEntryList;
	struct Ps3LibEvtErrDataEntry *errDataList;
};

struct Ps3LibEventDataCollectionKV {
	char key[PS3LIB_EXP_EVENT_DATA_COLLECT_MAX_NUM];
	char val[PS3LIB_EXP_EVENT_DATA_COLLECT_MAX_NUM];
};

struct Ps3LibEventDataCollection {
	struct Ps3LibEventDataCollectionKV
		kv[PS3LIB_EXP_EVENT_DATA_COLLECT_MAX_NUM];
};

enum {
	PS3LIB_EXPANDER_EVENT_TYPE = 0,
	PS3LIB_SWITCH_EVENT_TYPE = 1,
	PS3LIB_RAID_HBA_EVENT_TYPE = 0xff,
};

struct Ps3LibEvtPrintFunc {
	char const *(*evtCode2Str)(unsigned int opCode);
	char const *(*evtLoca2Str)(unsigned char locate);
	const char *(*getEvtDesc)(struct Ps3LibEvtLogEntry *event, int len,
				  char *buff, int buffLen);

	int (*getEvtData)(struct Ps3LibEventDataCollection *eventDataCollection,
			  struct Ps3LibEvtLogEntry *event, int len, char *buff,
			  int buffLen);
};

struct Ps3LibEvtPrintFunc *ps3libEventPrintFunc(unsigned int ctrlId,
						unsigned char eventType);

int ps3libCtrlEvtlogPerGet(unsigned int ctrlId,
			   struct Ps3LibEvtPersistInfo *evtlogRdInfo);

int ps3libEventLogGet(unsigned int ctrlId, unsigned int sinceSeqNum,
		      struct Ps3LibEvtLog **ppEvtLog);

void ps3libEventLogDestroy(struct Ps3LibEvtLog *pEvtLog);

int ps3libEvtLevelCompare(unsigned char levelA, unsigned char levelB);

int ps3libCtrlEventLogsDelete(unsigned int ctrlId);

unsigned int ps3libEventUinqueIdToCtrlId(unsigned int uniqueId);

#if defined(__cplusplus)
}
#endif

#endif
