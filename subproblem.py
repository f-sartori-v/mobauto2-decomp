import json, random, yaml
from pathlib import Path


def minutes(hh_mm: str) -> int:
    h, m = map(int, hh_mm.split(":"))
    return 60*h + m

def solve_windows(
        scheme: list,
        slots: int,
        base_start="07:00",
):
    duration = 30
    slots = len(scheme)
    t = minutes(base_start)
    end_time = t + duration * slots


def write_demand_files(demand: int, in_path: Path) -> Path:
    # Determine horizon from config to bound request times
    root = Path(__file__).resolve().parents[0]
    cfg_path = root / "configs" / "base.yaml"
    try:
        base = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
        horizon = int(base.get("time", {}).get("horizon_min", 120))
    except Exception:
        horizon = 120
    hi = max(0, horizon - 1)

    reqs = [
        {"dir": random.choice(["OUT", "RET"]), "time": random.randint(0, hi)}
        for _ in range(demand)
    ]
    obj = {"nreq": demand, "requests": reqs}

    in_path.write_text(json.dumps(obj, indent=2), encoding="utf-8")
    return in_path
