import json, random
from typing import Dict, Any
from pathlib import Path

candidates = [
    ["NUL"],
    ["OUT"],
    ["RET"],
    ["CRG"],
    ["OUT", "RET"],
    ["RET", "OUT"],
    ["RET", "CRG"],
    ["OUT", "RET", "OUT"],
    ["OUT", "RET", "CRG"],
    ["RET", "OUT", "RET"],
    ["RET", "CRG", "OUT"],
    ["CRG", "OUT", "RET"],
    ["OUT", "RET", "OUT", "RET"],
    ["OUT", "RET", "CRG", "OUT"],
    ["RET", "OUT", "RET", "CRG"],
    ["RET", "CRG", "OUT", "RET"],
    ["CRG", "OUT", "RET", "OUT"],
    ["CRG", "OUT", "RET", "CRG"],
    ["OUT", "RET", "OUT", "RET", "CRG"],
    ["OUT", "RET", "CRG", "OUT", "RET"],
    ["RET", "OUT", "RET", "CRG", "OUT"],
    ["RET", "CRG", "OUT", "RET", "OUT"],
    ["RET", "CRG", "OUT", "RET", "CRG"],
    ["CRG", "OUT", "RET", "OUT", "RET"],
    ["CRG", "OUT", "RET", "CRG", "OUT"]
]

def write_fake_input(out_path: Path, nbr_shuttles: int = 2):
    sub_obj: Dict[str, Any] = {"nbr_shuttles": nbr_shuttles, "shuttles": {}}

    for i in range(nbr_shuttles):
        seq = random.choice(candidates)
        soc0 = random.choice([30, 60, 90, 120, 150, 150])
        delay_window = 0 if random.choice([0, 0, 1]) == 0 else random.randint(0, 30)
        previous_task = random.choice(["OUT", "RET", "CRG"])

        sub_obj["shuttles"][f"S{i}"] = {
            "seq": seq,
            "soc0": soc0,
            "prev_task": previous_task,
            "delay": delay_window,
        }

    out_path.write_text(json.dumps(sub_obj, indent=2), encoding="utf-8")
    return out_path

def write_demand_files(demand: int, in_path: Path) -> Path:
    reqs = [
        {"dir": random.choice(["OUT", "RET"]), "time": random.randint(0, 120)}
        for _ in range(demand)
    ]
    obj = {"nreq": demand, "requests": reqs}

    in_path.write_text(json.dumps(obj, indent=2), encoding="utf-8")
    return in_path
