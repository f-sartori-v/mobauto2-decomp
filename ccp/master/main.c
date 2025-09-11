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
        fprintf(stderr, "usage: master <merged_master.json> <out_subproblem.json>\n");
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
    int trip_duration = req_int(op, "trip_duration");
    int trip_distance = req_int(op, "trip_distance");

    /* Read aggregated demand per slot if provided */
    int T = (trip_duration>0) ? ( (horizon_min + trip_duration - 1) / trip_duration ) : 1; /* ceil */
    int slot_minutes = trip_duration;
    cJSON* dagg = opt_obj(root, "demand_agg");
    int* R_out = NULL; int* R_ret = NULL;
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
    printf("[master] slots=%d slot_minutes=%d shuttles=%d Emax=%d L=%d dchg=%d demand(out,ret)=(%ld,%ld)\n",
           T, slot_minutes, nbr_shuttles, Emax, L, dchg, sum_out, sum_ret);

    for(int i=0;i<nbr_shuttles;i++){
        const char* prev = "NONE"; // no initial prev; first non-idle must be OUT
        int soc0 = Emax; // start full unless specified otherwise
        int delay = 0;   // idle handled via NUL

        char key[16]; snprintf(key,sizeof(key),"S%d", i);
        cJSON* Sj = cJSON_CreateObject();
        cJSON_AddItemToObject(shmap, key, Sj);
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToObject(Sj, "seq", arr);
        int soc_work = soc0;
        // Generate full-horizon plan following rules; prefer OUT as first non-idle and RET as last non-idle
        // We use demand aggregates if available to bias choices (not yet used in this scaffold)
        int slot_minutes = trip_duration;
        int TT = T;
        const char* last_non_idle = prev;
        int placed_first_out = 0;
        int placed_final_ret = 0;
        for(int t=0;t<TT;t++){
            int slots_left = TT - t;
            const char* a = "NUL";

            // Tail planning: ensure last non-idle is RET if possible
            if (!placed_final_ret && slots_left == 1){
                if (strcmp(last_non_idle, "OUT")==0 && soc_work - L >= 0){
                    a = "RET"; placed_final_ret = 1;
                } else {
                    a = "NUL";
                }
            } else if (!placed_final_ret && slots_left == 2) {
                if (strcmp(last_non_idle, "OUT")==0){
                    // OUT -> RET
                    if (soc_work - L >= 0){ a = "RET"; placed_final_ret = 1; }
                    else a = "NUL";
                } else {
                    // aim for OUT now to allow RET next
                    if (soc_work - L >= 0) a = "OUT"; else a = "CRG";
                }
            } else {
                // Mid-horizon logic
                if (!placed_first_out){
                    if (soc_work - L >= 0) { a = "OUT"; placed_first_out = 1; }
                    else { a = "CRG"; }
                } else {
                    // choose respecting transitions
                    if (strcmp(last_non_idle, "OUT")==0){
                        if (soc_work - L >= 0) a = "RET"; else a = "NUL";
                    } else if (strcmp(last_non_idle, "RET")==0){
                        if (soc_work - L >= 0) a = (rand()%2? "OUT":"CRG"); else a = "CRG";
                    } else if (strcmp(last_non_idle, "CRG")==0){
                        if (soc_work - L >= 0) a = (rand()%2? "OUT":"CRG"); else a = "CRG";
                    } else { // NONE
                        if (soc_work - L >= 0) a = "OUT"; else a = "CRG";
                    }
                }
            }

            // Apply SoC update and keep last_non_idle state
            if (strcmp(a, "OUT")==0 || strcmp(a, "RET")==0){
                if (soc_work - L < 0){ a = "NUL"; }
                else { soc_work -= L; last_non_idle = a; }
            } else if (strcmp(a, "CRG")==0){
                soc_work += dchg; if (soc_work > Emax) soc_work = Emax; last_non_idle = a;
            }
            // If we just placed RET and want it to be the final non-idle, mark and force remaining to NUL
            if (strcmp(a, "RET")==0 && (slots_left <= 2)) placed_final_ret = 1;

            cJSON_AddItemToArray(arr, cJSON_CreateString(a));
        }
    }

    /* write output */
    char* out_text = cJSON_PrintBuffered(out_root, 2, 0);
    if(!out_text) die("print json");
    const char* out_path = argv[2];
    FILE* fo = fopen(out_path, "wb");
    if(!fo) die("open out path");
    fwrite(out_text, 1, strlen(out_text), fo);
    fclose(fo);

    free(out_text);
    cJSON_Delete(out_root);
    cJSON_Delete(root);
    free(text);

    printf("[master] wrote %s\n", out_path);
    free(R_out); free(R_ret);
    return 0;
}
