from __future__ import annotations

import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple, Optional

import yaml
from subproblem import generate_scenarios_from_agg
import platform, subprocess, time

# Local paths (avoid importing from main to prevent cycles)
ROOT = Path(__file__).resolve().parents[0]
CONFIG = ROOT / "configs" / "base.yaml"
if platform.system().lower() == "windows":
    C_BIN = ROOT / "ccp" / "subproblem.exe"
else:
    C_BIN = ROOT / "ccp" / "subproblem.out"


@dataclass
class TimeCfg:
    horizon_min: int
    step_res: int


@dataclass
class FleetCfg:
    nbr_shuttles: int
    shuttle_capacity: int
    battery_range: int


@dataclass
class OperCfg:
    trip_duration: int
    trip_distance: int


@dataclass
class Cfg:
    time: TimeCfg
    fleet: FleetCfg
    operation: OperCfg


def load_cfg(cfg_path: Path = CONFIG) -> Cfg:
    base = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    time = base["time"]
    fleet = base["fleet"]
    op = base["operation"]
    return Cfg(
        time=TimeCfg(horizon_min=int(time["horizon_min"]), step_res=int(time["step_res"])),
        fleet=FleetCfg(
            nbr_shuttles=int(fleet["nbr_shuttles"]),
            shuttle_capacity=int(fleet["shuttle_capacity"]),
            battery_range=int(fleet["battery_range"]),
        ),
        operation=OperCfg(
            trip_duration=int(op["trip_duration"]),
            trip_distance=int(op["trip_distance"]),
        ),
    )


# --- Feasibility helpers mirrored from C ---
def check_prev_vs_first(prev: str, first: str) -> bool:
    if prev == "OUT":
        return first == "RET"
    if prev == "RET":
        return first in ("CRG", "OUT")
    if prev == "CRG":
        return first in ("CRG", "OUT")
    return True


def check_battery_feas(Emax: int, seq: List[str], soc0: int = 150, cons_trip: int = 28, rec_charge: int = 30) -> Tuple[bool, int]:
    soc = int(soc0)
    for tok in seq:
        if tok in ("OUT", "RET"):
            soc -= cons_trip
        elif tok == "CRG":
            soc += rec_charge
        # If battery dips below 0, infeasible (mirror of C code)
        if soc < 0:
            return False, soc
        # We ignore soc > Emax similar to the current C check
    return True, soc


def choose_feasible_sequences(cfg: Cfg, seed: int | None = None) -> Dict[str, Dict]:
    rng = random.Random(seed)

    sub_obj: Dict[str, Dict] = {"nbr_shuttles": cfg.fleet.nbr_shuttles, "shuttles": {}}
    for i in range(cfg.fleet.nbr_shuttles):
        # Strategy: sample until feasible under simple checks
        prev = "RET"  # matches generator in subproblem.py

        # try up to N attempts
        seq: List[str] | None = None
        for _ in range(50):
            cand = rng.choice(candidates)
            if not cand:
                continue
            if not check_prev_vs_first(prev, cand[0]):
                continue

            feas, _ = check_battery_feas(cfg.fleet.battery_range, cand, cons_trip=cfg.operation.trip_distance)
            if not feas:
                continue
            seq = cand
            break
        if seq is None:
            # Fall back to a minimal idle sequence
            seq = ["NUL"]

        sub_obj["shuttles"][f"S{i}"] = {
            "seq": seq,
            "prev_task": prev,
        }

    return sub_obj


def solve_master(iterations: int = 1, seed: int | None = None) -> bool:
    """Legacy helper: now generates aggregated demand and scenarios only.

    - Writes outputs/demand_agg.json with slot-wise random counts.
    - Generates 10 scenarios uniformly within slots.
    """
    cfg = load_cfg(CONFIG)
    rng = random.Random(seed)

    slot_minutes = cfg.operation.trip_duration
    T = max(1, (cfg.time.horizon_min + slot_minutes - 1) // slot_minutes)
    cap = cfg.fleet.shuttle_capacity
    r_out = [rng.randint(0, cap + 3) for _ in range(T)]
    r_ret = [rng.randint(0, cap + 3) for _ in range(T)]

    (ROOT / "outputs").mkdir(exist_ok=True)
    demand_agg_path = ROOT / "outputs" / "demand_agg.json"
    sum_out = int(sum(r_out))
    sum_ret = int(sum(r_ret))
    demand_agg = {
        "slots": T,
        "slot_minutes": slot_minutes,
        "r_out": r_out,
        "r_ret": r_ret,
        "sum_out": sum_out,
        "sum_ret": sum_ret,
        "sum_all": int(sum_out + sum_ret),
    }
    demand_agg_path.write_text(json.dumps(demand_agg, indent=2), encoding="utf-8")

    # Generate scenarios from aggregated demand
    scen_dir = ROOT / "outputs" / "scenarios"
    generate_scenarios_from_agg(demand_agg_path, scen_dir, num_scenarios=10, seed=seed)

    # State dump
    (ROOT / "outputs" / "master_state.json").write_text(
        json.dumps({
            "iter": 0,
            "demand_agg_file": str(demand_agg_path),
            "cfg": {
                "horizon_min": cfg.time.horizon_min,
                "trip_duration": cfg.operation.trip_duration,
                "battery_range": cfg.fleet.battery_range,
                "trip_distance": cfg.operation.trip_distance,
                "shuttle_capacity": cfg.fleet.shuttle_capacity,
            },
        }, indent=2),
        encoding="utf-8",
    )

    return True


__all__ = [
    "solve_master",
]

# --- Local copy of runner to avoid circular imports ---
def run_binary_with_config(bin_path: Path, cfg_path: Path, demand_path: Path, base_yaml_path: Path = CONFIG) -> bool:
    """Run C++ subproblem: <subproblem.json> <requests.json> <out.json>."""
    out_path = Path(__file__).resolve().parents[0] / "outputs" / "subproblem_result.json"
    argv = [str(bin_path), str(cfg_path), str(demand_path), str(out_path)]
    print(f"[run] {' '.join(argv)}")
    t0 = time.perf_counter()
    try:
        rc = subprocess.run(argv, check=False).returncode
    except subprocess.TimeoutExpired:
        print(f"[time] subproblem TIMEOUT after {time.perf_counter() - t0:.3f}s")
        return False
    print(f"[time] subproblem ran in {time.perf_counter() - t0:.3f}s, exit={rc}")
    if rc == 0:
        print(f"[subproblem] wrote {out_path}")
    return rc == 0


# --- Demand aggregation (exported) ---
def aggregate_demand(
    demand_path: Path,
    base_yaml_path: Path = CONFIG,
    slot_minutes: Optional[int] = None,
) -> Dict[str, object]:
    """Aggregate base demand into time slots.

    - Reads config for horizon and default slot duration unless slot_minutes is provided.
    - Returns a dict with keys: slots, slot_minutes, r_out, r_ret.
    """
    base = yaml.safe_load(Path(base_yaml_path).read_text(encoding="utf-8"))
    horizon = int(base["time"]["horizon_min"])
    if slot_minutes is None:
        slot_minutes = int(base["operation"]["trip_duration"])  # default slot size

    data = json.loads(Path(demand_path).read_text(encoding="utf-8"))
    T = max(1, (horizon + slot_minutes - 1) // slot_minutes)
    r_out = [0 for _ in range(T)]
    r_ret = [0 for _ in range(T)]

    for req in data.get("requests", []):
        tmin = int(req.get("ready", req.get("time", 0)))
        dirn = req.get("dir", "OUT")
        idx = tmin // slot_minutes
        if idx < 0:
            idx = 0
        if idx >= T:
            idx = T - 1
        if dirn == "RET":
            r_ret[idx] += 1
        else:
            r_out[idx] += 1

    return {
        "slots": T,
        "slot_minutes": int(slot_minutes),
        "r_out": r_out,
        "r_ret": r_ret,
    }
