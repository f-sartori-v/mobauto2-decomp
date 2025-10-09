# em src/main.py
from pathlib import Path
from subproblem import write_demand_files
import platform, shutil, subprocess, os, yaml, json, time, argparse, random, math

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
        f'-I"{studio}/cpoptimizer/include" '
        f'-I"{CJSON_DIR}" '
        f'"{M_CP_SRC}" "{CJSON_OBJ}" '
        f'-L"{studio}/concert/lib/x86-64_osx/static_pic" '
        f'-L"{studio}/cplex/lib/x86-64_osx/static_pic" '
        f'-L"{studio}/cpoptimizer/lib/x86-64_osx/static_pic" '
        f'-lconcert -lilocplex -lcplex -lcp -lyaml-cpp -lpthread -lm '
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

def master_flow():
    # Ensure master binary is available (master-only run)
    if not ensure_master_built():
        return False

    # 1) Prepare demand aggregation input for master (require pre-generated demand)
    demand_path = ROOT / "outputs" / "demand_base.json"
    if not demand_path.exists():
        print("[master] missing outputs/demand_base.json. Run with --mode demand first.")
        return False

    # 2) Build merged_master.json for the C master
    base = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    dem = json.loads(demand_path.read_text(encoding="utf-8"))

    # Aggregate demand outside the C master (cached file)
    from master import aggregate_demand  # local import to avoid cycles
    demand_agg = aggregate_demand(demand_path, CONFIG)

    # Write aggregated demand to outputs for reuse across iterations
    (ROOT / "outputs").mkdir(exist_ok=True)
    demand_agg_path = ROOT / "outputs" / "demand_agg.json"
    demand_agg_path.write_text(json.dumps(demand_agg, indent=2), encoding="utf-8")

    merged_master = {"base": base, "demand": dem, "demand_agg": demand_agg}
    merged_master_path = ROOT / "ccp" / "merged_master.json"
    merged_master_path.write_text(json.dumps(merged_master, indent=2), encoding="utf-8")

    # 3) Run C++ master to produce outputs/subproblem.json
    sub_in_path = ROOT / "outputs" / "subproblem.json"
    # Pass config and aggregated demand file to the C++ master
    argv = [str(M_CP_BIN), str(CONFIG), str(demand_agg_path), str(sub_in_path)]
    print(f"[run] {' '.join(argv)}")
    rc = subprocess.run(argv, check=False).returncode
    if rc != 0:
        print(f"[master] C master exited {rc}")
        return False
    print(f"[master] generated subproblem input at {sub_in_path}")
    return True

def subproblem_flow():
    # Build subproblem only; require master/demand inputs to exist
    if not ensure_built():
        return False

    in_path = ROOT / "outputs" / "subproblem.json"
    if not in_path.exists():
        print("[subproblem] missing outputs/subproblem.json. Run --mode master first.")
        return False

    demand_path = ROOT / "outputs" / "demand_base.json"
    if not demand_path.exists():
        print("[subproblem] missing outputs/demand_base.json. Run with --mode demand first.")
        return False

    # Call subproblem with existing demand and master-produced input
    return run_binary_with_config(C_BIN, in_path, demand_path, CONFIG)


def demand_only_flow():
    # Generate random demand and write to outputs/demand_base.json
    (ROOT / "outputs").mkdir(exist_ok=True)
    demand_base = ROOT / "outputs" / "demand_base.json"
    demand = 2 ** random.randint(0, 10)
    write_demand_files(demand, demand_base)
    print(f"[demand] wrote {demand_base}")
    return True


def iterate_flow(max_iters: int = 10, tol: float = 1e-6, sleep_sec: float = 0.0) -> bool:
    # Ensure binaries
    if not ensure_master_built():
        return False
    if not ensure_built():
        return False

    # Ensure demand exists; if not, create a random one
    demand_path = ROOT / "outputs" / "demand_base.json"
    if not demand_path.exists():
        print("[iterate] demand_base.json missing → generating a random one")
        if not demand_only_flow():
            return False

    prev_obj = None
    prev_seq_hash = None

    for it in range(1, max_iters + 1):
        print(f"[iterate] ===== Iteration {it}/{max_iters} =====")
        # Run master to produce outputs/subproblem.json
        if not master_flow():
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
        if not subproblem_flow():
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
        choices=["master", "subproblem", "build", "demand", "iterate"],
        default="master",
        help=(
            "build: compile C++ CP master and subproblem; "
            "master: run C++ master only (produces outputs/subproblem.json); "
            "subproblem: run C++ subproblem with existing outputs/demand_base.json; "
            "demand: generate random outputs/demand_base.json; "
            "iterate: alternate master→subproblem until convergence or limit"
        ),
    )
    parser.add_argument("--max-iters", type=int, default=10, help="max iterations for iterate mode")
    parser.add_argument("--tol", type=float, default=1e-6, help="objective tolerance for iterate mode")
    parser.add_argument("--sleep", type=float, default=0.0, help="sleep seconds between iterations")
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
    elif args.mode == "demand":
        ok = demand_only_flow()
        if not ok:
            exit(1)
    elif args.mode == "iterate":
        ok = iterate_flow(max_iters=args.max_iters, tol=args.tol, sleep_sec=args.sleep)
        if not ok:
            exit(1)
    else:
        exit(1)
