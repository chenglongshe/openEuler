/* SPDX-License-Identifier: GPL-2.0 */

#ifndef PS3_UT
int ps3_scsi_rw_cmd_filter_handle(struct scsi_cmnd *scmd);
int ps3_scsi_task_cmd_filter_handle(struct ps3_cmd *cmd);
int ps3_mgr_cmd_filter_handle(struct ps3_cmd *cmd);
#endif

unsigned char ps3_add_cmd_filter(struct ps3_instance *instance,
				 struct PS3CmdWord *cmd_word);

void ps3_delete_scsi_rw_inject(struct inject_cmds_t *this_pitem);
void ps3_delete_scsi_task_inject(struct inject_cmds_t *this_pitem);
void ps3_delete_mgr_inject(struct inject_cmds_t *this_pitem);

struct PS3HitCmd *get_hit_inject(void);

void ps3_inject_init(void);

void ps3_inject_exit(void);

#endif
