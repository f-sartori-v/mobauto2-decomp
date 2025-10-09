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
        std::fprintf(stderr, "usage: subproblem <subproblem.json> <requests.json> <out_result.json>\n");
        return 2;
    }
    const std::string sub_path = argv[1];
    const std::string req_path = argv[2];
    const std::string out_path = argv[3];

    // --------------------------
    // 1) Parse master output (sequences)
    // --------------------------
    cJSON* jsub = cJSON_Parse(slurp(sub_path).c_str());
    if (!jsub) die("invalid subproblem.json");
    const cJSON* jQ  = cJSON_GetObjectItemCaseSensitive(jsub, "nbr_shuttles");
    const cJSON* jT  = cJSON_GetObjectItemCaseSensitive(jsub, "num_slots");
    const cJSON* jSh = cJSON_GetObjectItemCaseSensitive(jsub, "shuttles");
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
    int delta_minutes = 15;
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
    // 2) Parse exact requests
    // --------------------------
    cJSON* jreq = cJSON_Parse(slurp(req_path).c_str());
    if (!jreq) die("invalid requests.json");
    const cJSON* jN   = cJSON_GetObjectItemCaseSensitive(jreq, "nreq");
    const cJSON* jArr = cJSON_GetObjectItemCaseSensitive(jreq, "requests");
    if (!cJSON_IsArray(jArr)) die("requests[] missing");
    const int R_total = (int) (cJSON_IsNumber(jN) ? jN->valuedouble : cJSON_GetArraySize(jArr));

    // Split requests by direction for leaner modeling
    std::vector<int> req_time_OUT, req_time_RET; // minutes
    std::vector<int> req_glob_index_OUT, req_glob_index_RET; // back-mapping
    req_time_OUT.reserve(R_total); req_time_RET.reserve(R_total);
    for (int i=0; i<R_total; ++i){
        const cJSON* ri = cJSON_GetArrayItem(jArr, i);
        const cJSON* d  = cJSON_GetObjectItemCaseSensitive(ri, "dir");
        const cJSON* tm = cJSON_GetObjectItemCaseSensitive(ri, "time");
        if (!cJSON_IsString(d) || !cJSON_IsNumber(tm)) die("bad request row");
        const std::string dir = d->valuestring;
        const int rtime = (int) tm->valuedouble;
        if (dir=="OUT"){ req_time_OUT.push_back(rtime); req_glob_index_OUT.push_back(i); }
        else if (dir=="RET"){ req_time_RET.push_back(rtime); req_glob_index_RET.push_back(i); }
        else die("unknown request dir (expect OUT/RET)");
    }
    const int R_OUT = (int)req_time_OUT.size();
    const int R_RET = (int)req_time_RET.size();

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

        // Assignment variables z and request-level waits/unserved
        // We keep two direction partitions for memory efficiency.
        IloBoolVarArray u_out(env, R_OUT), u_ret(env, R_RET);
        IloNumVarArray  w_out(env, R_OUT), w_ret(env, R_RET);
        for (int i=0; i<R_OUT; ++i){ u_out[i] = IloBoolVar(env); w_out[i] = IloNumVar(env, 0, IloInfinity); }
        for (int i=0; i<R_RET; ++i){ u_ret[i] = IloBoolVar(env); w_ret[i] = IloNumVar(env, 0, IloInfinity); }

        // z^{OUT}[k][i] (only for trips with dir=OUT), z^{RET}[k][i] for dir=RET
        std::vector<IloBoolVarArray> z_out_by_trip, z_ret_by_trip;
        z_out_by_trip.reserve(K); z_ret_by_trip.reserve(K);

        for (int k=0; k<K; ++k){
            if (trips[k].dir==0) { // OUT
                z_out_by_trip.emplace_back(env, R_OUT);
                for (int i=0;i<R_OUT;++i) z_out_by_trip.back()[i] = IloBoolVar(env);
                z_ret_by_trip.emplace_back(env, 0); // empty placeholder
            } else { // RET
                z_out_by_trip.emplace_back(env, 0);
                z_ret_by_trip.emplace_back(env, R_RET);
                for (int i=0;i<R_RET;++i) z_ret_by_trip.back()[i] = IloBoolVar(env);
            }
        }

        // Capacity per trip
        for (int k=0; k<K; ++k){
            if (trips[k].dir==0){
                IloExpr sum(env);
                for (int i=0;i<R_OUT;++i) sum += z_out_by_trip[k][i];
                model.add( sum <= seat_capacity );
                sum.end();
            } else {
                IloExpr sum(env);
                for (int i=0;i<R_RET;++i) sum += z_ret_by_trip[k][i];
                model.add( sum <= seat_capacity );
                sum.end();
            }
        }

        // Each request served at most once (in its direction)
        // Also waiting-time linearization and release-time consistency
        const double A = 50.0; // penalty for unserved
        // Maximum allowed waiting time (minutes)
        const int WAIT_MAX = 30;

        // OUT requests
        for (int i=0;i<R_OUT;++i){
            IloExpr served(env);
            for (int k=0; k<K; ++k){
                if (trips[k].dir==0){
                    served += z_out_by_trip[k][i];
                    // time feasibility + waiting linking
                    model.add( tau[k] >= req_time_OUT[i] - Mtime*(1.0 - z_out_by_trip[k][i]) );
                    // enforce max wait: tau <= ready + WAIT_MAX when assigned
                    model.add( tau[k] <= req_time_OUT[i] + WAIT_MAX + Mtime*(1.0 - z_out_by_trip[k][i]) );
                    model.add( w_out[i] >= tau[k] - req_time_OUT[i] - Mtime*(1.0 - z_out_by_trip[k][i]) );
                }
            }
            model.add( served + u_out[i] == 1 ); // served once or unserved
            served.end();
        }
        // RET requests
        for (int i=0;i<R_RET;++i){
            IloExpr served(env);
            for (int k=0; k<K; ++k){
                if (trips[k].dir==1){
                    served += z_ret_by_trip[k][i];
                    model.add( tau[k] >= req_time_RET[i] - Mtime*(1.0 - z_ret_by_trip[k][i]) );
                    // enforce max wait for RET as well
                    model.add( tau[k] <= req_time_RET[i] + WAIT_MAX + Mtime*(1.0 - z_ret_by_trip[k][i]) );
                    model.add( w_ret[i] >= tau[k] - req_time_RET[i] - Mtime*(1.0 - z_ret_by_trip[k][i]) );
                }
            }
            model.add( served + u_ret[i] == 1 );
            served.end();
        }

        // Objective: minimize A*unserved + sum waiting
        IloExpr obj(env);
        for (int i=0;i<R_OUT;++i){ obj += A * u_out[i] + w_out[i]; }
        for (int i=0;i<R_RET;++i){ obj += A * u_ret[i] + w_ret[i]; }
        model.add(IloMinimize(env, obj));
        obj.end();

        // Parameters (tune as you like)
        cplex.setParam(IloCplex::TiLim, 60.0);        // 60s limit
        cplex.setParam(IloCplex::EpGap, 1e-4);        // MIP gap
        cplex.setParam(IloCplex::Threads, 0);         // all cores
        // cplex.setParam(IloCplex::MIPEmphasis, 1);   // 1=feasibility, 2=optimality

        if (!cplex.solve()){
            std::fprintf(stderr, "subproblem: no solution (status=%d)\n", (int)cplex.getStatus());
            cJSON_Delete(jsub); cJSON_Delete(jreq); env.end();
            return 1;
        }

        const double objval = cplex.getObjValue();
        std::printf("[sp] status=%d obj=%.3f\n", (int)cplex.getStatus(), objval);

        // --------------------------
        // 5) Emit result JSON
        // --------------------------
        cJSON* jout = cJSON_CreateObject();
        cJSON_AddNumberToObject(jout, "objective", objval);
        cJSON_AddNumberToObject(jout, "alpha_unserved", A);

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

        // Served counts by trip
        cJSON* jCap = cJSON_CreateArray();
        for (int k=0; k<K; ++k){
            int served_k = 0;
            if (trips[k].dir==0){
                for (int i=0;i<R_OUT;++i) if (cplex.getValue(z_out_by_trip[k][i]) > 0.5) served_k++;
            } else {
                for (int i=0;i<R_RET;++i) if (cplex.getValue(z_ret_by_trip[k][i]) > 0.5) served_k++;
            }
            cJSON* ck = cJSON_CreateObject();
            cJSON_AddNumberToObject(ck, "q", trips[k].q);
            cJSON_AddNumberToObject(ck, "t", trips[k].t);
            cJSON_AddNumberToObject(ck, "served", served_k);
            cJSON_AddItemToArray(jCap, ck);
        }
        cJSON_AddItemToObject(jout, "served_per_trip", jCap);

        // Totals
        int served_total = 0, unserved_total = 0;
        double waiting_sum = 0.0;
        for (int i=0;i<R_OUT;++i){
            if (cplex.getValue(u_out[i]) > 0.5) unserved_total++;
            else served_total++;
            waiting_sum += cplex.getValue(w_out[i]);
        }
        for (int i=0;i<R_RET;++i){
            if (cplex.getValue(u_ret[i]) > 0.5) unserved_total++;
            else served_total++;
            waiting_sum += cplex.getValue(w_ret[i]);
        }
        cJSON_AddNumberToObject(jout, "served_total", served_total);
        cJSON_AddNumberToObject(jout, "unserved_total", unserved_total);
        cJSON_AddNumberToObject(jout, "waiting_sum", waiting_sum);

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
