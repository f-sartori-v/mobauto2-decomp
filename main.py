# em src/main.py
from pathlib import Path
from subproblem import generate_scenarios_from_agg
from master import aggregate_demand
import platform, shutil, subprocess, os, yaml, json, time, argparse, random, math, csv

ROOT = Path(__file__).resolve().parents[0]
CONFIG = ROOT / "configs" / "base.yaml"
# Always prefer C++ entrypoints
C_SRC = ROOT / "ccp" / "subproblem" / "main.cpp"
M_CP_SRC = ROOT / "ccp" / "master" / "main.cpp"

CJSON_DIR = ROOT / "ccp" / "subproblem" / "third_party" / "cjson"
CJSON_SRC = CJSON_DIR / "cJSON.c"
CJSON_OBJ = CJSON_DIR / "cJSON.o"
JSONIO_SRC = ROOT / "ccp" / "subproblem" / "jsonio.c"
FEAS_SRC = ROOT / "ccp" / "subproblem"/ "feas.c"


# platform-aware output binary
if platform.system().lower() == "windows":
    C_BIN = ROOT / "ccp" / "subproblem.exe"
else:
    C_BIN = ROOT / "ccp" / "subproblem.out"
if platform.system().lower() == "windows":
    M_CP_BIN = ROOT / "ccp" / "master.exe"
else:
    M_CP_BIN = ROOT / "ccp" / "master.out"

def build_subproblem(C_SRC: Path, C_OUT: Path):
    """Build C++ subproblem (Concert C++), linking cJSON and CPLEX/Concert.

    Currently configured for macOS and Linux via CPLEX_STUDIO. Windows not yet wired.
    """
    sys = platform.system().lower()

    if not CJSON_SRC.exists():
        print(f"[ERROR] missing {CJSON_SRC}. Put cJSON.c/.h in {CJSON_DIR}")
        return False

    if sys == "windows":
        print("[ERROR] C++ subproblem build not configured for Windows yet.")
        return False

    studio = os.getenv("CPLEX_STUDIO", "/Applications/CPLEX_Studio2211")

    if sys == "darwin":  # macOS
        if not shutil.which("clang++"):
            print("[ERROR] clang++ not found on macOS.")
            return False
        c_comp = shutil.which("clang") or shutil.which("cc")
        if not c_comp:
            print("[ERROR] C compiler (clang/cc) not found on macOS.")
            return False
        # 1) Build cJSON.c as a C object
        cmd_c = (
            f'{c_comp} -O2 -std=c11 '
            f'-I"{CJSON_DIR}" '
            f'-c "{CJSON_SRC}" -o "{CJSON_OBJ}"'
        )
        print(f"[build] {cmd_c}")
        if subprocess.call(cmd_c, shell=True) != 0:
            return False
        # 2) Link C++ with the produced object
        cmd = (
            f'clang++ -O2 -std=c++17 '
            f'-I"{studio}/concert/include" '
            f'-I"{studio}/cplex/include" '
            f'-I"{CJSON_DIR}" '
            f'"{C_SRC}" "{CJSON_OBJ}" '
            f'-L"{studio}/concert/lib/x86-64_osx/static_pic" '
            f'-L"{studio}/cplex/lib/x86-64_osx/static_pic" '
            f'-lconcert -lilocplex -lcplex -lpthread -lm '
            f'-o "{C_OUT}"'
        )
    else:  # linux
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            print("[ERROR] g++/clang++ not found on Linux.")
            return False
        cc = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
        if not cc:
            print("[ERROR] gcc/clang/cc not found on Linux.")
            return False
        # 1) Build cJSON.c as C object
        cmd_c = (
            f'{cc} -O2 -std=c11 '
            f'-I"{CJSON_DIR}" '
            f'-c "{CJSON_SRC}" -o "{CJSON_OBJ}"'
        )
        print(f"[build] {cmd_c}")
        if subprocess.call(cmd_c, shell=True) != 0:
            return False
        # 2) Link C++ with object
        cmd = (
            f'{cxx} -O2 -std=c++17 '
            f'-I"{studio}/concert/include" '
            f'-I"{studio}/cplex/include" '
            f'-I"{CJSON_DIR}" '
            f'"{C_SRC}" "{CJSON_OBJ}" '
            f'-L"{studio}/concert/lib/x86-64_linux/static_pic" '
            f'-L"{studio}/cplex/lib/x86-64_linux/static_pic" '
            f'-lconcert -lilocplex -lcplex -lpthread -lm -ldl '
            f'-o "{C_OUT}"'
        )

    print(f"[build] {cmd}")
    return subprocess.call(cmd, shell=True) == 0

def build_master(M_CP_SRC: Path, M_OUT: Path):
    sys = platform.system().lower()
    if sys != "darwin":
        print("[ERROR] CP master build currently configured for macOS only.")
        return False
    if not shutil.which("clang++"):
        print("[ERROR] clang++ not found on macOS.")
        return False
    if not M_CP_SRC.exists():
        print(f"[ERROR] missing {M_CP_SRC}")
        return False
    studio = os.getenv("CPLEX_STUDIO", "/Applications/CPLEX_Studio2211")
    # Build cJSON.c to object first (as C), then link with C++ master
    c_comp = shutil.which("clang") or shutil.which("cc")
    if not c_comp:
        print("[ERROR] C compiler (clang/cc) not found on macOS.")
        return False
    cmd_c = (
        f'{c_comp} -O2 -std=c11 '
        f'-I"{CJSON_DIR}" '
        f'-c "{CJSON_SRC}" -o "{CJSON_OBJ}"'
    )
    print(f"[build] {cmd_c}")
    if subprocess.call(cmd_c, shell=True) != 0:
        return False
    cmd = (
        f'clang++ -O2 -std=c++17 '
        f'-I"{studio}/concert/include" '
        f'-I"{studio}/cplex/include" '
        f'-I"{CJSON_DIR}" '
        f'"{M_CP_SRC}" "{CJSON_OBJ}" '
        f'-L"{studio}/concert/lib/x86-64_osx/static_pic" '
        f'-L"{studio}/cplex/lib/x86-64_osx/static_pic" '
        f'-lconcert -lilocplex -lcplex -lyaml-cpp -lpthread -lm '
        f'-o "{M_OUT}"'
    )
    print(f"[build] {cmd}")
    return subprocess.call(cmd, shell=True) == 0

def ensure_built() -> bool:
    if not C_BIN.exists():
        ok = build_subproblem(C_SRC, C_BIN)
        if not ok:
            print("[FAIL] build failed")
            return False
    return True

def ensure_master_built() -> bool:
    if not M_CP_BIN.exists():
        ok = build_master(M_CP_SRC, M_CP_BIN)
        if not ok:
            print("[FAIL] master build failed")
            return False
    return True

def _load_base_cfg() -> dict:
    try:
        return yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    except Exception:
        return {}

def _resolve_path(p: str | Path) -> Path:
    pth = Path(p)
    return pth if pth.is_absolute() else (ROOT / pth)

def _is_unset_path(v) -> bool:
    if v is None:
        return True
    if isinstance(v, bool):
        return True
    if isinstance(v, str) and v.strip().lower() in ("", "none", "null", "false"):
        return True
    return False

def _json_mode_from_yaml() -> bool:
    base = _load_base_cfg() or {}
    D = (base.get("data") or {})
    # json mode if explicitly set OR if load_demand is explicitly false
    ld = D.get("load_demand")
    ld_off = (isinstance(ld, bool) and not ld) or (isinstance(ld, str) and ld.strip().lower() in ("false", "no", "0"))
    return bool(D.get("json", False)) or ld_off

def _scenario_file_from_yaml() -> Path | None:
    """Return scenario file path from YAML, resolved under data/scenarios/ when relative.

    Precedence from configs/base.yaml:
      1) data.json == true and data.json_file set → use that path
      2) data.demand_file if set (JSON/CSV)
      3) scenarios.file (legacy key)

    If the chosen path is absolute, use it as-is. If it is relative and starts
    with data/scenarios/, resolve from repo root. Otherwise, treat it as a
    filename under ROOT/data/scenarios/.
    """
    base = _load_base_cfg() or {}
    D = (base.get("data") or {})
    S = (base.get("scenarios") or {})
    use_json = _json_mode_from_yaml()
    if use_json:
        cand = D.get("json_file")
        if _is_unset_path(cand):
            cand = S.get("file")
    else:
        cand = D.get("demand_file")
        if _is_unset_path(cand):
            cand = S.get("file")
    fname = cand
    if not fname:
        return None
    p = Path(str(fname))
    if p.is_absolute():
        return p
    # If caller already provides data/scenarios relative path, honor it
    parts = p.parts
    if len(parts) >= 2 and parts[0] == "data" and parts[1] == "scenarios":
        return ROOT / p
    # Otherwise, interpret as a file living in data/scenarios/
    return ROOT / "data" / "scenarios" / p

def _read_scenario_from_csv(csv_path: Path) -> dict:
    """Parse a scenario CSV into a single-scenario JSON object.

    Expected columns (case-insensitive best-effort):
      - direction: one of dir, direction, type → values OUT/RET (accepts variations)
      - time: one of time, ready, ready_min, start_min, minute, t

    Rows missing either field are skipped. Direction is normalized to OUT/RET.
    """
    def norm_dir(v: str) -> str | None:
        if not isinstance(v, str):
            return None
        s = v.strip().lower()
        if s in ("out", "outbound", "o"): return "OUT"
        if s in ("ret", "return", "inbound", "r", "retour"): return "RET"
        return None

    time_keys = ("time", "ready", "ready_min", "start_min", "minute", "t")
    dir_keys = ("dir", "direction", "type")

    reqs: list[dict] = []
    with csv_path.open("r", encoding="utf-8") as f:
        rdr = csv.DictReader(f)
        # Early normalize header keys to lowercase for convenience
        for row in rdr:
            row_l = { (k.lower() if isinstance(k, str) else k): v for k, v in row.items() }
            # direction
            dval = None
            for k in dir_keys:
                if k in row_l and row_l[k] not in (None, ""):
                    dval = norm_dir(row_l[k])
                    break
            # time
            tval = None
            for k in time_keys:
                if k in row_l and row_l[k] not in (None, ""):
                    try:
                        tval = int(float(row_l[k]))
                    except Exception:
                        tval = None
                    break
            if dval is None or tval is None:
                continue
            reqs.append({"dir": dval, "time": int(tval)})

    return {"nreq": len(reqs), "requests": reqs}

def _time_res_from_scenario(scen_path: Path) -> int:
    """Determine step size (minutes) from scenario JSON.

    Priority:
      1) meta.slot_minutes if present
      2) if combined {scenarios:[...]}, first scenario meta.slot_minutes
      3) infer as GCD of request times deltas (>0) or default 30
    """
    try:
        j = json.loads(scen_path.read_text(encoding="utf-8"))
    except Exception:
        return 30
    # 1) meta.slot_minutes
    def get_meta(obj: dict):
        m = obj.get("meta") if isinstance(obj, dict) else None
        if isinstance(m, dict) and "slot_minutes" in m:
            try:
                return int(m.get("slot_minutes"))
            except Exception:
                return None
        return None
    if isinstance(j, dict) and isinstance(j.get("scenarios"), list) and j["scenarios"]:
        m = get_meta(j["scenarios"][0])
        if isinstance(m, int) and m > 0:
            return m
    else:
        m = get_meta(j)
        if isinstance(m, int) and m > 0:
            return m
    # 2) infer from request times
    import math as _math
    times: list[int] = []
    def add_times(obj: dict):
        for r in (obj.get("requests") or []):
            try:
                t = int(r.get("time", r.get("ready", 0)))
            except Exception:
                t = 0
            if t >= 0:
                times.append(t)
    if isinstance(j, dict) and isinstance(j.get("scenarios"), list) and j["scenarios"]:
        for s in j["scenarios"]:
            if isinstance(s, dict):
                add_times(s)
    elif isinstance(j, dict):
        add_times(j)
    times = sorted(set(times))
    if len(times) >= 2:
        diffs = [b - a for a, b in zip(times, times[1:]) if (b - a) > 0]
        if diffs:
            g = diffs[0]
            for d in diffs[1:]:
                g = _math.gcd(g, d)
            if g > 0:
                return g
    return 30

def _last_request_min_from_json(j: dict) -> int:
    last = 0
    if isinstance(j, dict) and isinstance(j.get("scenarios"), list):
        for s in j.get("scenarios"):
            for r in (s.get("requests") or []):
                try:
                    t = int(r.get("time", r.get("ready", 0)))
                except Exception:
                    t = 0
                if t > last:
                    last = t
    else:
        for r in (j.get("requests") or []):
            try:
                t = int(r.get("time", r.get("ready", 0)))
            except Exception:
                t = 0
            if t > last:
                last = t
    return last

def _compute_horizon_from_json(scen_path: Path, step_res: int) -> int:
    try:
        j = json.loads(scen_path.read_text(encoding="utf-8"))
    except Exception:
        return step_res
    last = _last_request_min_from_json(j)
    # End time is the next multiple of step_res strictly after last request time
    end_time = ((last // step_res) + 1) * step_res
    return int(end_time)

def _flatten_scenarios_to_requests(src: Path, dst: Path) -> None:
    """Ensure JSON at dst has a top-level 'requests' array by flattening any 'scenarios' list."""
    try:
        j = json.loads(src.read_text(encoding="utf-8"))
    except Exception as e:
        raise RuntimeError(f"failed reading {src}: {e}")
    if isinstance(j, dict) and isinstance(j.get("scenarios"), list):
        reqs = []
        for s in j.get("scenarios"):
            reqs.extend(list(s.get("requests") or []))
        out = {"nreq": len(reqs), "requests": reqs}
    else:
        out = j
    dst.write_text(json.dumps(out, indent=2), encoding="utf-8")

def _run_dir_for_input(inp: Path) -> Path:
    """Return outputs/<input_stem> ensuring the directory exists."""
    name = inp.stem
    d = ROOT / "outputs" / name
    d.mkdir(parents=True, exist_ok=True)
    return d

def _scen_cfg_from_yaml() -> tuple[Path, Path, int, bool]:
    """Read scenario-gen configuration from configs/base.yaml.

    Returns: (demand_agg_path, scenarios_dir, num_scenarios, random_minutes)
    Fallbacks to defaults if keys are missing.
    """
    base = _load_base_cfg() or {}
    S = (base.get("scenarios") or {})
    dagg = _resolve_path(S.get("demand_agg_path", "outputs/demand_agg.json"))
    sdir = _resolve_path(S.get("dir", "outputs/scenarios_random"))
    try:
        num = int(S.get("num", 5))
    except Exception:
        num = 5
    rnd = bool(S.get("random_minutes", True))
    return dagg, sdir, num, rnd

def run_binary_with_config(bin_path: Path, cfg_path: Path, demand_path: Path, base_yaml_path: Path = CONFIG) -> bool:
    """Run subproblem binary.

    For C++ subproblem (main.cpp): expects 3 args: <subproblem.json> <requests.json> <out.json>.
    """
    out_path = ROOT / "outputs" / "subproblem_result.json"
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

def run_subproblem_with_out(sub_in_path: Path, req_path: Path, out_path: Path, warm_start_path: Path | None = None) -> bool:
    """Run C++ subproblem with explicit output path.

    Args:
        sub_in_path: path to outputs/subproblem.json (sequence per shuttle).
        req_path: scenario requests JSON file.
        out_path: output JSON file path for this run.
    """
    argv = [str(C_BIN), str(sub_in_path), str(req_path), str(out_path)]
    if warm_start_path is not None:
        argv.append(str(warm_start_path))
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

def master_flow(scenario_path: Path | None = None):
    # Ensure master binary is available (master-only run)
    if not ensure_master_built():
        return False

    # 1) Prepare demand aggregation input for master
    demand_agg_path, _, _, _ = _scen_cfg_from_yaml()
    cfg_for_master = CONFIG
    # If an explicit scenario is provided or JSON mode + YAML scenario is set,
    # derive horizon and aggregate from that JSON/CSV.
    scen_file = scenario_path if scenario_path is not None else _scenario_file_from_yaml()
    if scen_file is not None:
        (ROOT / "outputs").mkdir(exist_ok=True)
        step_res = _time_res_from_scenario(scen_file)
        horizon_min = _compute_horizon_from_json(scen_file, step_res)
        # Build a temporary YAML config overriding time.horizon_min
        try:
            base = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
        except Exception:
            base = {}
        if "time" not in base or not isinstance(base.get("time"), dict):
            base["time"] = {}
        base["time"]["horizon_min"] = int(horizon_min)
        tmp_cfg = ROOT / "outputs" / "config.horizon.auto.yaml"
        tmp_cfg.write_text(yaml.safe_dump(base, sort_keys=False), encoding="utf-8")
        cfg_for_master = tmp_cfg
        # Ensure aggregator gets a single requests[] object (flatten scenarios if needed)
        flat_path = ROOT / "outputs" / "requests.flat.json"
        try:
            _flatten_scenarios_to_requests(scen_file, flat_path)
        except Exception as e:
            print(f"[master] failed to prepare requests: {e}")
            return False
        # Aggregate demand using the computed horizon and step size
        try:
            dagg = aggregate_demand(flat_path, base_yaml_path=cfg_for_master, slot_minutes=step_res)
        except Exception as e:
            print(f"[master] aggregate_demand failed: {e}")
            return False
        demand_agg_path = ROOT / "outputs" / "demand_agg.json"
        demand_agg_path.write_text(json.dumps(dagg, indent=2), encoding="utf-8")
    # Otherwise, require pre-existing aggregated demand path
    if scen_file is None and not demand_agg_path.exists():
        print(
            f"[master] missing aggregated demand file: {demand_agg_path}. "
            "Set configs/base.yaml:scenarios.demand_agg_path or enable data.json/json_file."
        )
        return False

    # 2) Build merged_master.json for the C master under outputs/<input_name>/
    base = yaml.safe_load(cfg_for_master.read_text(encoding="utf-8"))
    demand_agg = json.loads(demand_agg_path.read_text(encoding="utf-8"))
    dem = {"note": "agg-only mode; no base demand file"}

    merged_master = {"base": base, "demand": dem, "demand_agg": demand_agg}
    # Decide folder name by the input reference (scenario file if provided, otherwise the agg file)
    naming_input = scen_file if scen_file is not None else demand_agg_path
    run_dir = _run_dir_for_input(naming_input)
    merged_master_path = run_dir / "merged_master.json"
    merged_master_path.write_text(json.dumps(merged_master, indent=2), encoding="utf-8")

    # 3) Run C++ master to produce outputs/subproblem.json
    sub_in_path = ROOT / "outputs" / "subproblem.json"
    # Pass config and aggregated demand file to the C++ master
    argv = [str(M_CP_BIN), str(cfg_for_master), str(demand_agg_path), str(sub_in_path)]
    print(f"[run] {' '.join(argv)}")
    rc = subprocess.run(argv, check=False).returncode
    if rc != 0:
        print(f"[master] C master exited {rc}")
        return False
    print(f"[master] generated subproblem input at {sub_in_path}")
    return True

def scenarios_flow(
    n: int = 5,
    seed: int | None = None,
    random_minutes: bool = True,
    max_iters: int | None = None,
    tol: float | None = None,
    sleep_sec: float | None = None,
) -> bool:
    """Generate N scenarios from the demand file and iterate each.

    Behavior:
      - If a demand file is configured (data.json=true or data.load_demand=false with data.json_file,
        or scenarios.file), aggregate it to outputs/demand_agg.json.
      - Otherwise, use configs/base.yaml:scenarios.demand_agg_path if it exists.
      - Run master once to produce outputs/subproblem.json (sequence skeleton).
      - Generate N scenarios from demand_agg into scenarios.dir.
      - For each scenario, run iterate_flow(scenario_path=that_scenario).
    """
    (ROOT / "outputs").mkdir(exist_ok=True)

    # Determine scenario source or aggregated demand
    dagg_cfg, sdir_cfg, n_cfg, rnd_cfg = _scen_cfg_from_yaml()
    demand_agg_path = dagg_cfg
    scen_file = _scenario_file_from_yaml()

    if scen_file is not None:
        # Derive aggregated demand from provided scenario file
        step_res = _time_res_from_scenario(scen_file)
        horizon_min = _compute_horizon_from_json(scen_file, step_res)
        try:
            base = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
        except Exception:
            base = {}
        if "time" not in base or not isinstance(base.get("time"), dict):
            base["time"] = {}
        base["time"]["horizon_min"] = int(horizon_min)
        tmp_cfg = ROOT / "outputs" / "config.horizon.auto.yaml"
        tmp_cfg.write_text(yaml.safe_dump(base, sort_keys=False), encoding="utf-8")
        flat_path = ROOT / "outputs" / "requests.flat.json"
        try:
            _flatten_scenarios_to_requests(scen_file, flat_path)
        except Exception as e:
            print(f"[scenarios] failed to prepare requests: {e}")
            return False
        try:
            dagg = aggregate_demand(flat_path, base_yaml_path=tmp_cfg, slot_minutes=step_res)
        except Exception as e:
            print(f"[scenarios] aggregate_demand failed: {e}")
            return False
        demand_agg_path = ROOT / "outputs" / "demand_agg.json"
        demand_agg_path.write_text(json.dumps(dagg, indent=2), encoding="utf-8")
    elif not demand_agg_path.exists():
        print(
            f"[scenarios] missing aggregated demand file: {demand_agg_path}. "
            "Set data.json/json_file in YAML or provide scenarios.demand_agg_path."
        )
        return False

    # Ensure subproblem input exists via master (respect scenario if provided)
    sub_in_path = ROOT / "outputs" / "subproblem.json"
    if not sub_in_path.exists():
        print("[scenarios] generating subproblem.json via master")
        if not master_flow(scenario_path=scen_file):
            return False

    # Generate scenarios (prefer YAML defaults unless overridden by args)
    scen_dir = sdir_cfg
    if random_minutes is None:
        random_minutes = rnd_cfg
    if n is None:
        n = n_cfg
    scen_paths = generate_scenarios_from_agg(
        demand_agg_path,
        scen_dir,
        num_scenarios=int(n),
        seed=seed,
        random_minutes=bool(random_minutes),
    )
    print(
        f"[scenarios] generated {len(scen_paths)} in {scen_dir} (random_minutes={'on' if random_minutes else 'off'})"
    )

    # Emit merged.json per-scenario under outputs/<scenario_name>/ for C subproblem users
    try:
        sub_in_for_merge = ROOT / "outputs" / "subproblem.json"
        sub_obj_for_merge = json.loads(sub_in_for_merge.read_text(encoding="utf-8")) if sub_in_for_merge.exists() else {}
        for scen_path in scen_paths:
            run_dir = _run_dir_for_input(scen_path)
            # base cfg with auto horizon from scenario
            step_res = _time_res_from_scenario(scen_path)
            horizon_min = _compute_horizon_from_json(scen_path, step_res)
            try:
                base_cfg = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
            except Exception:
                base_cfg = {}
            if "time" not in base_cfg or not isinstance(base_cfg.get("time"), dict):
                base_cfg["time"] = {}
            base_cfg["time"]["horizon_min"] = int(horizon_min)
            flat_req = run_dir / "requests.flat.json"
            _flatten_scenarios_to_requests(scen_path, flat_req)
            demand_obj = json.loads(flat_req.read_text(encoding="utf-8"))
            merged_obj = {"base": base_cfg, "subproblem": sub_obj_for_merge, "demand": demand_obj}
            (run_dir / "merged.json").write_text(json.dumps(merged_obj, indent=2), encoding="utf-8")
    except Exception as e:
        print(f"[scenarios] warn: failed to emit per-scenario merged.json files: {e}")

    # Iterate each scenario with the master↔subproblem loop
    all_ok = True
    for scen_path in scen_paths:
        print(f"[scenarios] iterating {scen_path.name}")
        ok = iterate_flow(
            max_iters=(max_iters if max_iters is not None else 10),
            tol=(tol if tol is not None else 1e-6),
            sleep_sec=(sleep_sec if sleep_sec is not None else 0.0),
            scenario_path=scen_path,
        )
        all_ok = all_ok and ok
    return all_ok

def subproblem_flow(scenario_path: Path | None = None):
    # Ensure subproblem binary and master-produced sequence exist
    if not ensure_built():
        return False
    sub_in = ROOT / "outputs" / "subproblem.json"
    if not sub_in.exists():
        print("[subproblem] missing outputs/subproblem.json → running master")
        if not master_flow(scenario_path=scenario_path):
            return False

    # If a specific scenario file is configured, prefer it (under data/scenarios/)
    scen_file = scenario_path if scenario_path is not None else _scenario_file_from_yaml()
    if scen_file is not None:
        if not scen_file.exists():
            print(f"[subproblem] configured scenario file not found: {scen_file}")
            return False
        (ROOT / "outputs").mkdir(exist_ok=True)
        out_path = ROOT / "outputs" / "subproblem_result.json"

        ext = scen_file.suffix.lower()
        if ext == ".json":
            # Normalize to a combined multi-scenario JSON the C subproblem consumes.
            # If file already has {scenarios:[...]}, copy it; otherwise wrap single scenario.
            try:
                j = json.loads(scen_file.read_text(encoding="utf-8"))
            except Exception as e:
                print(f"[subproblem] failed to read JSON '{scen_file}': {e}")
                return False
            combined_path = ROOT / "outputs" / "scenarios_combined.json"
            if isinstance(j, dict) and isinstance(j.get("scenarios"), list):
                combined = {"scenarios": j["scenarios"]}
            else:
                combined = {"scenarios": [j]}
            combined_path.write_text(json.dumps(combined, indent=2), encoding="utf-8")
            req_path = combined_path
        elif ext == ".csv":
            try:
                obj = _read_scenario_from_csv(scen_file)
            except Exception as e:
                print(f"[subproblem] failed to parse CSV '{scen_file}': {e}")
                return False
            combined_path = ROOT / "outputs" / "scenarios_combined.json"
            combined = {"scenarios": [obj]}
            combined_path.write_text(json.dumps(combined, indent=2), encoding="utf-8")
            req_path = combined_path
        else:
            print(f"[subproblem] unsupported scenario file extension: {ext} (use .json or .csv)")
            return False
        # Also produce a merged.json under outputs/<input_name>/ for C runner parity
        try:
            run_dir = _run_dir_for_input(scen_file)
            # Prepare base config (override horizon if needed)
            step_res = _time_res_from_scenario(scen_file)
            horizon_min = _compute_horizon_from_json(scen_file, step_res)
            try:
                base_cfg = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
            except Exception:
                base_cfg = {}
            if "time" not in base_cfg or not isinstance(base_cfg.get("time"), dict):
                base_cfg["time"] = {}
            base_cfg["time"]["horizon_min"] = int(horizon_min)
            # Prepare demand.requests (flatten if needed)
            flat_req = run_dir / "requests.flat.json"
            _flatten_scenarios_to_requests(scen_file, flat_req)
            demand_obj = json.loads(flat_req.read_text(encoding="utf-8"))
            # Load subproblem.json produced by master (ensure exists)
            sub_in = ROOT / "outputs" / "subproblem.json"
            sub_obj = json.loads(sub_in.read_text(encoding="utf-8")) if sub_in.exists() else {}
            merged_obj = {"base": base_cfg, "subproblem": sub_obj, "demand": demand_obj}
            (run_dir / "merged.json").write_text(json.dumps(merged_obj, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"[subproblem] warn: failed to emit merged.json: {e}")
        return run_subproblem_with_out(sub_in, req_path, out_path)

    # Ensure at least one scenario exists; if not, generate from demand_agg
    if _json_mode_from_yaml():
        print("[subproblem] data.json=true or data.load_demand=false but no json_file/scenarios.file set")
        return False
    demand_agg_path, sdir_cfg, _, _ = _scen_cfg_from_yaml()
    # Look for scenarios in preferred dirs
    preferred_dirs = [
        sdir_cfg,
        ROOT / "outputs" / "scenarios_random",
        ROOT / "outputs" / "scenarios_aligned",
        ROOT / "outputs" / "scenarios",
    ]
    scen_dir = None
    scen_list = []
    for d in preferred_dirs:
        if d.exists():
            files = sorted(d.glob("scenario*.json"))
            if files:
                scen_dir = d
                scen_list = files
                break
    if scen_dir is None:
        scen_dir = preferred_dirs[0]
        scen_list = []
    if not scen_list:
        if not demand_agg_path.exists():
            print(
                f"[subproblem] missing aggregated demand file: {demand_agg_path}. "
                "Provide it or update configs/base.yaml:scenarios.demand_agg_path."
            )
            return False
        # Use YAML-configured random/alignment
        _, _, n_cfg, rnd_cfg = _scen_cfg_from_yaml()
        generate_scenarios_from_agg(
            demand_agg_path, scen_dir, num_scenarios=int(n_cfg), random_minutes=bool(rnd_cfg)
        )
        scen_list = sorted(scen_dir.glob("scenario*.json"))
        if not scen_list:
            print("[subproblem] scenario generation failed")
            return False

    # Combine scenarios into one multi-scenario JSON and run a single optimization
    combined = {"scenarios": []}
    for scen_path in scen_list:
        try:
            combined["scenarios"].append(json.loads(scen_path.read_text(encoding="utf-8")))
        except Exception as e:
            print(f"[subproblem] failed reading {scen_path}: {e}")
            return False
    combined_path = ROOT / "outputs" / "scenarios_combined.json"
    combined_path.write_text(json.dumps(combined, indent=2), encoding="utf-8")

    out_path = ROOT / "outputs" / "subproblem_result.json"
    return run_subproblem_with_out(sub_in, combined_path, out_path)


def demand_only_flow(num_scenarios: int | None = None, seed: int | None = None, random_minutes: bool | None = None):
    # Use an existing aggregated demand file to generate scenarios only.
    (ROOT / "outputs").mkdir(exist_ok=True)
    dagg_cfg, sdir_cfg, n_cfg, rnd_cfg = _scen_cfg_from_yaml()
    demand_agg_path = dagg_cfg
    if not demand_agg_path.exists():
        print(
            f"[demand] aggregated demand file not found: {demand_agg_path}. "
            "Provide it or update configs/base.yaml:scenarios.demand_agg_path."
        )
        return False
    # Default to YAML settings unless explicitly overridden
    scen_dir = sdir_cfg
    if random_minutes is None:
        random_minutes = rnd_cfg
    if num_scenarios is None:
        num_scenarios = n_cfg
    scen_paths = generate_scenarios_from_agg(
        demand_agg_path,
        scen_dir,
        num_scenarios=int(num_scenarios),
        seed=seed,
        random_minutes=bool(random_minutes),
    )
    print(
        f"[demand] generated {len(scen_paths)} scenarios under {scen_dir} "
        f"(random_minutes={'on' if random_minutes else 'off'})"
    )
    return True


def iterate_flow(
    max_iters: int = 10,
    tol: float = 1e-6,
    sleep_sec: float = 0.0,
    scenario_path: Path | None = None,
) -> bool:
    # Ensure binaries
    if not ensure_master_built():
        return False
    if not ensure_built():
        return False

    # If no scenario is provided and not using JSON scenario mode, require demand_agg
    scen_file = scenario_path if scenario_path is not None else _scenario_file_from_yaml()
    demand_agg_path, _, _, _ = _scen_cfg_from_yaml()
    if scen_file is None and not _json_mode_from_yaml() and not demand_agg_path.exists():
        print(
            f"[iterate] missing aggregated demand file: {demand_agg_path}. "
            "Set data.json/json_file in YAML or provide scenarios.demand_agg_path."
        )
        return False

    prev_obj = None
    prev_seq_hash = None

    for it in range(1, max_iters + 1):
        print(f"[iterate] ===== Iteration {it}/{max_iters} =====")
        # Run master to produce outputs/subproblem.json
        if not master_flow(scenario_path=scen_file):
            print("[iterate] master_flow failed")
            return False

        # Hash the subproblem.json to detect identical sequences
        sub_in_path = ROOT / "outputs" / "subproblem.json"
        try:
            sub_txt = sub_in_path.read_text(encoding="utf-8")
            import hashlib
            seq_hash = hashlib.md5(sub_txt.encode("utf-8")).hexdigest()
        except Exception as e:
            print(f"[iterate] failed to read subproblem.json: {e}")
            return False

        # Run subproblem to produce outputs/subproblem_result.json
        if not subproblem_flow(scenario_path=scen_file):
            print("[iterate] subproblem_flow failed")
            return False

        # Read objective
        out_path = ROOT / "outputs" / "subproblem_result.json"
        obj = None
        try:
            res = json.loads(out_path.read_text(encoding="utf-8"))
            obj = float(res.get("objective", float("nan")))
        except Exception as e:
            print(f"[iterate] failed to read subproblem_result.json: {e}")
            return False

        print(f"[iterate] objective={obj} seq_hash={seq_hash}")

        # Check convergence: identical sequences OR small objective delta
        if prev_seq_hash is not None and seq_hash == prev_seq_hash:
            print("[iterate] convergence: sequences unchanged from previous iteration")
            return True
        if prev_obj is not None and obj == obj and abs(obj - prev_obj) <= tol:
            print(f"[iterate] convergence: |Δobj|={abs(obj - prev_obj):.3g} ≤ tol={tol}")
            return True

        prev_obj = obj
        prev_seq_hash = seq_hash

        if sleep_sec > 0:
            time.sleep(sleep_sec)

    print("[iterate] reached max iterations without convergence (consider increasing --max-iters)")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MOB-AUTO2 Decomposition Runner")
    parser.add_argument(
        "--mode",
        choices=["build", "master", "subproblem", "iterate", "scenarios"],
        default="master",
        help=(
            "build: build C++ binaries only; "
            "master: run CP master only to produce outputs/subproblem.json; "
            "subproblem: run MILP subproblem for configured demand (or generated); "
            "iterate: alternate master↔subproblem on a single demand file; "
            "scenarios: generate scenarios from a demand file and iterate each"
        ),
    )
    parser.add_argument("--max-iters", type=int, default=10, help="max iterations for iterate mode")
    parser.add_argument("--tol", type=float, default=1e-6, help="objective tolerance for iterate mode")
    parser.add_argument("--sleep", type=float, default=0.0, help="sleep seconds between iterations")
    parser.add_argument("--num-scenarios", type=int, default=None, help="override: number of scenarios to generate")
    parser.add_argument("--seed", type=int, default=None, help="random seed for scenario generation")
    parser.add_argument("--aligned", action="store_true", help="override: align request times to slot start (random minutes = off)")
    args = parser.parse_args()

    if args.mode == "build":
        ok_master = build_master(M_CP_SRC, M_CP_BIN)
        ok_subp   = build_subproblem(C_SRC, C_BIN)
        if not (ok_master and ok_subp):
            exit(1)
    elif args.mode == "master":
        ok = master_flow()
        if not ok:
            exit(1)
    elif args.mode == "subproblem":
        ok = subproblem_flow()
        if not ok:
            exit(1)
    elif args.mode == "iterate":
        # If a single demand file is configured, iterate it; else require demand_agg
        scen = _scenario_file_from_yaml()
        ok = iterate_flow(max_iters=args.max_iters, tol=args.tol, sleep_sec=args.sleep, scenario_path=scen)
        if not ok:
            exit(1)
    elif args.mode == "scenarios":
        rnd = None if not args.aligned else False
        ok = scenarios_flow(
            n=args.num_scenarios,
            seed=args.seed,
            random_minutes=rnd,
            max_iters=args.max_iters,
            tol=args.tol,
            sleep_sec=args.sleep,
        )
        if not ok:
            exit(1)
    else:
        exit(1)
