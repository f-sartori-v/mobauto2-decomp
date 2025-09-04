/* feas.c */
#include <string.h>
#include <stdio.h>
#include "feas.h"

TaskTok tok_from_str(const char* s){
    if (!s) return TK_NUL;
    if (strcmp(s,"OUT")==0) return TK_OUT;
    if (strcmp(s,"RET")==0) return TK_RET;
    if (strcmp(s,"CRG")==0 || strcmp(s,"CHR")==0) return TK_CRG;
    if (strcmp(s,"NUL")==0) return TK_NUL;
    return TK_NUL;
}
const char* tok_name(TaskTok t){
    switch(t){
        case TK_OUT: return "OUT";
        case TK_RET: return "RET";
        case TK_CRG: return "CRG";
        case TK_NUL: return "NUL";
        default: return "?";
    }
}

static CheckResult ok_res(void){ CheckResult r={1,"OK",-1}; return r; }
static CheckResult bad(const char* m, int pos){ CheckResult r={0,m,pos}; return r; }

/* Transition Rules:
   - OUT → RET
   - RET → (CRG|OUT)
   - CRG → (CRG|OUT)
   - Wrap: se última é OUT, primeira deve ser RET.
*/

CheckResult check_prev_vs_first(const char* prev_task, const char* first_task) {
    TaskTok prev  = tok_from_str(prev_task);
    TaskTok first = tok_from_str(first_task);

    if (prev == TK_OUT && first != TK_RET) {
        return bad("prev=OUT → first must be RET", 0);
    }
    if (prev == TK_RET && !(first == TK_CRG || first == TK_OUT)) {
        return bad("prev=RET → first must be CRG or OUT", 0);
    }
    if (prev == TK_CRG && !(first == TK_CRG || first == TK_OUT)) {
        return bad("prev=CRG → first must be CRG or OUT", 0);
    }
    return ok_res();
}

CheckResult check_time_feas(int horizon_min, int delay, int T, int trip_duration){
    if (trip_duration <= 0) return bad("Invalid duration", -1);
    if (delay < 0) delay = 0;

    int total_time = delay + T * trip_duration;
    if (total_time > horizon_min){
        static char msg[128];
        snprintf(msg, sizeof(msg),
                 "Time infeasible: delay + T*dur = %d > horizon=%d",
                 total_time, horizon_min);
        return bad(msg, -1);
    }
    return ok_res();
}

CheckResult check_battery_feas(int soc0, int Emax, const char* const* seq_str, int T, int cons_trip, int rec_charge,
                               int* soc_final){
    int soc = soc0;
    for (int t=0; t<T; ++t){
        TaskTok a = tok_from_str(seq_str[t]);
        if (a == TK_OUT || a == TK_RET)      soc -= cons_trip;
        else if (a == TK_CRG)                soc += rec_charge;

        if (soc < 0)  return bad("Not enough battery to complete the task sequence", t);
        /*if (soc > Emax) return bad("Warning! Battery exceeding E_max. #TODO", t);*/
    }
    if (soc_final) *soc_final = soc;
    return ok_res();
}
