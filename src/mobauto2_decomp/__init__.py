from __future__ import annotations
import os, sys, subprocess, shutil, glob
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # pasta raiz do repo
C_DIR = ROOT / "c_cplex"
SRC = C_DIR / "subproblem.c"
EXE = C_DIR / "subproblem.exe"

def _find_cplex_libname() -> str:
    """Procura automaticamente um cplexXXXX.lib em %CPLEX_LIB%."""
    cplex_lib_dir = os.environ.get("CPLEX_LIB")
    if not cplex_lib_dir:
        raise RuntimeError("CPLEX_LIB não está definido no ambiente.")
    candidates = glob.glob(str(Path(cplex_lib_dir) / "cplex*.lib"))
    if not candidates:
        raise RuntimeError(f"Nenhum cplex*.lib encontrado em {cplex_lib_dir}")
    # pega o de maior versão (ordem lexicográfica funciona bem aqui)
    return Path(sorted(candidates)[-1]).name

def build_c_subproblem(force: bool = False) -> Path:
    """Compila c_cplex/subproblem.c com MSVC + CPLEX. Retorna caminho do .exe."""
    if EXE.exists() and not force:
        return EXE

    # checagens rápidas
    if not shutil.which("cl"):
        raise RuntimeError(
            "Compilador MSVC (cl) não encontrado. Abra o 'Developer Command Prompt for VS' "
            "ou adicione o vcvars64 ao PATH."
        )
    for env_name in ("CPLEX_INC", "CPLEX_LIB"):
        if not os.environ.get(env_name):
            raise RuntimeError(f"Variável de ambiente {env_name} não definida.")

    libname = _find_cplex_libname()
    inc = os.environ["CPLEX_INC"]
    lib = os.environ["CPLEX_LIB"]

    cmd = [
        "cl",
        f'/I"{inc}"',
        str(SRC),
        "/Fe:" + str(EXE),
        "/link",
        f'/LIBPATH:"{lib}"',
        libname,
    ]
    # Compila
    proc = subprocess.run(" ".join(cmd), shell=True)
    if proc.returncode != 0 or not EXE.exists():
        raise RuntimeError("Falha na compilação do subproblem.c")

    return EXE

def run_c_subproblem(args: list[str] | None = None, rebuild: bool = False) -> int:
    """Garante build e executa o subproblem.exe, repassando argumentos."""
    exe = build_c_subproblem(force=rebuild)
    run_cmd = [str(exe)]
    if args:
        run_cmd += args
    proc = subprocess.run(run_cmd)
    return proc.returncode
