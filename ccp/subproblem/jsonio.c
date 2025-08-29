#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "third_party/cjson/cJSON.h"
#include "params.h"

static void die(const char* m){ fprintf(stderr,"ERROR: %s\n", m); exit(1); }

static char* slurp(const char* path){
    FILE* f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    char* s=(char*)malloc((size_t)n+1); if(!s){fclose(f);return NULL;}
    size_t r=fread(s,1,(size_t)n,f); fclose(f); s[r]='\0'; return s;
}

/* ---------- cleanup ---------- */
void free_shuttle(Shuttle *s){
    if(!s) return;
    for(int i=0;i<s->T;i++) free(s->seq[i]);
    free(s->seq);
    s->seq=NULL; s->T=0;
}
void free_fleet(Fleet *F){
    if(!F) return;
    for(int i=0;i<F->n;i++) free_shuttle(&F->arr[i]);
    free(F->arr); F->arr=NULL; F->n=0;
}
void free_demand(DemandPool *D){
    if(!D) return;
    free(D->arr); D->arr=NULL; D->n=0;
}

/* ---------- parse helpers ---------- */
static cJSON* req_obj(cJSON* parent, const char* key){
    cJSON* x=cJSON_GetObjectItemCaseSensitive(parent,key);
    if(!cJSON_IsObject(x)) die(key);
    return x;
}
static int req_int(cJSON* parent, const char* key){
    cJSON* x=cJSON_GetObjectItemCaseSensitive(parent,key);
    if(!cJSON_IsNumber(x)) die(key);
    return (int)x->valuedouble;
}
static const char* req_str(cJSON* parent, const char* key){
    cJSON* x=cJSON_GetObjectItemCaseSensitive(parent,key);
    if(!cJSON_IsString(x)) die(key);
    return x->valuestring;
}

/* ---------- public: parse merged.json ---------- */
void parse_params_and_fleet(const char* merged_path, Params* P, Fleet* F, DemandPool* D){
    char* text=slurp(merged_path); if(!text) die("cannot read merged.json");
    cJSON* root=cJSON_Parse(text); if(!root) die("invalid JSON");

    cJSON* base=req_obj(root,"base");
    cJSON* time=req_obj(base,"time");
    cJSON* fleet=req_obj(base,"fleet");
    cJSON* oper=req_obj(base,"operation");

    P->horizon_min   = req_int(time,"horizon_min");
    P->cap           = req_int(fleet,"shuttle_capacity");
    P->battery_range = req_int(fleet,"battery_range");
    P->trip_dist     = req_int(oper,"trip_distance");
    P->nbr_shuttles  = req_int(fleet,"nbr_shuttles");

    cJSON* subp=req_obj(root,"subproblem");
    int sub_n = req_int(subp,"nbr_shuttles");
    if (sub_n != P->nbr_shuttles) {
        fprintf(stderr,"WARN: base.fleet.nbr_shuttles=%d vs subproblem.nbr_shuttles=%d\n",
                P->nbr_shuttles, sub_n);
    }
    cJSON* sh=req_obj(subp,"shuttles");

    F->n = sub_n;
    F->arr = (Shuttle*)calloc(F->n, sizeof(Shuttle));
    for(int i=0;i<F->n;i++){
        char key[16]; snprintf(key,sizeof(key),"S%d",i);
        cJSON* Si = cJSON_GetObjectItemCaseSensitive(sh,key);
        if(!cJSON_IsObject(Si)) die("missing shuttle block");

        cJSON* seq = cJSON_GetObjectItemCaseSensitive(Si,"seq");
        if(!cJSON_IsArray(seq)) die("seq");
        int T=cJSON_GetArraySize(seq);
        F->arr[i].T=T;
        F->arr[i].seq=(char**)calloc(T,sizeof(char*));
        for(int t=0;t<T;t++){
            cJSON* tok=cJSON_GetArrayItem(seq,t);
            if(!cJSON_IsString(tok)) die("seq token");
            const char* s=tok->valuestring;
            F->arr[i].seq[t]=(char*)malloc(strlen(s)+1);
            strcpy(F->arr[i].seq[t], s);
        }
        F->arr[i].soc0  = req_int(Si,"soc0");
        F->arr[i].delay = req_int(Si,"delay");
        const char* prev = req_str(Si,"prev_task");
        strncpy(F->arr[i].prev, prev, sizeof(F->arr[i].prev)-1);
        F->arr[i].prev[sizeof(F->arr[i].prev)-1] = '\0';
    }

    cJSON* demand=req_obj(root,"demand");
    cJSON* reqs=cJSON_GetObjectItemCaseSensitive(demand,"requests");
    if(!cJSON_IsArray(reqs)) die("demand.requests");

    int n=cJSON_GetArraySize(reqs);
    D->n=n;
    D->arr=(Request*)calloc(n,sizeof(Request));
    for(int j=0;j<n;j++){
        cJSON* r=cJSON_GetArrayItem(reqs,j);
        if(!cJSON_IsObject(r)) die("request");
        const char* dir = NULL;
        cJSON* ddir=cJSON_GetObjectItemCaseSensitive(r,"dir");
        if(cJSON_IsString(ddir)) dir=ddir->valuestring; else dir="OUT";
        strncpy(D->arr[j].dir, dir, sizeof(D->arr[j].dir)-1);
        cJSON* rdy = cJSON_GetObjectItemCaseSensitive(r,"ready");
        if(!cJSON_IsNumber(rdy)){
            rdy = cJSON_GetObjectItemCaseSensitive(r,"time"); // tolerate "time"
        }
        D->arr[j].ready = cJSON_IsNumber(rdy) ? (int)rdy->valuedouble : 0;
    }

    cJSON_Delete(root);
    free(text);
}
