import argparse, json, math, subprocess, sys

def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values))

def o0_geomean(report):
    return geomean([
        k["cider"]["median_ns"] / k["c"]["O0"]["median_ns"]
        for k in report["kernels"]
    ])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-o0", type=float, default=1.0)
    ap.add_argument("--max-o1", type=float, default=3.0)
    ap.add_argument("--samples", type=int, default=15)
    ap.add_argument("--sessions", type=int, default=3)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--cider-args", default="--codegen=fast")
    args = ap.parse_args()
    samples = 1 if args.quick else args.samples
    sessions = 1 if args.quick else args.sessions
    cmd = [sys.executable, "bench/compiler/run.py",
           "--suite", "bench/compiler/kernels",
           "--samples", str(samples), "--sessions", str(sessions),
           "--cpu", "2", "--cider-args=" + args.cider_args,
           "--out", args.out]
    subprocess.run(cmd, check=True)
    report = json.load(open(args.out))
    bad = [k["name"] for k in report["kernels"] if k["checksum"] != "ok"]
    if bad:
        print("FAIL checksum: " + ", ".join(bad), file=sys.stderr)
        return 1
    o0 = o0_geomean(report)
    o1 = report["geomean"]["O1"]
    print("O0 %.4f (limit %.4f)  O1 %.4f (limit %.4f)" %
          (o0, args.max_o0, o1, args.max_o1))
    if o0 > args.max_o0 or o1 > args.max_o1:
        print("FAIL: Stage 1.5 gate", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
