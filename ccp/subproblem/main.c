/* Subproblem CPLEX C API (scheduling window) */

#include <stdio.h>
#include <stdlib.h>
#include "/Applications/CPLEX_Studio2211/cplex/include/ilcplex/cplex.h"
#include <time.h>

static void die(CPXENVptr env, CPXLPptr lp, const char* msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    if (lp) CPXfreeprob(env, &lp);
    if (env) CPXcloseCPLEX(&env);
    exit(1);
}

int main(void) {
    clock_t tic = clock();
    int status = 0;

    /* Abrir ambiente e problema */
    CPXENVptr env = CPXopenCPLEX(&status);
    if (!env) die(env, NULL, "CPXopenCPLEX failed");

    CPXLPptr lp = CPXcreateprob(env, &status, "subproblem");
    if (!lp) die(env, lp, "CPXcreateprob failed");

    /* Parâmetros do subproblema (janela local simplificada) */
    int Q = 1;                /* nº de shuttles na janela */
    int T = 5;                /* nº slots na janela */
    int cap = 15;             /* capacidade por viagem */
    double demand[5] = {10, 8, 12, 6, 5};  /* demanda exógena por slot */
    int L = 30;               /* km consumidos por trip */
    int Emax = 150;           /* autonomia total em km (energia disponível) */
    (void)Q;                  /* evita warning se Q não for usado */

    /* Variáveis binárias x_t: 1 se faz trip no slot t, 0 se idle */
    double *obj = (double*) calloc(T, sizeof(double));
    double *lb  = (double*) calloc(T, sizeof(double));
    double *ub  = (double*) calloc(T, sizeof(double));
    char   *ctype = (char*) calloc(T, sizeof(char));
    if (!obj || !lb || !ub || !ctype) die(env, lp, "calloc failed");

    for (int t = 0; t < T; t++) {
        int served = demand[t] < cap ? (int)demand[t] : cap; /* pax servidos se operar */
        obj[t] = - (double)served;   /* max served -> min -served */
        lb[t] = 0.0;
        ub[t] = 1.0;
        ctype[t] = 'B';
    }

    /* adiciona colunas (variáveis) */
    status = CPXnewcols(env, lp, T, obj, lb, ub, ctype, NULL);
    if (status) die(env, lp, "CPXnewcols failed");

    /* Restrição de energia: L * sum_t x_t <= Emax */
    int nzcnt = T;
    int rmatbeg = 0;
    int *rmatind = (int*) malloc(nzcnt * sizeof(int));
    double *rmatval = (double*) malloc(nzcnt * sizeof(double));
    if (!rmatind || !rmatval) die(env, lp, "malloc failed");

    for (int t = 0; t < T; t++) {
        rmatind[t] = t;       /* índice da coluna x_t */
        rmatval[t] = (double)L;
    }

    double rhs = (double)Emax;
    char sense = 'L';
    status = CPXaddrows(env, lp,
                        0,        /* 0 novas colunas */
                        1,        /* 1 nova linha   */
                        nzcnt,    /* nº de coeficientes não-nulos */
                        &rhs, &sense, &rmatbeg, rmatind, rmatval,
                        NULL, NULL);
    if (status) die(env, lp, "CPXaddrows (energy) failed");

    /* Otimiza como MIP, pois x_t são binárias */
    status = CPXmipopt(env, lp);
    if (status) die(env, lp, "CPXmipopt failed");

    /* Recupera objetivo e solução */
    double objval = 0.0;
    status = CPXgetobjval(env, lp, &objval);
    if (status) die(env, lp, "CPXgetobjval failed");

    double *x = (double*) calloc(T, sizeof(double));
    if (!x) die(env, lp, "calloc x failed");
    status = CPXgetmipx(env, lp, x, 0, T - 1);
    if (status) die(env, lp, "CPXgetmipx failed");

    clock_t toc = clock();
    double elapsed = (double)(toc - tic) / CLOCKS_PER_SEC;
    printf("Elapsed CPU time: %.3f s\n", elapsed);

    /* Imprime resultado */
    printf("Obj = %.3f (negativo == max pax)\n", objval);
    int trips = 0, energy_used = 0, pax = 0;
    for (int t = 0; t < T; t++) {
        int decide = (x[t] > 0.5) ? 1 : 0;
        trips += decide;
        energy_used += decide * L;
        pax += decide * (demand[t] < cap ? (int)demand[t] : cap);
        printf("t=%d  x_t=%d\n", t, decide);
    }
    printf("trips=%d  energy_used=%d  pax_served=%d\n", trips, energy_used, pax);

    /* limpeza */
    free(obj); free(lb); free(ub); free(ctype);
    free(rmatind); free(rmatval);
    free(x);

    CPXfreeprob(env, &lp);
    CPXcloseCPLEX(&env);
    return 0;
}
