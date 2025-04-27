 /*
  * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
  * Description: failover time header file
  * Create: 2023-08-02
  */

#ifndef FAILOVER_TIME_H
#define FAILOVER_TIME_H

#include

void failover_adjust_task_timeout(struct rpc_task *task, void *condition);
void failover_init_task_req(struct rpc_task *task, struct rpc_rqst *req);

#endif // FAILOVER_TIME_H
