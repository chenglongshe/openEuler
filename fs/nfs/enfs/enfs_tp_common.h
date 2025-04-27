/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022-2022. All rights reserved.
 * Description: nfs tracepoint common header file
 * Author: h00583093
 * Create: 2022-11-01
 */

#ifndef ENFS_TP_COMMON_H
#define ENFS_TP_COMMON_H

#ifdef NFS_CLIENT_DEBUG

#ifndef RETURN_OK
#define RETURN_OK 0
#define RETURN_ERROR (~0)
#endif

#define LVOS_MAX_TRACEP_NUM   1024
#define LVOS_STR_TO_KEY_BASE_NUM   31

/* 计算Hash表大小时使用的移位次数 */
#define LVOS_MAX_TP_HASH_SHIFT 7
/* Hash表内最多包含多少Chunk, 即Hash表的大小 */
#define LVOS_MAX_TP_HASH_SIZE (1 << LVOS_MAX_TP_HASH_SHIFT)

#define LVOS_TRACEP_PARAM_SIZE     32UL
#ifndef MAX_NAME_LEN
#define MAX_NAME_LEN 128
#endif
#ifndef MAX_DESC_LEN
#define MAX_DESC_LEN 256
#endif
#define LVOS_TRACEP_STAT_DELETED   0
#define LVOS_TRACEP_STAT_ACTIVE    1
#define LVOS_TRACEP_STAT_DEACTIVE  2

typedef enum tagLVOS_TP_TYPE_E {
	LVOS_TP_TYPE_CALLBACK = 0,
	LVOS_TP_TYPE_RESET,
	LVOS_TP_TYPE_PAUSE,
	LVOS_TP_TYPE_ABORT,
	LVOS_TP_TYPE_BUTT
} LVOS_TP_TYPE_E;

typedef struct {
	char achParamData[LVOS_TRACEP_PARAM_SIZE];
					       /**<  自定义参数数据区。 */
} LVOS_TRACEP_PARAM_S;

typedef void (*FN_TRACEP_COMMON_T)(LVOS_TRACEP_PARAM_S *, ...);

typedef struct tagLVOS_TRACEP_NEW_S {
	char szName[MAX_NAME_LEN];
	char szDesc[MAX_DESC_LEN];
	unsigned int uiPid;
	int iActive;
	int type;
	unsigned int timeAlive;
	unsigned int timeCalled;
	FN_TRACEP_COMMON_T fnHook;
	LVOS_TRACEP_PARAM_S stParam;
} LVOS_TRACEP_NEW_S;

typedef struct {
	unsigned int cmd;
	unsigned int pid;
	int type;
	unsigned int timeAlive;
	LVOS_TRACEP_PARAM_S userParam;
	char traceName[MAX_NAME_LEN];
} NfsTracePointCmd;

#define IOCTL_MAGIC 'N'
#define IOCTL_CMD_TP_ACTION _IOW(IOCTL_MAGIC, 5, NfsTracePointCmd)

int enfs_tracepoint_init(void);
void enfs_tracepoint_exit(void);
int RegTracePoint(unsigned int pid, const char *name, const char *desc,
		  FN_TRACEP_COMMON_T fnHook);
int UnregTracePoint(unsigned int pid, const char *name);
int GetTracePoint(unsigned int pid, const char *name,
		  LVOS_TRACEP_NEW_S ** tracepoint);
void DoTracePointPause(LVOS_TRACEP_NEW_S * tracepoint);
int deactive_tracepoint(unsigned int pid, const char *name);
int deactive_tracepoint_all(void);
int active_tracepoint(unsigned int pid, const char *name, int type,
		      unsigned int time, LVOS_TRACEP_PARAM_S userParam);

#ifndef MY_PID
#define MY_PID 1234
#endif

#define LVOS_TP_REG(name, desc, fn) RegTracePoint(MY_PID, #name, desc, (FN_TRACEP_COMMON_T)(fn))
#define LVOS_TP_UNREG(name)         UnregTracePoint(0, #name)
#define LVOS_TP_START(name, ...) \
    do {                                                                                        \
        static LVOS_TRACEP_NEW_S *_pstTp = NULL;                      \
        if (unlikely(NULL == _pstTp)) {                                           \
            (void)GetTracePoint(0, #name, &_pstTp);     \
            if (NULL == _pstTp) {                                                   \
                printk(KERN_ERR "tracepoint `%s` not registered", #name);    \
            }                                                                                   \
        }                                                                                       \
        if (NULL != _pstTp && LVOS_TRACEP_STAT_ACTIVE == _pstTp->iActive && LVOS_TP_TYPE_CALLBACK == _pstTp->type) {   \
            _pstTp->fnHook(&_pstTp->stParam, __VA_ARGS__);  \
            _pstTp->timeCalled++;                                   \
            if (_pstTp->timeAlive > 0 && 0 == --(_pstTp->timeAlive)) {                           \
                deactive_tracepoint(0, #name);                                         \
            }                                                                                    \
        } else {                                                                                 \
            if (NULL != _pstTp && LVOS_TRACEP_STAT_ACTIVE == _pstTp->iActive && LVOS_TP_TYPE_PAUSE == _pstTp->type) {      \
                DoTracePointPause(_pstTp);               \
                _pstTp->timeCalled++;                                   \
                if (_pstTp->timeAlive > 0 && 0 == --(_pstTp->timeAlive)) {                                              \
                    deactive_tracepoint(0, #name);                                            \
                }                                                                           \
            }

/*插入故障点结束*/
#define LVOS_TP_END     \
        }               \
    } while (0);

#else

#define LVOS_TP_REG(name, desc, fn)
#define LVOS_TP_UNREG(name)
#define LVOS_TP_START(name, ...)
#define LVOS_TP_END

#endif // NFS_CLIENT_DEBUG

#endif // ENFS_TP_COMMON_H
