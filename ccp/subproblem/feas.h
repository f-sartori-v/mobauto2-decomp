/* feas.h */
#ifndef FEAS_H
#define FEAS_H

typedef enum { TK_OUT=0, TK_RET=1, TK_CRG=2, TK_NUL=3 } TaskTok;

TaskTok tok_from_str(const char* s);
const char* tok_name(TaskTok t);

typedef struct {
    int ok;
    const char* msg;
    int viol_pos;   /* violation index, -1 if wrap */
} CheckResult;

CheckResult check_prev_vs_first(const char* prev_task, const char* first_task);
CheckResult check_time_feas(int horizon_min, int delay, int T, int dur);
CheckResult check_battery_feas(int soc0, int Emax, const char* const* seq_str, int T,
                               int cons_trip, int rec_charge, int* soc_final);

#endif
