# em src/main.py
from pathlib import Path
from subproblem import write_demand_files
import platform, shutil, subprocess, os, yaml, json, time, argparse, random, math

ROOT = Path(__file__).resolve().parents[0]
CONFIG = ROOT / "configs" / "base.yaml"
C_SRC = ROOT / "ccp" / "subproblem" / "main.c"
M_CP_SRC = ROOT / "ccp" / "master" / "main.cpp"

CJSON_DIR = ROOT / "ccp" / "subproblem" / "third_party" / "cjson"
CJSON_SRC = CJSON_DIR / "cJSON.c"
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
    sys = platform.system().lower()
    inc = os.getenv("CPLEX_INC")
    lib = os.getenv("CPLEX_LIB")

    if not CJSON_SRC.exists():
        print(f"[ERROR] missing {CJSON_SRC}. Put cJSON.c/.h in {CJSON_DIR}")
        return False

    if sys == "windows":
        if not shutil.which("cl"):
            print("[ERROR] MSVC 'cl' not found. Open the x64 Native Tools Prompt.")
            return False
        if not inc or not lib:
            print("[ERROR] Set CPLEX_INC and CPLEX_LIB.")
            return False
        cmd = f'cl /nologo /O2 /MD /std:c11 /I"{inc}" /I"{CJSON_DIR}" "{C_SRC}" "{CJSON_SRC}" "{JSONIO_SRC}" /Fe:"{C_OUT}" /link /LIBPATH:"{lib}" cplex*.lib'

    elif sys == "darwin":  # macOS
        if not shutil.which("clang"):
            print("[ERROR] clang not found on macOS.")
            return False
        if not inc or not lib:
            print("[ERROR] Set CPLEX_INC and CPLEX_LIB in your Run Configuration.")
            return False
        cmd = (f'clang -O2 -std=c11 -I"{inc}" -I"{CJSON_DIR}" "{C_SRC}" "{CJSON_SRC}" "{JSONIO_SRC}" '
               f'"{FEAS_SRC}" -L"{lib}" -lcplex -lm -lpthread -o "{C_OUT}"')

    else:  # linux
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            print("[ERROR] gcc/clang not found on Linux.")
            return False
        if not inc or not lib:
            print("[ERROR] Set CPLEX_INC and CPLEX_LIB.")
            return False
        cmd = (
            f'{cc} -O2 -std=c11 '
            f'-I"{inc}" -I"{CJSON_DIR}" '
            f'"{C_SRC}" "{CJSON_SRC}" "{JSONIO_SRC}"'
            f'-L"{lib}" -lcplex -lm -lpthread '
            f'-o "{C_OUT}"'
        )

    print(f"[build] {cmd}")
    ok = subprocess.call(cmd, shell=True) == 0


    return ok

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
    cmd = (
        f'clang++ -O2 -std=c++17 '
        f'-I"{studio}/concert/include" '
        f'-I"{studio}/cplex/include" '
        f'-I"{studio}/cpoptimizer/include" '
        f'-I"{CJSON_DIR}" '
        f'"{M_CP_SRC}" "{CJSON_SRC}" '
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
    # collect everything
    merged = {}

    # base.yaml

    base = yaml.safe_load(Path(base_yaml_path).read_text(encoding="utf-8"))
    subp = json.loads(Path(cfg_path).read_text(encoding="utf-8"))
    dem  = json.loads(Path(demand_path).read_text(encoding="utf-8"))

    merged = {
        "base": base,
        "subproblem": subp,
        "demand": dem,
    }

    # write merged file
    merged_path = bin_path.parent / "merged.json"
    merged_path.write_text(json.dumps(merged, indent=2), encoding="utf-8")

    # run the binary with this single path
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MOB-AUTO2 Decomposition Runner")
    parser.add_argument(
        "--mode",
        choices=["master", "subproblem", "build", "demand"],
        default="master",
        help=(
            "build: compile C++ CP master only; "
            "master: run C++ master only (produces outputs/subproblem.json); "
            "subproblem: run subproblem with existing outputs/demand_base.json; "
            "demand: generate random outputs/demand_base.json"
        ),
    )
    args = parser.parse_args()

    if args.mode == "build":
        ok = build_master(M_CP_SRC, M_CP_BIN)
        if not ok:
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
    else:
        exit(1)
