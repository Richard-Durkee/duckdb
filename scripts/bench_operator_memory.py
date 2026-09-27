#!/usr/bin/env python3
"""Compare query runtime (paired rounds) for per-operator memory attribution against upstream.

Runs each query with profiling off and on, on an upstream binary and a prototype binary, interleaving the
configurations across rounds so drift (thermal, background load) affects all of them equally.

usage: bench_operator_memory.py UPSTREAM_BINARY PROTOTYPE_BINARY [--rounds N] [--reps N] [--out FILE]
"""
import argparse
import json
import random
import re
import statistics
import subprocess
import time

QUERIES = {
    "two_hash_joins": "SELECT count(*) FROM range(8000000) a JOIN range(8000000) b ON a.range = b.range "
    "JOIN range(8000000) c ON a.range = c.range",
    "group_by_4m_groups": "SELECT count(*) FROM (SELECT range % 4000000 AS g, count(*) FROM range(20000000) GROUP BY g)",
    "order_by_20m": "SELECT * FROM range(20000000) t(x) ORDER BY hash(x) LIMIT 5 OFFSET 19999990",
    "list_agg_1m_groups": "SELECT count(*) FROM (SELECT range % 1000000 AS g, list(range) AS l FROM range(10000000) GROUP BY g)",
    "string_agg_500k_groups": "SELECT count(*) FROM (SELECT range % 500000 AS g, string_agg(range::VARCHAR, ',') AS s "
    "FROM range(5000000) GROUP BY g)",
}
THREADS = 8
MODES = {
    "off": "",
    "on": "PRAGMA enable_profiling = 'no_output';",
}
TIME_RE = re.compile(r"Run Time \(s\): real ([0-9.]+) user ([0-9.]+) sys ([0-9.]+)")
MIN_IDLE_PCT = 85.0


def idle_pct():
    # CPU idle over a one-second window, sampled between runs so none of our own processes are counted
    out = subprocess.run(["top", "-l", "2", "-s", "1", "-n", "0"], capture_output=True, text=True).stdout
    return float(re.findall(r"([0-9.]+)% idle", out)[-1])


def wait_for_quiet_machine():
    # other load distorts wall-clock time, so wait until the machine is otherwise idle
    while idle_pct() < MIN_IDLE_PCT:
        time.sleep(5)


def run_config(binary, query, mode, reps):
    # one warm-up run, then `reps` timed runs, all in one process
    script = (
        "SET threads = %d;\n" % THREADS + MODES[mode] + "\n.timer on\n" + "\n".join([query + ";"] * (reps + 1)) + "\n"
    )
    out = subprocess.run([binary], input=script, capture_output=True, text=True, check=True).stdout
    found = TIME_RE.findall(out)
    if len(found) != reps + 1:
        raise RuntimeError("expected %d timings, got %d:\n%s" % (reps + 1, len(found), out[-2000:]))
    # (wall seconds, cpu seconds = user + sys summed over all threads), warm-up dropped
    return [(float(real), float(user) + float(sys)) for real, user, sys in found[1:]]


def main():
    global MIN_IDLE_PCT
    parser = argparse.ArgumentParser()
    parser.add_argument("upstream")
    parser.add_argument("prototype")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--out", default="/tmp/bench_operator_memory.json")
    parser.add_argument("--min-idle", type=float, default=MIN_IDLE_PCT, help="wait until CPU idle %% is at least this")
    args = parser.parse_args()
    MIN_IDLE_PCT = args.min_idle

    binaries = {"upstream": args.upstream, "prototype": args.prototype}
    pairs = [(q, m) for q in QUERIES for m in MODES]
    samples = {(b, q, m): [] for b in binaries for q, m in pairs}
    ratios = {p: [] for p in pairs}
    for round_idx in range(args.rounds):
        random.shuffle(pairs)
        for query, mode in pairs:
            order = list(binaries)
            random.shuffle(order)
            wait_for_quiet_machine()
            medians = {}
            for build in order:
                times = run_config(binaries[build], QUERIES[query], mode, args.reps)
                samples[(build, query, mode)] += times
                medians[build] = (statistics.median(t[0] for t in times), statistics.median(t[1] for t in times))
            up, pr = medians["upstream"], medians["prototype"]
            ratios[(query, mode)].append((pr[0] / up[0], pr[1] / up[1]))
        print("round %d/%d done" % (round_idx + 1, args.rounds), flush=True)
        with open(args.out + ".progress", "w") as f:
            f.write("%d/%d\n" % (round_idx + 1, args.rounds))

    random.seed(0)
    results = []

    def ci(values):
        boots = sorted(statistics.median(random.choices(values, k=len(values))) for _ in range(5000))
        return (boots[int(0.025 * len(boots))] - 1) * 100, (boots[int(0.975 * len(boots)) - 1] - 1) * 100

    print("\n%-24s %-4s %9s %17s %9s %17s" % ("query", "prof", "wall", "95% CI", "cpu", "95% CI"))
    for query in QUERIES:
        for mode in MODES:
            rs = ratios[(query, mode)]
            wall = [r[0] for r in rs]
            cpu = [r[1] for r in rs]
            wall_d, cpu_d = (statistics.median(wall) - 1) * 100, (statistics.median(cpu) - 1) * 100
            wall_ci, cpu_ci = ci(wall), ci(cpu)
            results.append(
                {
                    "query": query,
                    "profiling": mode,
                    "upstream": samples[("upstream", query, mode)],
                    "prototype": samples[("prototype", query, mode)],
                    "wall_ratios": wall,
                    "cpu_ratios": cpu,
                    "wall_delta_pct": wall_d,
                    "wall_ci_pct": wall_ci,
                    "cpu_delta_pct": cpu_d,
                    "cpu_ci_pct": cpu_ci,
                }
            )
            print(
                "%-24s %-4s %+8.1f%% [%+6.1f%%, %+5.1f%%] %+8.1f%% [%+6.1f%%, %+5.1f%%]"
                % (query, mode, wall_d, wall_ci[0], wall_ci[1], cpu_d, cpu_ci[0], cpu_ci[1])
            )
    with open(args.out, "w") as f:
        json.dump(results, f, indent=1)
    print("\nraw samples written to", args.out)


if __name__ == "__main__":
    main()
