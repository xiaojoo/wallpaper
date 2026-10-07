import json
import sys

NAMES = ("datarain", "binarydrift", "neonfall", "embers")


def load(p):
    return {r["file"].split("_")[0]: r for r in json.load(open(p))}


def fmt(v, w, pre=""):
    return f"{'n/a':>{w}}" if v is None else f"{pre}{v:>{w}.1f}"


before, after = load(sys.argv[1]), load(sys.argv[2])
print(f"{'effect':13} {'coverage %':>19} {'marks':>19} {'spacing px':>19} "
      f"{'runs/row':>19} {'run gap px':>20}")
for n in NAMES:
    b, a = before[n], after[n]
    cols = []
    for k in ("c", "m", "s", "r", "q"):
        cols.append(f"{fmt(b.get(k + '12'), 7)}->{fmt(a.get(k + '12'), 7)}")
    print(f"{n:13} " + " ".join(f"{c:>17}" for c in cols))

print("\nthreshold sensitivity, after (delta 8 / 12 / 20) - a stable ruler reads flat here:")
for n in NAMES:
    a = after[n]
    cov = " ".join(f"{a['c' + str(d)]:5.2f}" for d in (8, 12, 20))
    mk = " ".join(f"{a['m' + str(d)]:6d}" for d in (8, 12, 20))
    sp = " ".join(f"{a['s' + str(d)]:6.1f}" for d in (8, 12, 20))
    rn = " ".join(f"{a['r' + str(d)]:5.1f}" for d in (8, 12, 20))
    qg = " ".join(("  n/a" if a["q" + str(d)] is None else f"{a['q' + str(d)]:5.1f}") for d in (8, 12, 20))
    print(f"  {n:13} cov {cov}   marks {mk}   spacing {sp}   runs {rn}   gap {qg}")
