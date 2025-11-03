// Subproblem MILP (Concert C++): assign requests to fixed sequences, choose departure times within ±delta,
// enforce no-overlap across all non-NUL tasks, minimize 50*unserved + sum waiting.
//
// Build: link with -lilocplex -lcplex -lconcert and include your cJSON path.
// Usage: subproblem <subproblem.json> <requests.json> <out_result.json>

#include <ilcplex/ilocplex.h>
ILOSTLBEGIN

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <chrono>
#include <limits>

extern "C" {
#include "third_party/cjson/cJSON.h"
}

static std::string slurp(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream ss; ss << ifs.rdbuf();
    return ss.str();
}
static void die(const char* msg){ std::fprintf(stderr,"ERROR: %s\n", msg); std::exit(1); }

struct Trip {
    int q;           // shuttle id
    int t;           // slot index
    int e;           // earliest start (min)
    int l;           // latest start (min)
    int D;           // duration (min)
    int dir;         // 0=OUT, 1=RET
};

int main(int argc, char** argv){
    if (argc < 4){
        std::fprintf(stderr, "usage: subproblem <subproblem.json> <requests.json> <out_result.json> [warm_start.json]\n");
        return 2;
    }
    const std::string sub_path = argv[1];
    const std::string req_path = argv[2];
    const std::string out_path = argv[3];
    const std::string warm_path = (argc >= 5 ? argv[4] : "");

    // --------------------------
    // 1) Parse master output (sequences)
    // --------------------------
    cJSON* jsub = cJSON_Parse(slurp(sub_path).c_str());
    if (!jsub) die("invalid subproblem.json");
    const cJSON* jQ  = cJSON_GetObjectItemCaseSensitive(jsub, "nbr_shuttles");
    const cJSON* jT  = cJSON_GetObjectItemCaseSensitive(jsub, "num_slots");
    const cJSON* jSh = cJSON_GetObjectItemCaseSensitive(jsub, "shuttles");
    const cJSON* jTi = cJSON_GetObjectItemCaseSensitive(jsub, "sp_time_limit");
    if (!cJSON_IsObject(jSh)) die("subproblem.json missing 'shuttles' object");

    int Q = cJSON_IsNumber(jQ) ? (int) jQ->valuedouble : 0;
    int T = cJSON_IsNumber(jT) ? (int) jT->valuedouble : -1;
    if (Q <= 0) {
        // Count shuttles from object keys (assumes S0..S{Q-1})
        int cnt = 0;
        for (const cJSON* it = jSh->child; it; it = it->next) {
            if (cJSON_IsObject(it)) cnt++;
        }
        Q = std::max(1, cnt);
    }
    if (T < 0) {
        // Infer T from the first shuttle's seq length
        const cJSON* first = jSh->child;
        if (!first) die("subproblem.json has empty 'shuttles'");
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(first, "seq");
        if (!cJSON_IsArray(arr)) die("subproblem.json: shuttle.seq must be array");
        T = cJSON_GetArraySize(arr);
    }

    // optional parameters (with defaults)
    int slot_minutes  = 30;
    int delta_minutes = 30;   // allow ±30 min around slot
    int trip_duration = 30;   // D
    int seat_capacity = 15;
    int theta0_min    = 0;    // horizon origin
    const cJSON* jsmin  = cJSON_GetObjectItemCaseSensitive(jsub, "slot_minutes");
    const cJSON* jdel   = cJSON_GetObjectItemCaseSensitive(jsub, "window_plus_minus");
    const cJSON* jdur   = cJSON_GetObjectItemCaseSensitive(jsub, "trip_duration_min");
    const cJSON* jcap   = cJSON_GetObjectItemCaseSensitive(jsub, "seat_capacity");
    const cJSON* jth0   = cJSON_GetObjectItemCaseSensitive(jsub, "theta0_min");
    if (cJSON_IsNumber(jsmin)) slot_minutes  = (int) jsmin->valuedouble;
    if (cJSON_IsNumber(jdel))  delta_minutes = (int) jdel->valuedouble;
    if (cJSON_IsNumber(jdur))  trip_duration = (int) jdur->valuedouble;
    if (cJSON_IsNumber(jcap))  seat_capacity = (int) jcap->valuedouble;
    if (cJSON_IsNumber(jth0))  theta0_min    = (int) jth0->valuedouble;

    // read sequences
    struct Event { int q, t; std::string typ; };
    std::vector<std::vector<std::string>> seq(Q, std::vector<std::string>(T, "NUL"));
    for (int q=0; q<Q; ++q){
        char key[16]; std::snprintf(key,sizeof(key),"S%d", q);
        const cJSON* Sj = cJSON_GetObjectItemCaseSensitive(jSh, key);
        if (!Sj){ die("missing shuttle key in subproblem.json"); }
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(Sj, "seq");
        if (!cJSON_IsArray(arr)) die("bad or missing seq array");
        const int n = cJSON_GetArraySize(arr);
        if (n != T) die("seq length mismatch with num_slots");
        for (int t=0; t<T; ++t){
            const cJSON* s = cJSON_GetArrayItem(arr, t);
            if (!cJSON_IsString(s)) die("non-string seq entry");
            seq[q][t] = s->valuestring;
        }
    }

    // --------------------------
    // 2) Parse exact requests (single or multi-scenario)
    // --------------------------
    cJSON* jreq = cJSON_Parse(slurp(req_path).c_str());
    if (!jreq) die("invalid requests.json");
    const cJSON* jSc = cJSON_GetObjectItemCaseSensitive(jreq, "scenarios");
    int S_scen = cJSON_IsArray(jSc) ? cJSON_GetArraySize(jSc) : 1;
    if (S_scen <= 0) die("empty scenarios list");

    // For each scenario s, split requests by direction
    std::vector<std::vector<int>> req_time_OUT_S(S_scen), req_time_RET_S(S_scen);
    std::vector<int> R_OUT_S(S_scen, 0), R_RET_S(S_scen, 0);

    if (cJSON_IsArray(jSc)) {
        for (int s=0; s<S_scen; ++s){
            const cJSON* sc = cJSON_GetArrayItem(jSc, s);
            const cJSON* jArr = cJSON_GetObjectItemCaseSensitive(sc, "requests");
            if (!cJSON_IsArray(jArr)) die("scenario requests[] missing");
            const cJSON* jN = cJSON_GetObjectItemCaseSensitive(sc, "nreq");
            const int R_total = (int) (cJSON_IsNumber(jN) ? jN->valuedouble : cJSON_GetArraySize(jArr));
            req_time_OUT_S[s].reserve(R_total); req_time_RET_S[s].reserve(R_total);
            for (int i=0; i<R_total; ++i){
                const cJSON* ri = cJSON_GetArrayItem(jArr, i);
                const cJSON* d  = cJSON_GetObjectItemCaseSensitive(ri, "dir");
                const cJSON* tm = cJSON_GetObjectItemCaseSensitive(ri, "time");
                if (!cJSON_IsString(d) || !cJSON_IsNumber(tm)) die("bad request row");
                const std::string dir = d->valuestring;
                const int rtime = (int) tm->valuedouble;
                if (dir=="OUT"){ req_time_OUT_S[s].push_back(rtime); }
                else if (dir=="RET"){ req_time_RET_S[s].push_back(rtime); }
                else die("unknown request dir (expect OUT/RET)");
            }
            R_OUT_S[s] = (int)req_time_OUT_S[s].size();
            R_RET_S[s] = (int)req_time_RET_S[s].size();
        }
    } else {
        // Single scenario in legacy format {nreq, requests}
        const cJSON* jN   = cJSON_GetObjectItemCaseSensitive(jreq, "nreq");
        const cJSON* jArr = cJSON_GetObjectItemCaseSensitive(jreq, "requests");
        if (!cJSON_IsArray(jArr)) die("requests[] missing");
        const int R_total = (int) (cJSON_IsNumber(jN) ? jN->valuedouble : cJSON_GetArraySize(jArr));
        req_time_OUT_S[0].reserve(R_total); req_time_RET_S[0].reserve(R_total);
        for (int i=0; i<R_total; ++i){
            const cJSON* ri = cJSON_GetArrayItem(jArr, i);
            const cJSON* d  = cJSON_GetObjectItemCaseSensitive(ri, "dir");
            const cJSON* tm = cJSON_GetObjectItemCaseSensitive(ri, "time");
            if (!cJSON_IsString(d) || !cJSON_IsNumber(tm)) die("bad request row");
            const std::string dir = d->valuestring;
            const int rtime = (int) tm->valuedouble;
            if (dir=="OUT"){ req_time_OUT_S[0].push_back(rtime); }
            else if (dir=="RET"){ req_time_RET_S[0].push_back(rtime); }
            else die("unknown request dir (expect OUT/RET)");
        }
        R_OUT_S[0] = (int)req_time_OUT_S[0].size();
        R_RET_S[0] = (int)req_time_RET_S[0].size();
    }

    // --------------------------
    // 3) Build list of trips (variables tau) and “events” (sigma) per shuttle
    // --------------------------
    std::vector<Trip> trips; trips.reserve(Q*T);
    std::vector<std::vector<int>> tripIndexAt(Q, std::vector<int>(T, -1)); // map (q,t)->k or -1
    std::vector<std::vector<int>> eventIndexAt(Q, std::vector<int>(T, -1)); // map (q,t)->m or -1

    // For each shuttle, create a chain of non-NUL events with start time sigma_m in [theta(t)-delta, theta(t)+delta]
    struct EventSlot { int t; int e; int l; bool isTrip; int tripIdx; int D; };
    std::vector<std::vector<EventSlot>> events(Q);

    auto theta = [&](int t){ return theta0_min + t*slot_minutes; };

    for (int q=0; q<Q; ++q){
        for (int t=0; t<T; ++t){
            const std::string& typ = seq[q][t];
            if (typ=="NUL") continue;

            int e = std::max(0, theta(t) - delta_minutes);
            int l = theta(t) + delta_minutes;
            int D = trip_duration;

            bool isTrip = (typ=="OUT" || typ=="RET");
            int kidx = -1;
            if (isTrip){
                Trip k; k.q=q; k.t=t; k.e=e; k.l=l; k.D=D; k.dir = (typ=="OUT"?0:1);
                kidx = (int)trips.size();
                trips.push_back(k);
                tripIndexAt[q][t] = kidx;
            }
            EventSlot es; es.t=t; es.e=e; es.l=l; es.isTrip=isTrip; es.tripIdx=kidx; es.D=D;
            eventIndexAt[q][t] = (int)events[q].size();
            events[q].push_back(es);
        }
    }
    const int K = (int)trips.size();

    // Optional warm start: map (q,t)->tau suggestion from previous best scenario
    std::vector<double> tauWarm(K, std::numeric_limits<double>::quiet_NaN());
    if (!warm_path.empty()){
        try {
            std::string wt = slurp(warm_path);
            if (!wt.empty()){
                cJSON* jw = cJSON_Parse(wt.c_str());
                if (jw){
                    const cJSON* jTrips = cJSON_GetObjectItemCaseSensitive(jw, "trips");
                    if (cJSON_IsArray(jTrips)){
                        for (int i=0; i<cJSON_GetArraySize(jTrips); ++i){
                            const cJSON* tk = cJSON_GetArrayItem(jTrips, i);
                            const cJSON* jq = cJSON_GetObjectItemCaseSensitive(tk, "q");
                            const cJSON* jt = cJSON_GetObjectItemCaseSensitive(tk, "t");
                            const cJSON* jtau = cJSON_GetObjectItemCaseSensitive(tk, "tau");
                            if (cJSON_IsNumber(jq) && cJSON_IsNumber(jt) && cJSON_IsNumber(jtau)){
                                int q = (int)jq->valuedouble;
                                int t = (int)jt->valuedouble;
                                if (q >=0 && q < Q && t >=0 && t < T){
                                    int k = tripIndexAt[q][t];
                                    if (k >= 0 && k < K){
                                        tauWarm[k] = jtau->valuedouble;
                                    }
                                }
                            }
                        }
                    }
                    cJSON_Delete(jw);
                }
            }
        } catch (...) {
            // ignore warm start errors
        }
    }

    // Horizon stats for big-M
    int earliest_any = 0;
    int latest_any   = (T>0 ? theta(T-1) + delta_minutes : 0);
    const double Mtime = (double)(std::max(0, latest_any - earliest_any) + trip_duration);

    // --------------------------
    // 4) Build MILP (Concert)
    // --------------------------
    IloEnv env;
    int exit_code = 0;
    try {
        IloModel model(env);
        IloCplex cplex(model);
        double solve_time_sec = 0.0;

        // Variables
        // tau[k] for trips (start time)
        IloNumVarArray tau(env, K);
        for (int k=0; k<K; ++k){
            tau[k] = IloNumVar(env, trips[k].e, trips[k].l, ILOFLOAT);
            tau[k].setName(("tau_"+std::to_string(k)).c_str());
        }
        // sigma (start time) for every non-NUL event, chained; link tau for trips
        std::vector<IloNumVarArray> sigma(Q);
        for (int q=0; q<Q; ++q){
            const int Mq = (int)events[q].size();
            sigma[q] = IloNumVarArray(env, Mq);
            for (int m=0; m<Mq; ++m){
                sigma[q][m] = IloNumVar(env, events[q][m].e, events[q][m].l, ILOFLOAT);
                sigma[q][m].setName(("sigma_q"+std::to_string(q)+"_m"+std::to_string(m)).c_str());
            }
            // chain: sigma[m+1] ≥ sigma[m] + D_m
            for (int m=0; m+1<Mq; ++m){
                model.add( sigma[q][m+1] >= sigma[q][m] + events[q][m].D );
            }
            // link trips
            for (int m=0; m<Mq; ++m){
                if (events[q][m].isTrip){
                    int k = events[q][m].tripIdx;
                    model.add( tau[k] == sigma[q][m] );
                }
            }
        }

        // Assignment variables per scenario and request-level waits/unserved
        // We keep two direction partitions for memory efficiency.
        const double A = 50.0; // penalty for unserved
        const int WAIT_MAX = 30; // Maximum allowed waiting time (minutes)

        // Per-scenario variables
        std::vector<IloBoolVarArray> u_out_S(S_scen), u_ret_S(S_scen);
        std::vector<IloNumVarArray>  w_out_S(S_scen), w_ret_S(S_scen);
        for (int s=0; s<S_scen; ++s){
            u_out_S[s] = IloBoolVarArray(env, R_OUT_S[s]);
            w_out_S[s] = IloNumVarArray(env, R_OUT_S[s]);
            for (int i=0; i<R_OUT_S[s]; ++i){ u_out_S[s][i] = IloBoolVar(env); w_out_S[s][i] = IloNumVar(env, 0, IloInfinity); }
            u_ret_S[s] = IloBoolVarArray(env, R_RET_S[s]);
            w_ret_S[s] = IloNumVarArray(env, R_RET_S[s]);
            for (int i=0; i<R_RET_S[s]; ++i){ u_ret_S[s][i] = IloBoolVar(env); w_ret_S[s][i] = IloNumVar(env, 0, IloInfinity); }
        }

        // z^{OUT}_s[k][i], z^{RET}_s[k][i]
        std::vector<std::vector<IloBoolVarArray>> z_out_S(S_scen), z_ret_S(S_scen);
        for (int s=0; s<S_scen; ++s){
            z_out_S[s].reserve(K);
            z_ret_S[s].reserve(K);
            for (int k=0; k<K; ++k){
                if (trips[k].dir==0){
                    z_out_S[s].emplace_back(env, R_OUT_S[s]);
                    for (int i=0; i<R_OUT_S[s]; ++i) z_out_S[s].back()[i] = IloBoolVar(env);
                    z_ret_S[s].emplace_back(env, 0);
                } else {
                    z_out_S[s].emplace_back(env, 0);
                    z_ret_S[s].emplace_back(env, R_RET_S[s]);
                    for (int i=0; i<R_RET_S[s]; ++i) z_ret_S[s].back()[i] = IloBoolVar(env);
                }
            }
        }

        // Capacity per trip per scenario (not coupled across scenarios)
        for (int s=0; s<S_scen; ++s){
            for (int k=0; k<K; ++k){
                if (trips[k].dir==0){
                    IloExpr sum(env);
                    for (int i=0;i<R_OUT_S[s];++i) sum += z_out_S[s][k][i];
                    model.add( sum <= seat_capacity );
                    sum.end();
                } else {
                    IloExpr sum(env);
                    for (int i=0;i<R_RET_S[s];++i) sum += z_ret_S[s][k][i];
                    model.add( sum <= seat_capacity );
                    sum.end();
                }
            }
        }

        // Each request served at most once (in its scenario), waiting and time feasibility
        for (int s=0; s<S_scen; ++s){
            // OUT requests
            for (int i=0;i<R_OUT_S[s];++i){
                IloExpr served(env);
                for (int k=0; k<K; ++k){
                    if (trips[k].dir==0){
                        served += z_out_S[s][k][i];
                        // time feasibility + waiting linking (scenario-specific ready time)
                        model.add( tau[k] >= req_time_OUT_S[s][i] - Mtime*(1.0 - z_out_S[s][k][i]) );
                        model.add( tau[k] <= req_time_OUT_S[s][i] + WAIT_MAX + Mtime*(1.0 - z_out_S[s][k][i]) );
                        model.add( w_out_S[s][i] >= tau[k] - req_time_OUT_S[s][i] - Mtime*(1.0 - z_out_S[s][k][i]) );
                    }
                }
                model.add( served + u_out_S[s][i] == 1 );
                served.end();
            }
            // RET requests
            for (int i=0;i<R_RET_S[s];++i){
                IloExpr served(env);
                for (int k=0; k<K; ++k){
                    if (trips[k].dir==1){
                        served += z_ret_S[s][k][i];
                        model.add( tau[k] >= req_time_RET_S[s][i] - Mtime*(1.0 - z_ret_S[s][k][i]) );
                        model.add( tau[k] <= req_time_RET_S[s][i] + WAIT_MAX + Mtime*(1.0 - z_ret_S[s][k][i]) );
                        model.add( w_ret_S[s][i] >= tau[k] - req_time_RET_S[s][i] - Mtime*(1.0 - z_ret_S[s][k][i]) );
                    }
                }
                model.add( served + u_ret_S[s][i] == 1 );
                served.end();
            }
        }

        // Objective: minimize average over scenarios of (A*unserved + sum waiting)
        IloExpr obj(env);
        for (int s=0; s<S_scen; ++s){
            for (int i=0;i<R_OUT_S[s];++i){ obj += A * u_out_S[s][i] + w_out_S[s][i]; }
            for (int i=0;i<R_RET_S[s];++i){ obj += A * u_ret_S[s][i] + w_ret_S[s][i]; }
        }
        // Average is obj / S_scen (constant factor), equivalent for optimization
        model.add(IloMinimize(env, obj));
        obj.end();

        // Parameters (tune as you like)
        double tiLim = 300.0; // default
        if (cJSON_IsNumber(jTi)) tiLim = jTi->valuedouble;
        cplex.setParam(IloCplex::TiLim, tiLim);
        cplex.setParam(IloCplex::EpGap, 1e-4);        // MIP gap
        cplex.setParam(IloCplex::Threads, 0);         // all cores
        // cplex.setParam(IloCplex::MIPEmphasis, 1);   // 1=feasibility, 2=optimality

        // Provide warm start if available (partial MIP start on tau)
        {
            IloNumVarArray startVars(env);
            IloNumArray    startVals(env);
            for (int k=0; k<K; ++k){
                double v = tauWarm[k];
                if (v == v) { // not NaN
                    startVars.add(tau[k]);
                    // clip to variable bounds just in case
                    double lb = trips[k].e;
                    double ub = trips[k].l;
                    if (v < lb) v = lb;
                    if (v > ub) v = ub;
                    startVals.add(v);
                }
            }
            if (startVars.getSize() > 0) {
                cplex.addMIPStart(startVars, startVals, IloCplex::MIPStartAuto);
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        bool ok = cplex.solve();
        auto t1 = std::chrono::steady_clock::now();
        solve_time_sec = std::chrono::duration<double>(t1 - t0).count();
        if (!ok){
            std::fprintf(stderr, "subproblem: no solution (status=%d)\n", (int)cplex.getStatus());
            cJSON_Delete(jsub); cJSON_Delete(jreq); env.end();
            return 1;
        }

        const double objval = cplex.getObjValue();
        std::printf("[sp] status=%d obj=%.3f S=%d time=%.3f s\n", (int)cplex.getStatus(), objval, S_scen, solve_time_sec);

        // --------------------------
        // 5) Emit result JSON
        // --------------------------
        cJSON* jout = cJSON_CreateObject();
        cJSON_AddNumberToObject(jout, "objective_sum", objval);
        cJSON_AddNumberToObject(jout, "objective_avg", (S_scen>0 ? objval / (double)S_scen : objval));
        cJSON_AddNumberToObject(jout, "alpha_unserved", A);
        cJSON_AddNumberToObject(jout, "solve_time_sec", solve_time_sec);

        // Trip schedule: tau per (q,t) for trips
        cJSON* jTrips = cJSON_CreateArray();
        for (int k=0; k<K; ++k){
            cJSON* tk = cJSON_CreateObject();
            cJSON_AddNumberToObject(tk, "q", trips[k].q);
            cJSON_AddNumberToObject(tk, "t", trips[k].t);
            cJSON_AddStringToObject(tk, "dir", trips[k].dir==0?"OUT":"RET");
            cJSON_AddNumberToObject(tk, "tau", cplex.getValue(tau[k]));
            cJSON_AddItemToArray(jTrips, tk);
        }
        cJSON_AddItemToObject(jout, "trips", jTrips);

        // Scenario metrics
        cJSON* jScMet = cJSON_CreateArray();
        for (int s=0; s<S_scen; ++s){
            int served_total = 0, unserved_total = 0;
            double waiting_sum = 0.0;
            for (int i=0;i<R_OUT_S[s];++i){
                if (cplex.getValue(u_out_S[s][i]) > 0.5) unserved_total++;
                else served_total++;
                waiting_sum += cplex.getValue(w_out_S[s][i]);
            }
            for (int i=0;i<R_RET_S[s];++i){
                if (cplex.getValue(u_ret_S[s][i]) > 0.5) unserved_total++;
                else served_total++;
                waiting_sum += cplex.getValue(w_ret_S[s][i]);
            }
            double obj_s = 50.0 * unserved_total + waiting_sum;
            cJSON* js = cJSON_CreateObject();
            cJSON_AddNumberToObject(js, "scenario_index", s);
            cJSON_AddNumberToObject(js, "served_total", served_total);
            cJSON_AddNumberToObject(js, "unserved_total", unserved_total);
            cJSON_AddNumberToObject(js, "waiting_sum", waiting_sum);
            cJSON_AddNumberToObject(js, "objective", obj_s);
            cJSON_AddItemToArray(jScMet, js);
        }
        cJSON_AddItemToObject(jout, "scenario_metrics", jScMet);

        // write file
        {
            std::ofstream ofs(out_path, std::ios::binary);
            char* text = cJSON_PrintBuffered(jout, 2, 0);
            if (text){ ofs << text; std::free(text); }
            cJSON_Delete(jout);
        }

        cJSON_Delete(jsub); cJSON_Delete(jreq);
        env.end();
        return 0;
    } catch (const IloException& e){
        std::fprintf(stderr, "IloException: %s\n", e.getMessage());
        exit_code = 1;
    } catch (const std::exception& e){
        std::fprintf(stderr, "std::exception: %s\n", e.what());
        exit_code = 1;
    }
    cJSON_Delete(jsub); cJSON_Delete(jreq);
    return exit_code;
}
