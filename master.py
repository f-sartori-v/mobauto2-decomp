from __future__ import annotations

import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

import yaml

from subproblem import candidates, write_demand_files
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


def check_time_feas(horizon_min: int, delay: int, T: int, trip_duration: int) -> bool:
    if trip_duration <= 0:
        return False
    delay = max(0, int(delay))
    return delay + T * trip_duration <= horizon_min


def check_battery_feas(soc0: int, Emax: int, seq: List[str], cons_trip: int = 28, rec_charge: int = 35) -> Tuple[bool, int]:
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
        delay = rng.choice([0, 0, 0, rng.randint(0, 20)])
        soc0 = rng.choice([60, 90, 120, 150])

        # try up to N attempts
        seq: List[str] | None = None
        for _ in range(50):
            cand = rng.choice(candidates)
            if not cand:
                continue
            if not check_prev_vs_first(prev, cand[0]):
                continue
            if not check_time_feas(cfg.time.horizon_min, delay, len(cand), cfg.operation.trip_duration):
                continue
            feas, _ = check_battery_feas(soc0, cfg.fleet.battery_range, cand, cons_trip=cfg.operation.trip_distance)
            if not feas:
                continue
            seq = cand
            break
        if seq is None:
            # Fall back to a minimal idle sequence
            seq = ["NUL"]

        sub_obj["shuttles"][f"S{i}"] = {
            "seq": seq,
            "soc0": soc0,
            "prev_task": prev,
            "delay": delay,
        }

    return sub_obj


def solve_master(iterations: int = 1, seed: int | None = None) -> bool:
    cfg = load_cfg(CONFIG)

    demand_base = ROOT / "outputs" / "demand_base.json"
    # simple random demand level for now; master can evolve later
    rng = random.Random(seed)
    demand_level = 2 ** rng.randint(0, 10)
    demand_path = write_demand_files(demand_level, demand_base)

    # basic single-iteration loop scaffolding
    best_ok = False
    for it in range(max(1, iterations)):
        sub_in = choose_feasible_sequences(cfg, seed=rng.randint(0, 10**9))

        in_path = ROOT / "outputs" / "subproblem.json"
        in_path.write_text(json.dumps(sub_in, indent=2), encoding="utf-8")

        # keep track of last master state
        (ROOT / "outputs").mkdir(exist_ok=True)
        (ROOT / "outputs" / "master_state.json").write_text(
            json.dumps({
                "iter": it,
                "subproblem": sub_in,
                "demand_file": str(demand_path),
                "cfg": {
                    "horizon_min": cfg.time.horizon_min,
                    "trip_duration": cfg.operation.trip_duration,
                    "battery_range": cfg.fleet.battery_range,
                    "trip_distance": cfg.operation.trip_distance,
                },
            }, indent=2),
            encoding="utf-8",
        )

        ok = run_binary_with_config(C_BIN, in_path, demand_path, CONFIG)
        best_ok = best_ok or ok
        # In the future, parse subproblem results and add cuts/columns

    return best_ok


__all__ = [
    "solve_master",
]

# --- Local copy of runner to avoid circular imports ---
def run_binary_with_config(bin_path: Path, cfg_path: Path, demand_path: Path, base_yaml_path: Path = CONFIG) -> bool:
    base = yaml.safe_load(Path(base_yaml_path).read_text(encoding="utf-8"))
    subp = json.loads(Path(cfg_path).read_text(encoding="utf-8"))
    dem = json.loads(Path(demand_path).read_text(encoding="utf-8"))

    merged = {
        "base": base,
        "subproblem": subp,
        "demand": dem,
    }

    merged_path = bin_path.parent / "merged.json"
    merged_path.write_text(json.dumps(merged, indent=2), encoding="utf-8")

    argv = [str(bin_path), str(merged_path)]
    print(f"[run] {' '.join(argv)}")
    t0 = time.perf_counter()
    try:
        rc = subprocess.run(argv, check=False).returncode
    except subprocess.TimeoutExpired:
        print(f"[time] subproblem TIMEOUT after {time.perf_counter() - t0:.3f}s")
        return False
    print(f"[time] subproblem ran in {time.perf_counter() - t0:.3f}s, exit={rc}")
    return rc == 0
