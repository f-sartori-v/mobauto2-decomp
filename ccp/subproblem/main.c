/* Subproblem CPLEX C API (scheduling window) */

#include <stdio.h>
#include <time.h>
#include <string.h>
#include "params.h"
#include "feas.h"
#include "/Applications/CPLEX_Studio2211/cplex/include/ilcplex/cplex.h"
#include "third_party/cjson/cJSON.h"

static void die(CPXENVptr env, CPXLPptr lp, const char* msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    if (lp) CPXfreeprob(env, &lp);
    if (env) CPXcloseCPLEX(&env);
    exit(1);
}

void parse_params_and_fleet(const char* merged_path, Params* P, Fleet* F, DemandPool* D);

static int check_one_shuttle(const Params* P, const Shuttle* S, int idx){
    CheckResult r1 = check_prev_vs_first(S->prev, S->seq[0]);
    if (!r1.ok) {
        printf("S%d infeasible: %s\n", idx, r1.msg);
        return 0;
    }

    // 2. Checar tempo: delay + T*dur <= horizon
    const int DUR = 30; // duração de cada tarefa (min)
    CheckResult r2 = check_time_feas(P->horizon_min, S->delay, S->T, DUR);
    if (!r2.ok) {
        printf("S%d infeasible (time): %s\n", idx, r2.msg);
        return 0;
    }

    // 3. Checar bateria
    int soc_final = 0;
    CheckResult r3 = check_battery_feas(
        S->soc0, P->battery_range,
        (const char* const*)S->seq, S->T,
        /*consumo por OUT/RET*/ 28,
        /*recarga por CRG*/ 35,
        &soc_final
    );
    if (!r3.ok) {
        printf("S%d infeasible (battery): %s (at pos %d)\n", idx, r3.msg, r3.viol_pos);
        return 0;
    }

    // Se chegou aqui, shuttle é factível
    printf("S%d factible | seq_len=%d delay=%d soc0=%d soc_final=%d\n",
           idx, S->T, S->delay, S->soc0, soc_final);
           return 1;
}

int main(int argc, char** argv){
    clock_t tic = clock();

    if (argc < 2) { fprintf(stderr,"usage: subproblem <merged.json>\n"); return 2; }

    Params P = {0};
    Fleet  F = {0};
    DemandPool D = {0};

    parse_params_and_fleet(argv[1], &P, &F, &D);

    // sanity print only
    printf("[ok] horizon=%d cap=%d trip_dist=%d Emax=%d shuttles=%d requests=%d\n",
           P.horizon_min, P.cap, P.trip_dist, P.battery_range, P.nbr_shuttles, D.n);

    for(int i=0;i<F.n;i++){
        Shuttle* S=&F.arr[i];
        printf("S%d: T=%d soc0=%d prev=%s delay=%d seq=[", i, S->T, S->soc0, S->prev, S->delay);
        for(int t=0;t<S->T;t++){ printf("%s%s", S->seq[t], (t+1<S->T)?",":""); }
        printf("]\n");
    }

    int all_feas = 1;
    for (int i=0; i<F.n; ++i){
        int feas_i = check_one_shuttle(&P, &F.arr[i], i);
        if (!feas_i) all_feas = 0;
    }

    if (!all_feas) {
        clock_t toc = clock();
        double elapsed = (double)(toc - tic) / CLOCKS_PER_SEC;
        fprintf(stderr, "Infeasible. Elapsed CPU time: %.3f seconds\n", elapsed);
        free_fleet(&F);
        free_demand(&D);
        return 0;
    }

    int status = 0;
    CPXENVptr env = CPXopenCPLEX(&status);
    if (!env) { fprintf(stderr,"CPXopenCPLEX failed\n"); goto CLEANUP_FAIL; }

    CPXLPptr lp = CPXcreateprob(env, &status, "subproblem");
    if (!lp) { fprintf(stderr,"CPXcreateprob failed\n"); goto CLEANUP_FAIL; }

    int I = F.n;
    int R = D.n;
    int N = I * R;

    double *obj = (double*) calloc(N, sizeof(double));
    double *lb  = (double*) calloc(N, sizeof(double));
    double *ub  = (double*) calloc(N, sizeof(double));
    char   *ctype = (char*) calloc(N, sizeof(char));
    if (!obj || !lb || !ub || !ctype) { fprintf(stderr,"calloc failed\n"); goto CLEANUP_FAIL; }

    /* Objective: maximize served demand */
    for (int i = 0; i < I; i++) {
      for (int r = 0; r < R; r++) {
        int k = i * R + r;
        obj[k] = -1.0;
        lb[k] = 0.0;
        ub[k] = 1.0;
        ctype[k] = 'B';
      }
    }
    status = CPXnewcols(env, lp, N, obj, lb, ub, ctype, NULL);

    /* Capacity per shuttle */
    for (int i = 0; i < I; ++i){
        /* Count trip slots in this shuttle’s sequence as a crude capacity proxy */
        int trip_slots = 0;
        for (int t = 0; t < F.arr[i].T; ++t){
            const char* a = F.arr[i].seq[t];
            if (strcmp(a, "OUT")==0 || strcmp(a,"RET")==0) trip_slots++;
        }
        int max_assign_i = trip_slots * P.cap;

        int nzcnt = R;
        int rmatbeg = 0;
        int *rmatind = (int*) malloc(nzcnt * sizeof(int));
        double *rmatval = (double*) malloc(nzcnt * sizeof(double));
        if (!rmatind || !rmatval) { fprintf(stderr,"malloc failed\n"); goto CLEANUP_FAIL; }

        for (int r = 0; r < R; ++r){
            rmatind[r] = i*R + r;
            rmatval[r] = 1.0;
        }
        double rhs = (double)max_assign_i;
        char sense = 'L';
        status = CPXaddrows(env, lp, 0, 1, nzcnt, &rhs, &sense, &rmatbeg, rmatind, rmatval, NULL, NULL);
        free(rmatind); free(rmatval);
        if (status) { fprintf(stderr,"CPXaddrows(capacity) failed\n"); goto CLEANUP_FAIL; }
    }

    /* Each request served at most once */
    for (int r = 0; r < R; ++r){
        int nzcnt = I;
        int rmatbeg = 0;
        int *rmatind = (int*) malloc(nzcnt * sizeof(int));
        double *rmatval = (double*) malloc(nzcnt * sizeof(double));
        if (!rmatind || !rmatval) { fprintf(stderr,"malloc failed\n"); goto CLEANUP_FAIL; }

        for (int i = 0; i < I; ++i){
            rmatind[i] = i*R + r;
            rmatval[i] = 1.0;
        }
        double rhs = 1.0;
        char sense = 'L';
        status = CPXaddrows(env, lp, 0, 1, nzcnt, &rhs, &sense, &rmatbeg, rmatind, rmatval, NULL, NULL);
        free(rmatind); free(rmatval);
        if (status) { fprintf(stderr,"CPXaddrows(req≤1) failed\n"); goto CLEANUP_FAIL; }
    }

    status = CPXmipopt(env, lp);
    if (status) { fprintf(stderr,"CPXmipopt failed\n"); goto CLEANUP_FAIL; }

    double objval = 0.0;
    status = CPXgetobjval(env, lp, &objval);
    if (status) { fprintf(stderr,"CPXgetobjval failed\n"); goto CLEANUP_FAIL; }

    double *y = (double*) calloc(N, sizeof(double));
    if (!y) { fprintf(stderr,"calloc y failed\n"); goto CLEANUP_FAIL; }
    status = CPXgetmipx(env, lp, y, 0, N-1);
    if (status) { fprintf(stderr,"CPXgetmipx failed\n"); goto CLEANUP_FAIL; }

    /* Report timing (feasible branch) */
    {
        clock_t toc = clock();
        double elapsed = (double)(toc - tic) / CLOCKS_PER_SEC;
        printf("Feasible. Assignment solved. Elapsed CPU time: %.3f s\n", elapsed);
    }
/* Simple print of chosen pairs */
    {
        int served = 0;
        for (int i=0;i<I;++i){
            for (int r=0;r<R;++r){
                int k = i*R + r;
                if (y[k] > 0.5) {
                    printf("Assign: shuttle %d -> request %d (y=1)\n", i, r);
                    served++;
                }
            }
        }
        printf("Total served requests = %d; objective (max served) = %.0f\n", served, -objval);
    }

    /* cleanup on success */
    free(y);
    free(obj); free(lb); free(ub); free(ctype);
    CPXfreeprob(env, &lp);
    CPXcloseCPLEX(&env);

    free_fleet(&F);
    free_demand(&D);
    return 0;

CLEANUP_FAIL:
    free(obj); free(lb); free(ub); free(ctype);
    if (lp) CPXfreeprob(env, &lp);
    if (env) CPXcloseCPLEX(&env);
    free_fleet(&F);
    free_demand(&D);
    return 1;
}
