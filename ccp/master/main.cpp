// CP Master scaffolding (C++): read YAML config and demand_agg.json,
// echo a summary, copy the demand file next to the output, and write a stub subproblem.json.

#include <ilcp/cp.h>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>   // snprintf
#include <cstdlib>  // free

extern "C" {
#include "../subproblem/third_party/cjson/cJSON.h"
}

static std::string slurp(const std::string& path){
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        std::cerr << "[cp-master] error: cannot open file: " << path << "\n";
        return std::string();
    }
    std::ostringstream ss; ss << ifs.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 4){
        std::cerr << "usage: master_cp <config.yaml> <demand_agg.json> <out_subproblem.json>\n";
        return 2;
    }

    const std::string cfg_path   = argv[1];
    const std::string demand_path= argv[2];
    const std::string out_path   = argv[3];

    // Read YAML config
    YAML::Node base;
    try {
        base = YAML::LoadFile(cfg_path);
    } catch (const std::exception& e) {
        std::cerr << "[cp-master] error: failed to load YAML config '" << cfg_path << "': " << e.what() << "\n";
        return 2;
    }
    YAML::Node time   = base["time"];
    YAML::Node fleet  = base["fleet"];
    YAML::Node op     = base["operation"];
    YAML::Node solver = base["solver"];

    if (!time || !fleet || !op || !solver){
        std::cerr << "[cp-master] error: config must contain nodes: time, fleet, operation, solver\n";
        return 2;
    }

    if (!time["horizon_min"])   { std::cerr << "[cp-master] error: missing time.horizon_min in config\n";   return 2; }
    if (!op["trip_duration"])   { std::cerr << "[cp-master] error: missing operation.trip_duration in config\n"; return 2; }
    if (!op["trip_distance"])   { std::cerr << "[cp-master] error: missing operation.trip_distance in config\n"; return 2; }
    if (!fleet["nbr_shuttles"]) { std::cerr << "[cp-master] error: missing fleet.nbr_shuttles in config\n";  return 2; }
    if (!fleet["shuttle_capacity"]) { std::cerr << "[cp-master] error: missing fleet.shuttle_capacity in config\n"; return 2; }
    if (!fleet["battery_range"]) { std::cerr << "[cp-master] error: missing fleet.battery_range in config\n"; return 2; }
    if (!solver["time_limit"])  { std::cerr << "[cp-master] error: missing solver.time_limit in config\n";   return 2; }
    if (!solver["search_type"]) { std::cerr << "[cp-master] error: missing solver.search_type in config\n";   return 2; }

    const int horizon_min   = time["horizon_min"].as<int>();
    const int trip_duration = op["trip_duration"].as<int>();
    const int trip_distance = op["trip_distance"].as<int>();
    const int nbr_shuttles  = fleet["nbr_shuttles"].as<int>();
    const int seat_capacity = fleet["shuttle_capacity"].as<int>();
    const int battery_range = fleet["battery_range"].as<int>();
    const int time_limit    = solver["time_limit"].as<int>();
    const std::string search_type = solver["search_type"].as<std::string>();
    const int sp_time_limit = solver["subproblem_time_limit"] ? solver["subproblem_time_limit"].as<int>() : 300;
    const double trip_cost = solver["trip_cost"] ? solver["trip_cost"].as<double>() : 1e-3; // small penalty per trip

    // --- demand_agg.json ---
    std::string dagg_txt = slurp(demand_path);
    if (dagg_txt.empty()) {
        std::cerr << "[cp-master] error: empty or unreadable demand file: " << demand_path << "\n";
        return 3;
    }
    cJSON* dagg = cJSON_Parse(dagg_txt.c_str());
    if (!dagg){ std::cerr << "invalid demand_agg.json\n"; return 3; }

    cJSON* smin    = cJSON_GetObjectItemCaseSensitive(dagg, "slot_minutes");
    cJSON* arr_out = cJSON_GetObjectItemCaseSensitive(dagg, "r_out");
    cJSON* arr_ret = cJSON_GetObjectItemCaseSensitive(dagg, "r_ret");
    if (!cJSON_IsArray(arr_out) || !cJSON_IsArray(arr_ret)) {
        std::cerr << "missing arrays r_out/r_ret in demand_agg.json\n";
        cJSON_Delete(dagg); return 3;
    }
    const int num_slots    = cJSON_GetArraySize(arr_out);
    if (cJSON_GetArraySize(arr_ret) != num_slots){
        std::cerr << "r_ret size mismatch\n";
        cJSON_Delete(dagg); return 3;
    }
    const int slot_minutes = cJSON_IsNumber(smin) ? (int)smin->valuedouble : trip_duration;

    long sum_out = 0, sum_ret = 0;
    for (int i=0;i<num_slots;i++){
        cJSON* vo=cJSON_GetArrayItem(arr_out,i);
        cJSON* vr=cJSON_GetArrayItem(arr_ret,i);
        sum_out += cJSON_IsNumber(vo) ? (long)vo->valuedouble : 0;
        sum_ret += cJSON_IsNumber(vr) ? (long)vr->valuedouble : 0;
    }

    std::cout << "[cp-master] cfg: T=" << num_slots
              << " slot_min=" << slot_minutes
              << " shuttles=" << nbr_shuttles
              << " seats=" << seat_capacity
              << " Emax="  << battery_range
              << " L="     << trip_distance
              << " demand(out,ret)=(" << sum_out << "," << sum_ret << ")\n";

    // Create solver environment
    IloEnv env;
    int exit_code = 0;

    try {
      // Create the model
      IloModel model(env);
      IloCP cp(env);

      const int T = num_slots;       // demand slots
      const int Tact = T + 1;        // action slots (one extra to serve last-slot demand)
      const int Q = nbr_shuttles;
      const int S = seat_capacity;

      // --- Battery params (read from YAML; fallback shown) ---
      const int Emax     = battery_range;           // km-equivalent (E^{max})
      const int Lleg     = trip_distance;           // km-equivalent per leg
      const int DeltaChg = base["operation"]["delta_chg"] ? base["operation"]["delta_chg"].as<int>()
                                                          : (battery_range / 5); // 5 slots = full

      // --- Build demand caps from demand_agg.json ---
      std::vector<int> RhatOut(T, 0), RhatRet(T, 0);
      for (int t = 0; t < T; ++t) {
        cJSON *vo = cJSON_GetArrayItem(arr_out, t);
        cJSON *vr = cJSON_GetArrayItem(arr_ret, t);
        RhatOut[t] = cJSON_IsNumber(vo) ? (int)vo->valuedouble : 0;
        RhatRet[t] = cJSON_IsNumber(vr) ? (int)vr->valuedouble : 0;
      }

      // --- Decision vars ---
      std::vector<std::vector<IloBoolVar>> xOUT(Q, std::vector<IloBoolVar>(Tact));
      std::vector<std::vector<IloBoolVar>> xRET(Q, std::vector<IloBoolVar>(Tact));
      std::vector<std::vector<IloBoolVar>> xCHR(Q, std::vector<IloBoolVar>(Tact));

      std::vector<std::vector<IloIntVar>>  s(Q,   std::vector<IloIntVar>(Tact + 1)); // 0..Tact
      std::vector<std::vector<IloBoolVar>> idlL(Q, std::vector<IloBoolVar>(Tact));   // 0..Tact-1
      std::vector<std::vector<IloBoolVar>> zL(Q,   std::vector<IloBoolVar>(Tact + 1)); // 0..Tact

      for (int q = 0; q < Q; ++q) {
        // location state 0..Tact
        for (int t = 0; t <= Tact; ++t) {
          s[q][t] = IloIntVar(env, 0, 1);
          s[q][t].setName(("s_" + std::to_string(q) + "_" + std::to_string(t)).c_str());
        }
        model.add(s[q][0] == 0);
        model.add(s[q][Tact] == 0);

        // zL init
        zL[q][0] = IloBoolVar(env);
        zL[q][0].setName(("zL_" + std::to_string(q) + "_0").c_str());
        model.add(zL[q][0] == 0);

        // decisions and helpers 0..Tact-1
        for (int t = 0; t < Tact; ++t) {
          xOUT[q][t] = IloBoolVar(env);
          xOUT[q][t].setName(("xOUT_" + std::to_string(q) + "_" + std::to_string(t)).c_str());
          xRET[q][t] = IloBoolVar(env);
          xRET[q][t].setName(("xRET_" + std::to_string(q) + "_" + std::to_string(t)).c_str());
          xCHR[q][t] = IloBoolVar(env);
          xCHR[q][t].setName(("xCHR_" + std::to_string(q) + "_" + std::to_string(t)).c_str());

          // ≤ 1 task per slot
          model.add(xOUT[q][t] + xRET[q][t] + xCHR[q][t] <= 1);

          // balance + feasible actions by location
          model.add(s[q][t + 1] == s[q][t] + xOUT[q][t] - xRET[q][t]);
          model.add(xOUT[q][t] <= 1 - s[q][t]); // OUT only at L
          model.add(xRET[q][t] <= s[q][t]);     // RET only at M
          model.add(xCHR[q][t] <= 1 - s[q][t]); // CRG only at L

          // idle-at-L: 1 iff at L and no task chosen
          idlL[q][t] = IloBoolVar(env);
          idlL[q][t].setName(("idlL_" + std::to_string(q) + "_" + std::to_string(t)).c_str());
          model.add(idlL[q][t] <= 1 - s[q][t]);
          model.add(idlL[q][t] <= 1 - (xOUT[q][t] + xRET[q][t] + xCHR[q][t]));
          model.add(idlL[q][t] >= 1 - s[q][t] - (xOUT[q][t] + xRET[q][t] + xCHR[q][t]));

          // CRG-before-IDL ordering at L: after an idle-at-L in this block, no more CRG before next OUT
          model.add(xCHR[q][t] + zL[q][t] <= 1);

          // advance zL to next slot (reset on RET, persist otherwise, trigger on idle-at-L)
          zL[q][t + 1] = IloBoolVar(env);
          zL[q][t + 1].setName(("zL_" + std::to_string(q) + "_" + std::to_string(t + 1)).c_str());
          model.add(zL[q][t + 1] <= 1 - xRET[q][t]);
          model.add(zL[q][t + 1] >= zL[q][t] - xRET[q][t]);
          model.add(zL[q][t + 1] >= idlL[q][t]);
        }
        // Earliest service starts at t=1: enforce idle at t=0 (no OUT/RET/CRG)
        model.add(idlL[q][0] == 1);
        model.add(xOUT[q][0] == 0);
        model.add(xRET[q][0] == 0);
      }

      // --- Battery state (linear, capped) ---
      std::vector<std::vector<IloIntVar>> b(Q, std::vector<IloIntVar>(Tact + 1));
      std::vector<std::vector<IloIntVar>> g(Q, std::vector<IloIntVar>(Tact));
      for (int q = 0; q < Q; ++q) {
        for (int t = 0; t <= Tact; ++t) {
          b[q][t] = IloIntVar(env, 0, Emax);
          b[q][t].setName(("b_" + std::to_string(q) + "_" + std::to_string(t)).c_str());
        }
        model.add(b[q][0] == Emax);
        for (int t = 0; t < Tact; ++t) {
          g[q][t] = IloIntVar(env, 0, DeltaChg);
          g[q][t].setName(("gchg_" + std::to_string(q) + "_" + std::to_string(t)).c_str());
          model.add(b[q][t + 1] == b[q][t] - Lleg * (xOUT[q][t] + xRET[q][t]) + g[q][t]);
          model.add(g[q][t] <= DeltaChg * xCHR[q][t]);  // charge only if CRG
          model.add(g[q][t] <= Emax - b[q][t]);         // cap at Emax
          model.add(g[q][t] >= xCHR[q][t]);             // if CRG chosen, must add at least 1 unit (no CRG at full)
          model.add(b[q][t] >= Lleg * (xOUT[q][t] + xRET[q][t])); // enough to start a leg
        }
      }

      // --- Served demand and objective ---
      // Enforce a one-slot lag with an extra action slot to serve last demand
      std::vector<IloIntVar> rOut(T), rRet(T);
      for (int t = 0; t < T; ++t) {
        rOut[t] = IloIntVar(env, 0, RhatOut[t]);
        rOut[t].setName(("rOut_" + std::to_string(t)).c_str());
        rRet[t] = IloIntVar(env, 0, RhatRet[t]);
        rRet[t].setName(("rRet_" + std::to_string(t)).c_str());
        model.add(rOut[t] <= RhatOut[t]);
        model.add(rRet[t] <= RhatRet[t]);
      }
      // Now link capacity at action slot t≥1 to demand at slot t-1
      for (int t = 0; t < Tact; ++t) {
        IloExpr capOut(env), capRet(env);
        for (int q = 0; q < Q; ++q) {
          capOut += S * xOUT[q][t];
          capRet += S * xRET[q][t];
        }
        if (t > 0 && (t - 1) < T) {
          model.add(rOut[t - 1] <= capOut);
          model.add(rRet[t - 1] <= capRet);
        }
        capOut.end();
        capRet.end();
      }
      IloExpr totalServed(env);
      for (int t = 0; t < T; ++t) totalServed += rOut[t] + rRet[t];
      // Small per-trip penalty to avoid unnecessary trips
      IloExpr tripCount(env);
      for (int t = 0; t < Tact; ++t) {
        for (int q = 0; q < Q; ++q) tripCount += xOUT[q][t] + xRET[q][t];
      }
      model.add(IloMaximize(env, totalServed - trip_cost * tripCount));
      totalServed.end();
      tripCount.end();

      // --- Solve ---
      cp.setParameter(IloCP::TimeLimit, time_limit);
      // Map search_type from config to CP Optimizer parameter, if recognized
      if (search_type == "auto") {
          // default; do nothing
      } else if (search_type == "depth_first") {
          cp.setParameter(IloCP::SearchType, IloCP::DepthFirst);
      } else if (search_type == "restart") {
          cp.setParameter(IloCP::SearchType, IloCP::Restart);
      } else if (search_type == "multi_point") {
          cp.setParameter(IloCP::SearchType, IloCP::MultiPoint);
      } else {
          std::cerr << "[cp-master] warn: unknown search_type '" << search_type << "' (using default)\n";
      }
      // cp.setParameter(IloCP::LogVerbosity, IloCP::Verbose);
      cp.extract(model);
      bool ok = cp.solve();
      if (!ok) {
        std::cerr << "[cp-master] no solution\n";
        exit_code = 1;
      } else {
        std::cout << "[cp-master] obj=" << (long long)cp.getObjValue() << "\n";
      }

      if (exit_code == 0) {
        // Generate subproblem.json (sequence per shuttle/slot)
        cJSON *sub = cJSON_CreateObject();
        cJSON_AddNumberToObject(sub, "nbr_shuttles", Q);
        cJSON_AddNumberToObject(sub, "num_slots", Tact);
        cJSON_AddNumberToObject(sub, "sp_time_limit", sp_time_limit);
        cJSON *shmap = cJSON_CreateObject();
        cJSON_AddItemToObject(sub, "shuttles", shmap);
        for (int q = 0; q < Q; ++q) {
          char key[16]; snprintf(key, sizeof(key), "S%d", q);
          cJSON *Sj = cJSON_CreateObject();
          cJSON_AddItemToObject(shmap, key, Sj);
          cJSON *arr = cJSON_CreateArray();
          for (int t = 0; t < Tact; ++t) {
            const char *lab = "NUL";
            if (cp.getValue(xOUT[q][t]) > 0.5) lab = "OUT";
            else if (cp.getValue(xRET[q][t]) > 0.5) lab = "RET";
            else if (cp.getValue(xCHR[q][t]) > 0.5) lab = "CRG";
            cJSON_AddItemToArray(arr, cJSON_CreateString(lab));
          }
          cJSON_AddItemToObject(Sj, "seq", arr);
        }
        // Write file
        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs.good()) {
            std::cerr << "[cp-master] error: cannot open output file: " << out_path << "\n";
        }
        char *text = cJSON_PrintBuffered(sub, 2, 0);
        if (text && ofs.good()) { ofs << text; }
        if (text) { free(text); }
        cJSON_Delete(sub);
      }

    } catch (IloException& e) {
        std::cerr << "IloException: " << e << std::endl;
        exit_code = 1;
    } catch (const std::exception& e) {
        std::cerr << "std::exception: " << e.what() << std::endl;
        exit_code = 1;
    }
    env.end();

    // copy demand_agg used (optional)
    try {
        std::string out_dir = out_path;
        auto pos = out_dir.find_last_of('/');
        if (pos != std::string::npos) out_dir = out_dir.substr(0, pos);
        std::ifstream src(demand_path, std::ios::binary);
        std::ofstream dst(out_dir + "/demand_agg.used.json", std::ios::binary);
        dst << src.rdbuf();
    } catch (...) {
        std::cerr << "[cp-master] warn: failed to copy demand_agg\n";
    }

    cJSON_Delete(dagg);
    return exit_code;
}
