/* Vita Lab additions to the user module (see lab.c). */
#pragma once

#include <stddef.h>

void cmd_delete(char **arg_list, size_t arg_count, char *res_msg);
void cmd_apps(char **arg_list, size_t arg_count, char *res_msg);
void cmd_stats(char **arg_list, size_t arg_count, char *res_msg);
void cmd_clock(char **arg_list, size_t arg_count, char *res_msg);

void lab_net_up(void);
void lab_net_down(void);
void lab_end(void);
