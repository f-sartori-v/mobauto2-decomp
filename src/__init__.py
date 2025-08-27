import shutil
import subprocess, sys, os
from pathlib import Path
import yaml, json

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "configs" / "base.yaml"
C_SRC = ROOT / "cpp" / "subproblem" / "main.c"
C_EXE = ROOT / "cpp" / "subproblem.exe"

def run_basic():
    # 1) abre config
    with open(CONFIG, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    # 2) salva estrutura em JSON só pra mostrar que funcionou
    out = ROOT / "outputs" / "config_copy.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    print(f"[OK] Config lida de {CONFIG}, salva em {out}")

    # 3) compila main.c se exe não existir
    if not C_EXE.exists():
        if not shutil.which("cl"):
            print("[WARN] cl (MSVC) não encontrado. Pule compilação por enquanto.")
            return
        inc = os.environ.get("CPLEX_INC")
        lib = os.environ.get("CPLEX_LIB")
        if not inc or not lib:
            print("[WARN] Variáveis CPLEX_INC/CPLEX_LIB não definidas.")
            return
        cmd = f'cl /I"{inc}" "{C_SRC}" /Fe:"{C_EXE}" /link /LIBPATH:"{lib}" cplex2211.lib'
        print(f"[build] {cmd}")
        subprocess.run(cmd, shell=True, check=True)

    # 4) roda o exe
    if C_EXE.exists():
        print(f"[run] Executando {C_EXE} ...")
        subprocess.run([str(C_EXE)])
    else:
        print("[SKIP] Nenhum .exe do subproblem disponível")

run_basic()
