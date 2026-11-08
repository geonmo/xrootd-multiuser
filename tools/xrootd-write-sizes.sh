#!/bin/bash
# xrootd-write-sizes.sh: measure the size distribution of the data writes the
# xrootd daemon issues to the storage, to decide whether write buffering
# (xrootd-multiuser issue #64 / PR #65) would help.
#
# It attaches strace to the running xrootd process(es) for a short time, records
# write/pwrite64/writev/pwritev calls on files under the given data path, and
# prints a size histogram and per-file statistics.  Run it as root on the
# storage server while typical transfers (FTS, xrdcp, HTTP PUT) are running.
#
# strace slows the traced process down noticeably (every write is stopped and
# decoded), so keep the duration short (default 30 s) and avoid peak hours.
#
# Usage:  xrootd-write-sizes.sh -p <data path prefix> [-d seconds] [-n process name] [-o out dir]
#   -p  only count writes to files under this path, e.g. /lustre/cms (required)
#   -d  how long to trace, in seconds (default 30)
#   -n  process name to trace (default xrootd); every process of that name is traced
#   -o  directory for the raw strace log (default: a new directory under /tmp)
#   -r  only analyse an existing strace log (given as the argument), do not trace
set -euo pipefail

prefix="" duration=30 pname=xrootd outdir="" reanalyse=""
while getopts "p:d:n:o:r:h" opt; do
  case $opt in
    p) prefix=$OPTARG ;;
    d) duration=$OPTARG ;;
    n) pname=$OPTARG ;;
    o) outdir=$OPTARG ;;
    r) reanalyse=$OPTARG ;;
    *) sed -n '2,20p' "$0"; exit 1 ;;
  esac
done
[ -n "$prefix" ] || { echo "error: -p <data path prefix> is required" >&2; exit 1; }
[[ "$duration" =~ ^[0-9]+$ ]] || { echo "error: -d must be a number of seconds" >&2; exit 1; }

if [ -n "$reanalyse" ]; then
  log=$reanalyse
else
  [ "$(id -u)" = 0 ] || { echo "error: run as root (strace must attach to the daemon)" >&2; exit 1; }
  command -v strace >/dev/null || { echo "error: strace is not installed (dnf install strace)" >&2; exit 1; }
  pids=$(pgrep -x "$pname" || true)
  [ -n "$pids" ] || { echo "error: no running process named $pname" >&2; exit 1; }
  outdir=${outdir:-$(mktemp -d /tmp/xrootd-write-sizes.XXXXXX)}
  mkdir -p "$outdir"
  log=$outdir/strace.log
  args=()
  for p in $pids; do args+=(-p "$p"); done
  echo "tracing $(echo $pids | wc -w) $pname process(es) ($(echo $pids)) for ${duration}s; writes under $prefix"
  echo "raw log: $log"
  # -f: all threads; -y: show the path behind each fd; -s0: do not copy data;
  # -qq: no attach/exit messages; -e signal=none: no signal lines
  timeout -s INT "$duration" strace -f -y -s0 -qq -e signal=none \
    -e trace=write,pwrite64,writev,pwritev,pwritev2 -o "$log" "${args[@]}" || true
fi

python3 - "$log" "$prefix" "$duration" <<'EOF'
import collections, re, sys

log, prefix, duration = sys.argv[1], sys.argv[2], int(sys.argv[3])
# 12345 pwrite64(25</lustre/x/f.root>, ""..., 16384, 0) = 16384
# 12345 <... pwrite64 resumed>) = 16384       (paired with an "unfinished" line)
call_re = re.compile(r'^(\d+)\s+(write|pwrite64|writev|pwritev2?)\((\d+)<([^>]*)>')
ret_re = re.compile(r'=\s+(-?\d+)')
resumed_re = re.compile(r'^(\d+)\s+<\.\.\. (\w+) resumed>')

pending = {}            # tid -> path of an unfinished call
sizes = []              # (path, bytes) of successful writes under prefix
errors = 0
other_paths = collections.Counter()

def record(path, line):
    global errors
    m = ret_re.search(line)
    if not m:
        return
    n = int(m.group(1))
    if n < 0:
        errors += 1
    elif path.startswith(prefix):
        sizes.append((path, n))
    else:
        other_paths[path.split('/')[1] if path.startswith('/') else path[:20]] += 1

with open(log, errors='replace') as f:
    for line in f:
        m = call_re.match(line)
        if m:
            tid, path = m.group(1), m.group(4)
            if line.rstrip().endswith('<unfinished ...>'):
                pending[tid] = path
            else:
                record(path, line)
            continue
        m = resumed_re.match(line)
        if m and m.group(1) in pending:
            record(pending.pop(m.group(1)), line)

if not sizes:
    print(f"\nno writes under {prefix} were seen.")
    if other_paths:
        print("writes went to:", ", ".join(f"/{k} ({v})" for k, v in other_paths.most_common(5)))
    sys.exit(0)

def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024

total = sum(n for _, n in sizes)
count = len(sizes)
files = collections.defaultdict(lambda: [0, 0])
for path, n in sizes:
    files[path][0] += 1
    files[path][1] += n

# power-of-two buckets: [0,4K), [4K,8K), ... [8M, ...)
edges = [0] + [4096 << i for i in range(12)]      # 4 KiB .. 8 MiB
buckets = collections.Counter()
bbytes = collections.Counter()
for _, n in sizes:
    i = max(k for k, e in enumerate(edges) if n >= e)
    buckets[i] += 1
    bbytes[i] += n

print(f"\n{count} writes, {human(total)} to {len(files)} file(s) under {prefix} in {duration} s "
      f"({count / max(duration, 1):.0f} writes/s, {human(total / max(duration, 1))}/s)")
if errors:
    print(f"{errors} write(s) returned an error")
print(f"mean {human(total / count)}, median {human(sorted(n for _, n in sizes)[count // 2])}\n")
print(f"{'size':>20} {'writes':>9} {'%writes':>8} {'%bytes':>7}")
for i in sorted(buckets):
    lo = edges[i]
    label = f">= {human(lo)}" if i == len(edges) - 1 else f"{human(lo)} - {human(edges[i + 1])}"
    print(f"{label:>20} {buckets[i]:9d} {100 * buckets[i] / count:7.1f}% {100 * bbytes[i] / max(total, 1):6.1f}%")

small = sum(1 for _, n in sizes if n < 65536)
print(f"\nwrites smaller than 64 KiB: {small} ({100 * small / count:.1f}% of writes, "
      f"{100 * sum(n for _, n in sizes if n < 65536) / max(total, 1):.1f}% of bytes)")

print("\nfiles with the smallest average write (at least 1 MiB written):")
print(f"{'writes':>8} {'written':>10} {'avg write':>10}  file")
big = [(p, c, b) for p, (c, b) in files.items() if b >= 1 << 20]
for p, c, b in sorted(big, key=lambda x: x[2] / x[1])[:10]:
    print(f"{c:8d} {human(b):>10} {human(b / c):>10}  {p}")

print("""
How to read this:
- Mostly writes of 1 MiB or more (typical for HTTP-TPC pulls, which xrootd already
  aggregates to 1 MiB, and for xrdcp's 8 MiB chunks): write buffering will not help.
- A large share of writes below 64 KiB, at a high write rate: buffering (or a cheaper
  per-write ID switch) may be worth it; the files listed above show which transfers
  produce them.""")
EOF
