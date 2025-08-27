import argparse
from pathlib import Path
import sys

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--config", "-c", type=Path, required=False, default=Path("configs/base.yaml"))
    args = p.parse_args()
    print(f"[mobauto2-decomp] using config: {args.config}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
