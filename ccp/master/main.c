/* Master problem scaffold (C)
 * - Reads merged_master.json containing { base: {...}, demand: {...} }
 * - Generates a per-slot plan over the whole horizon using slot duration from config
 * - Enforces transitions on non-idle actions:
 *     OUT -> RET ; RET -> OUT|CRG ; CRG -> OUT|CRG ; NUL is idle allowed anywhere
 * - Allows idle (NUL) periods in the middle of the sequence
 * - Writes outputs/subproblem.json (path passed as argv[2])
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../subproblem/feas.h"
#include "../subproblem/params.h"
#include "../subproblem/third_party/cjson/cJSON.h"

static void die(const char* msg){ fprintf(stderr, "ERROR: %s\n", msg); exit(1); }

static char* slurp(const char* path){
    FILE* f=fopen(path, "rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char* buf=(char*)malloc(n+1); if(!buf){ fclose(f); return NULL; }
    if (fread(buf,1,n,f)!=(size_t)n){ fclose(f); free(buf); return NULL; }
    buf[n]='\0'; fclose(f); return buf;
}

static cJSON* req_obj(cJSON* root, const char* key){ cJSON* o=cJSON_GetObjectItemCaseSensitive(root,key); if(!cJSON_IsObject(o)) die(key); return o; }
static cJSON* opt_obj(cJSON* root, const char* key){ cJSON* o=cJSON_GetObjectItemCaseSensitive(root,key); return cJSON_IsObject(o)?o:NULL; }
static cJSON* opt_arr(cJSON* root, const char* key){ cJSON* a=cJSON_GetObjectItemCaseSensitive(root,key); return cJSON_IsArray(a)?a:NULL; }
static int req_int(cJSON* root, const char* key){ cJSON* x=cJSON_GetObjectItemCaseSensitive(root,key); if(!cJSON_IsNumber(x)) die(key); return (int)x->valuedouble; }

/* --- Transition + generation helpers --- */
static int transition_ok(const char* prev, const char* next){
    if (strcmp(next, "NUL") == 0) return 1; // idle always OK
    TaskTok p = tok_from_str(prev);
    TaskTok n = tok_from_str(next);
    if (p == TK_OUT) return n == TK_RET;
    if (p == TK_RET) return (n == TK_OUT || n == TK_CRG);
    if (p == TK_CRG) return (n == TK_OUT || n == TK_CRG);
    return 1; // unknown prev -> allow
}

static const char* choose_next_action(const char* last_non_idle, int soc, int trip_distance){
    /* Heuristic selection guided by SoC and transitions */
    const char* opts[4];
    int m = 0;
    if (transition_ok(last_non_idle, "OUT")) opts[m++] = "OUT";
    if (transition_ok(last_non_idle, "RET")) opts[m++] = "RET";
    if (transition_ok(last_non_idle, "CRG")) opts[m++] = "CRG";
    opts[m++] = "NUL"; // always available

    int need_charge = (soc - trip_distance < 0);
    for (int k=0;k<50;k++){
        const char* cand = opts[rand()%m];
        if ((strcmp(cand, "OUT")==0 || strcmp(cand, "RET")==0) && need_charge){
            continue; // avoid negative SoC
        }
        return cand;
    }
    return "NUL";
}

static void generate_full_horizon_sequence(
    cJSON* out_seq_array,
    const char* prev_task,
    int horizon_min,
    int slot_dur,
    int* io_soc,
    int battery_range,
    int trip_distance
){
    int T = (slot_dur > 0) ? (horizon_min / slot_dur) : 0;
    if (T <= 0) T = 1;

    const char* last_non_idle = prev_task;
    int soc = *io_soc;
    for (int t=0; t<T; ++t){
        const char* a = choose_next_action(last_non_idle, soc, trip_distance);
        if (strcmp(a, "OUT")==0 || strcmp(a, "RET")==0){
            if (soc - trip_distance < 0){
                a = "NUL";
            } else {
                soc -= trip_distance;
                last_non_idle = a;
            }
        } else if (strcmp(a, "CRG")==0){
            soc += 35; // same rec_charge constant as subproblem
            if (soc > battery_range) soc = battery_range;
            last_non_idle = a;
        } // NUL: do nothing
        cJSON_AddItemToArray(out_seq_array, cJSON_CreateString(a));
    }
    *io_soc = soc;
}

int main(int argc, char** argv){
    if (argc < 3){
        fprintf(stderr, "usage: master <merged_master.json> <out_subproblem.json> [demand_agg.json]\n");
        return 2;
    }
    srand((unsigned int)time(NULL));

    char* text = slurp(argv[1]);
    if(!text) die("failed to read merged_master.json");
    cJSON* root = cJSON_Parse(text);
    if(!root) die("invalid JSON");

    cJSON* base = req_obj(root, "base");
    cJSON* timeb = req_obj(base, "time");
    cJSON* fleet = req_obj(base, "fleet");
    cJSON* op    = req_obj(base, "operation");
    int horizon_min   = req_int(timeb, "horizon_min");
    int nbr_shuttles  = req_int(fleet, "nbr_shuttles");
    int battery_range = req_int(fleet, "battery_range");
    int seat_capacity = req_int(fleet, "shuttle_capacity");
    int trip_duration = req_int(op, "trip_duration");
    int trip_distance = req_int(op, "trip_distance");

    /* Read aggregated demand per slot if provided */
    int T = (trip_duration>0) ? ( (horizon_min + trip_duration - 1) / trip_duration ) : 1; /* ceil */
    int slot_minutes = trip_duration;
    cJSON* dagg = NULL;
    int* R_out = NULL; int* R_ret = NULL;

    /* Prefer external file if provided as argv[3] */
    if (argc >= 4 && argv[3] && strlen(argv[3]) > 0){
        char* dagg_text = slurp(argv[3]);
        if (!dagg_text){ fprintf(stderr, "[master] warn: failed to read %s, falling back to embedded demand_agg\n", argv[3]); }
        else {
            dagg = cJSON_Parse(dagg_text);
            free(dagg_text);
            if (!dagg || !cJSON_IsObject(dagg)){
                fprintf(stderr, "[master] warn: invalid JSON in %s, falling back to embedded demand_agg\n", argv[3]);
                if (dagg){ cJSON_Delete(dagg); dagg = NULL; }
            }
        }
    }
    if (!dagg){
        dagg = opt_obj(root, "demand_agg");
    }

    if (dagg){
        cJSON* arr_out = opt_arr(dagg, "r_out");
        cJSON* arr_ret = opt_arr(dagg, "r_ret");
        cJSON* smin = cJSON_GetObjectItemCaseSensitive(dagg, "slot_minutes");
        if (cJSON_IsNumber(smin)) slot_minutes = (int)smin->valuedouble;
        if (arr_out && arr_ret){
            int n = cJSON_GetArraySize(arr_out);
            if (n > 0){
                T = n; // trust Python aggregation for slot count
                R_out = (int*)calloc(T, sizeof(int));
                R_ret = (int*)calloc(T, sizeof(int));
                for(int i=0;i<T;i++){
                    cJSON* vo=cJSON_GetArrayItem(arr_out,i);
                    cJSON* vr=cJSON_GetArrayItem(arr_ret,i);
                    R_out[i] = cJSON_IsNumber(vo) ? (int)vo->valuedouble : 0;
                    R_ret[i] = cJSON_IsNumber(vr) ? (int)vr->valuedouble : 0;
                }
            }
        }
    }
    if (!R_out || !R_ret){
        /* Fallback: zero demand */
        R_out = (int*)calloc(T, sizeof(int));
        R_ret = (int*)calloc(T, sizeof(int));
    }

    /* Build subproblem JSON */
    cJSON* out_root = cJSON_CreateObject();
    cJSON_AddNumberToObject(out_root, "nbr_shuttles", nbr_shuttles);
    cJSON* shmap = cJSON_CreateObject();
    cJSON_AddItemToObject(out_root, "shuttles", shmap);

    const int L = trip_distance; // consumption per OUT/RET
    const int Emax = battery_range;
    const int dchg = Emax / 5;   // per-slot charge increment

    /* --- Print a concise summary of inputs for CP master scaffolding --- */
    long sum_out = 0, sum_ret = 0;
    for(int i=0;i<T;i++){ sum_out += R_out[i]; sum_ret += R_ret[i]; }
    printf("[master] slots=%d slot_minutes=%d shuttles=%d seats=%d Emax=%d L=%d dchg=%d demand(out,ret)=(%ld,%ld)\n",
           T, slot_minutes, nbr_shuttles, seat_capacity, Emax, L, dchg, sum_out, sum_ret);

    /* Greedy per-slot assignment to maximize expected served demand */
    int* rem_out = (int*)calloc(T, sizeof(int));
    int* rem_ret = (int*)calloc(T, sizeof(int));
    for(int t=0;t<T;t++){ rem_out[t] = R_out[t]; rem_ret[t] = R_ret[t]; }

    /* Initialize per-shuttle state and JSON arrays */
    char** last_non_idle = (char**)calloc(nbr_shuttles, sizeof(char*));
    int* soc = (int*)calloc(nbr_shuttles, sizeof(int));
    cJSON** seq_arr = (cJSON**)calloc(nbr_shuttles, sizeof(cJSON*));
    int* placed_first_out = (int*)calloc(nbr_shuttles, sizeof(int));
    int* placed_final_ret = (int*)calloc(nbr_shuttles, sizeof(int));
    for(int i=0;i<nbr_shuttles;i++){
        last_non_idle[i] = "NONE";
        soc[i] = Emax;
        char key[16]; snprintf(key,sizeof(key),"S%d", i);
        cJSON* Sj = cJSON_CreateObject();
        cJSON_AddItemToObject(shmap, key, Sj);
        seq_arr[i] = cJSON_CreateArray();
        cJSON_AddItemToObject(Sj, "seq", seq_arr[i]);
    }

    long served_out_total = 0, served_ret_total = 0;
    for(int t=0;t<T;t++){
        for(int i=0;i<nbr_shuttles;i++){
            const char* a = "NUL";
            int can_out = (soc[i] - L >= 0) && (strcmp(last_non_idle[i], "OUT") != 0);
            int must_ret = (strcmp(last_non_idle[i], "OUT") == 0);
            int can_ret = (soc[i] - L >= 0) && must_ret;
            int can_crg = (strcmp(last_non_idle[i], "OUT") != 0);
            int slots_left = T - t;

            if (placed_final_ret[i]){
                a = "NUL"; // remain idle after final RET
            } else if (slots_left == 1){
                // Ensure last non-idle is RET if possible
                if (must_ret && can_ret){ a = "RET"; placed_final_ret[i] = 1; }
                else { a = "NUL"; }
            } else if (slots_left == 2){
                if (must_ret){
                    if (can_ret){ a = "RET"; placed_final_ret[i] = 1; }
                    else { a = "NUL"; }
                } else {
                    // Aim for OUT now to allow RET in the last slot
                    if (!placed_first_out[i]){
                        if (can_out){ a = "OUT"; placed_first_out[i] = 1; }
                        else if (can_crg && soc[i] < Emax){ a = "CRG"; }
                        else { a = "NUL"; }
                    } else if (can_out){
                        a = "OUT";
                    } else if (can_crg && soc[i] < Emax){
                        a = "CRG";
                    } else { a = "NUL"; }
                }
            } else if (must_ret){
                // Must close an OUT with RET before doing anything else
                if (can_ret){ a = "RET"; }
                else { a = "NUL"; }
            } else if (!placed_first_out[i]){
                // Enforce first non-idle is OUT (may idle or charge until feasible)
                if (can_out){ a = "OUT"; placed_first_out[i] = 1; }
                else if (can_crg && soc[i] < Emax){ a = "CRG"; }
                else { a = "NUL"; }
            } else {
                // Greedy: serve OUT demand if any, else charge if needed, else idle
                int gain_out = can_out ? (rem_out[t] > 0 ? (rem_out[t] < seat_capacity ? rem_out[t] : seat_capacity) : 0) : 0;
                if (gain_out > 0){ a = "OUT"; }
                else if (can_crg && soc[i] < Emax){ a = "CRG"; }
                else { a = "NUL"; }
            }

            if (strcmp(a, "OUT")==0){
                soc[i] -= L; if (soc[i] < 0){ a = "NUL"; soc[i] += L; }
                else {
                    int served = seat_capacity; if (served > rem_out[t]) served = rem_out[t];
                    rem_out[t] -= served; served_out_total += served; last_non_idle[i] = "OUT";
                }
            }
            if (strcmp(a, "RET")==0){
                soc[i] -= L; if (soc[i] < 0){ a = "NUL"; soc[i] += L; }
                else {
                    int served = seat_capacity; if (served > rem_ret[t]) served = rem_ret[t];
                    rem_ret[t] -= served; served_ret_total += served; last_non_idle[i] = "RET";
                    // If we placed RET with 1 slot left or marked earlier, commit final-ret status
                    if (slots_left <= 2) placed_final_ret[i] = 1;
                }
            }
            if (strcmp(a, "CRG")==0){ soc[i] += dchg; if (soc[i] > Emax) soc[i] = Emax; last_non_idle[i] = "CRG"; }

            cJSON_AddItemToArray(seq_arr[i], cJSON_CreateString(a));
        }
    }

    printf("[master] served_out_total=%ld served_ret_total=%ld\n", served_out_total, served_ret_total);

    free(rem_out); free(rem_ret);
    free(last_non_idle); free(soc); free(seq_arr);
    free(placed_first_out); free(placed_final_ret);

    /* write output */
    char* out_text = cJSON_PrintBuffered(out_root, 2, 0);
    if(!out_text) die("print json");
    const char* out_path = argv[2];
    FILE* fo = fopen(out_path, "wb");
    if(!fo) die("open out path");
    fwrite(out_text, 1, strlen(out_text), fo);
    fclose(fo);

    /* Print sequences to stdout for convenience */
    printf("[master] sequences (per shuttle):\n");
    cJSON* out_shmap = cJSON_GetObjectItem(out_root, "shuttles");
    for(int i=0;i<nbr_shuttles;i++){
        char key[16]; snprintf(key,sizeof(key),"S%d", i);
        cJSON* Sj = cJSON_GetObjectItem(out_shmap, key);
        cJSON* arr = Sj ? cJSON_GetObjectItem(Sj, "seq") : NULL;
        printf("  %s: [", key);
        if (cJSON_IsArray(arr)){
            int n = cJSON_GetArraySize(arr);
            for(int k=0;k<n;k++){
                cJSON* it = cJSON_GetArrayItem(arr, k);
                const char* s = cJSON_IsString(it) ? it->valuestring : "?";
                printf("%s%s", s, (k==n-1?"":","));
            }
        }
        printf("]\n");
    }

    free(out_text);
    cJSON_Delete(out_root);
    /* If dagg was parsed from external file, it is separate from root */
    if (dagg && dagg != opt_obj(root, "demand_agg")){
        /* Only delete if it wasn't borrowed from root */
        /* In borrowed case, it will be deleted with root */
        cJSON_Delete(dagg);
    }
    cJSON_Delete(root);
    free(text);

    printf("[master] wrote %s\n", out_path);
    free(R_out); free(R_ret);
    return 0;
}
