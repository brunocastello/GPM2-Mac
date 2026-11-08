"""python -m recomp f1 GPM2.EXE gen/: translates Grand Prix Manager 2 1.02B to C in gen/."""

from __future__ import annotations

import argparse
import json
import sys

from .pe import PE


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="recomp")
    sub = ap.add_subparsers(dest="cmd", required=True)
    q = sub.add_parser("f1", help="translate the executable to C")
    q.add_argument("exe")
    q.add_argument("out")
    args = ap.parse_args(argv)
    if args.cmd == "f1":
        import hashlib
        from .discover import Discovery
        from .emit import Emitter
        pe = PE.load(args.exe)
        em = Emitter(Discovery(pe).run()).run()
        if em.errors or em.soft_errors:
            print(em.failure_report(), file=sys.stderr)
            return 1
        print(json.dumps(em.write(args.out, hashlib.sha256(pe.data).hexdigest()), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
