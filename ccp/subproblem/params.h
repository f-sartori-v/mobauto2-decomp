#ifndef PARAMS_H
#define PARAMS_H

typedef struct {
    int horizon_min;
    int cap;            // base.fleet.shuttle_capacity
    int trip_dist;      // base.operation.trip_distance
    int battery_range;  // base.fleet.battery_range
    int nbr_shuttles;   // base.fleet.nbr_shuttles
} Params;

typedef struct {
    char **seq;   // tokens
    int T;        // |seq|
    int soc0;
    int delay;
    char prev[8]; // "OUT"/"RET"/"CRG"
} Shuttle;

typedef struct {
    Shuttle *arr;
    int n;
} Fleet;

typedef struct {
    char dir[4];  // "OUT" or "RET"
    int ready;    // [0, horizon]
} Request;

typedef struct {
    Request *arr;
    int n;
} DemandPool;

/* ownership/cleanup */
void free_shuttle(Shuttle *s);
void free_fleet(Fleet *F);
void free_demand(DemandPool *D);

#endif
