from __future__ import annotations
import json, random
from pathlib import Path


def generate_scenarios_from_agg(demand_agg_path: Path, out_dir: Path, num_scenarios: int = 5, seed: int | None = None) -> list[Path]:
    """Generate random request scenarios that exactly match aggregated slot counts.

    - Reads `demand_agg.json` with keys: r_out, r_ret, slot_minutes.
    - For each scenario i, creates `scenario{i}.json` under `out_dir` with fields:
      { nreq, requests: [{dir: OUT|RET, time: int}, ...] }.
    - Request times are uniformly sampled within each slot's minute range.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    data = json.loads(Path(demand_agg_path).read_text(encoding="utf-8"))
    r_out = list(map(int, data.get("r_out", [])))
    r_ret = list(map(int, data.get("r_ret", [])))
    slot_minutes = int(data.get("slot_minutes", 30))
    T = max(len(r_out), len(r_ret))
    rng = random.Random(seed)

    paths: list[Path] = []
    for sidx in range(1, num_scenarios + 1):
        reqs = []
        # OUT requests per slot
        for t in range(T):
            a = t * slot_minutes
            b = (t + 1) * slot_minutes - 1
            for _ in range(r_out[t] if t < len(r_out) else 0):
                reqs.append({"dir": "OUT", "time": rng.randint(a, b)})
        # RET requests per slot
        for t in range(T):
            a = t * slot_minutes
            b = (t + 1) * slot_minutes - 1
            for _ in range(r_ret[t] if t < len(r_ret) else 0):
                reqs.append({"dir": "RET", "time": rng.randint(a, b)})

        # Sort requests by time (earliest first)
        reqs.sort(key=lambda r: int(r.get("time", 0)))
        obj = {"nreq": len(reqs), "requests": reqs}
        out_path = out_dir / f"scenario{sidx}.json"
        out_path.write_text(json.dumps(obj, indent=2), encoding="utf-8")
        paths.append(out_path)
    return paths
