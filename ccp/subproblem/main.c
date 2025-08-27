/* Subproblem CPLEX C API (scheduling window) */

#include <stdio.h>
#include <stdlib.h>
#include "ilcplex/cplex.h"

static void die(CPXENVptr env, CPXLPptr lp, const char* msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
    if (lp) CPXfreeprob(env, &lp);
    if (env) CPXcloseCPLEX(&env);
    exit(1);
}

int main(void) {
    int status = 0;

    /* abrir ambiente e problema */
    CPXENVptr env = CPXopenCPLEX(&status);
    if (!env) die(env, NULL, "CPXopenCPLEX failed");

    CPXLPptr lp = CPXcreateprob(env, &status, "subproblem");
    if (!lp) die(env, lp, "CPXcreateprob failed");

    /* Parâmetros do subproblema (janela local simplificada) */
    int Q = 1;                // número de shuttles na janela
    int T = 5;                // nº slots na janela
    int cap = 15;             // capacidade
    double demand[5] = {10, 8, 12, 6, 5};  // demanda exógena nessa janela
    int L = 30;               // km por leg
    int Emax = 150;           // autonomia
    int Epartial = 75;

    /* Variáveis binárias x_t: 1 se o shuttle faz um trip no slot t, 0 idle */
    double *obj = (double*) calloc(T, sizeof(double));
    double *lb  = (double*) calloc(T, sizeof(double));
    double *ub  = (double*) calloc(T, sizeof(double));
    char   *ctype = (char*) calloc(T, sizeof(char));
    char **names  = (char**) calloc(T, sizeof(char*));

    for (int t = 0; t < T; t++) {
        obj[t] = - (demand[t] < cap ? demand[t] : cap);  // max pax -> min -pax
        lb[t] = 0.0; ub[t] = 1.0;
        ctype[t] = 'B';
    }

    status = CPXnewcols(env, lp, T, obj, lb, ub, ctype, names);
    if (status) die(env, lp, "add vars failed");

    /* Restrição: no máximo 1 atividade por slot (já que só 1 shuttle) */
    for (int t = 0; t < T; t++) {
