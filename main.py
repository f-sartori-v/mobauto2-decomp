# em src/main.py
from pathlib import Path
from subproblem import write_fake_input, write_demand_files
import platform, shutil, subprocess, os, yaml, json, time, argparse, random

ROOT = Path(__file__).resolve().parents[0]
CONFIG = ROOT / "configs" / "base.yaml"
C_SRC = ROOT / "ccp" / "subproblem" / "main.c"

# platform-aware output binary
if platform.system().lower() == "windows":
    C_BIN = ROOT / "ccp" / "subproblem.exe"
else:
    C_BIN = ROOT / "ccp" / "subproblem.out"

def build_subproblem(C_SRC: Path, C_OUT: Path):
    sys = platform.system().lower()
    inc = os.getenv("CPLEX_INC")
    lib = os.getenv("CPLEX_LIB")

    if sys == "windows":
        if not shutil.which("cl"):
            print("[ERROR] MSVC 'cl' not found. Open the x64 Native Tools Prompt.")
            return False
        if not inc or not lib:
            print("[ERROR] Set CPLEX_INC and CPLEX_LIB.")
            return False
        cmd = f'cl /nologo /O2 /MD /I"{inc}" "{C_SRC}" /Fe:"{C_OUT}" /link /LIBPATH:"{lib}" cplex*.lib'

    elif sys == "darwin":  # macOS
        if not shutil.which("clang"):
            print("[ERROR] clang not found on macOS.")
            return False
        if not inc or not lib:
            print("[ERROR] Set CPLEX_INC and CPLEX_LIB in your Run Configuration.")
            return False
        cmd = f'clang -O2 -I"{inc}" "{C_SRC}" -L"{lib}" -lcplex -lm -lpthread -o "{C_OUT}"'

    else:  # linux
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            print("[ERROR] gcc/clang not found on Linux.")
            return False
        if not inc or not lib:
            print("[ERROR] Set CPLEX_INC and CPLEX_LIB.")
            return False
        cmd = f'{cc} -O2 -I"{inc}" "{C_SRC}" -L"{lib}" -lcplex -lm -lpthread -o "{C_OUT}"'

    print(f"[build] {cmd}")
    return subprocess.call(cmd, shell=True) == 0

def ensure_built() -> bool:
    if not C_BIN.exists():
        ok = build_subproblem(C_SRC, C_BIN)
        if not ok:
            print("[FAIL] build failed")
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
    t0 = time.perf_counter()
    try:
        rc = subprocess.run(argv, check=False).returncode
    except subprocess.TimeoutExpired:
        print(f"[time] subproblem TIMEOUT after {time.perf_counter() - t0:.3f}s")
        return False
    print(f"[time] subproblem ran in {time.perf_counter() - t0:.3f}s, exit={rc}")
    return rc == 0

def master_flow():
    # Placeholder: in the real master you will read CONFIG, optimize, then build a sequence per shuttle and window
    cfg = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    # stub example: single shuttle, 5 slots

    if not ensure_built():
        return False

    in_path = ROOT / "outputs" / "subproblem.json"
    write_fake_input(in_path)

    demand_base = ROOT / "outputs" / "demand_base.json"
    demand = 2 ** random.randint(0, 10)
    demand_path = write_demand_files(demand, demand_base)

    return run_binary_with_config(C_BIN, in_path, demand_path)

def fake_master_flow():
    if not ensure_built():
        return False

    in_path = ROOT / "outputs" / "subproblem.json"
    write_fake_input(in_path)

    demand_base = ROOT / "outputs" / "demand_base.json"
    demand = 2 ** random.randint(0, 10)
    demand_path = write_demand_files(demand, demand_base)

    # Call subproblem with fake input
    return run_binary_with_config(C_BIN, in_path, demand_path, CONFIG)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Runner")
    parser.add_argument("--mode", choices=["master", "subproblem", "build"], default="master",
                        help="basic: copy config only; master: real master flow; fake: simulate master and call subproblem")
    args = parser.parse_args()

    if args.mode == "build":
        ok = build_subproblem(C_SRC, C_BIN)
        if not ok:
            exit(1)
    elif args.mode == "master":
        ok = master_flow()
        if not ok:
            exit(1)
    elif args.mode == "subproblem":
        ok = fake_master_flow()
        if not ok:
            exit(1)
    else:
        exit(1)