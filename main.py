# em src/main.py
from pathlib import Path
import shutil, subprocess, os, yaml, json

ROOT = Path(__file__).resolve().parents[0]
CONFIG = ROOT / "configs" / "base.yaml"
C_SRC = ROOT / "ccp" / "subproblem" / "main.c"
C_EXE = ROOT / "ccp" / "subproblem.exe"

def run_basic():
    with open(CONFIG, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    out = ROOT / "outputs" / "config_copy.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    print(f"[OK] Config lida de {CONFIG}, salva em {out}")

    if not C_EXE.exists():
        if not shutil.which("cl"):
            print("[WARN] 'cl' (MSVC) não encontrado. Pulei compilação.")
            return
        inc = os.environ.get("CPLEX_INC")
        lib = os.environ.get("CPLEX_LIB")
        if not inc or not lib:
            print("[WARN] Defina CPLEX_INC e CPLEX_LIB antes de compilar.")
            return
        cmd = f'cl /nologo /I"{inc}" "{C_SRC}" /Fe:"{C_EXE}" /link /LIBPATH:"{lib}" cplex2211.lib'
        print(f"[build] {cmd}")
        subprocess.run(cmd, shell=True, check=True)

    if C_EXE.exists():
        print(f"[run] {C_EXE}")
        subprocess.run([str(C_EXE)])
    else:
        print("[SKIP] Sem .exe do subproblem")

if __name__ == "__main__":
    run_basic()