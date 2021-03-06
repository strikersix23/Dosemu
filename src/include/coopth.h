/*
 * (C) Copyright 1992, ..., 2007 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING.DOSEMU in the DOSEMU distribution
 */

#ifndef COOPTH_H
#define COOPTH_H

#define COOPTH_TID_INVALID (-1)

typedef void (*coopth_func_t)(void *arg);

void coopth_init(void);
int coopth_create(char *name);
int coopth_create_multi(char *name, int len);
int coopth_start(int tid, coopth_func_t func, void *arg);
int coopth_set_post_handler(int tid, coopth_func_t func, void *arg);
void coopth_set_user_data(void *udata);
void *coopth_get_user_data(int tid);
void coopth_wait(void);
void coopth_sleep(void);
void coopth_leave(void);
void coopth_wake_up(int tid);
void coopth_done(void);

int coopth_tag_alloc(void);
void coopth_tag_set(int tag, int cookie);
void coopth_tag_clear(int tag, int cookie);
void coopth_sleep_tagged(int tag, int cookie);
int coopth_get_tid_by_tag(int tag, int cookie);

#endif
