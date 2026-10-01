"""Collapse an xperf text dump (``xperf -i trace.etl -symbols -a dumper``) into per-thread stack counts of one process.

Usage: python scripts/hotspots_collapse.py <dump.txt> <process_name> <out.stacks.txt>

Output format (tab separated, one line per distinct stack):
    tid<TAB>thread_start<TAB>count<TAB>frame1;frame2;...;frameN
frame1 is the sampled instruction (leaf), frameN the thread start. A frame is ``module!function`` and, for
frames of the profiled executable (and any module given in APP_MODULES), ``module!function@0x<address>`` so that
the address can later be resolved to a source line (the address of a non-leaf frame is a return address).
The first line is a header ``# samples_total=<n> samples_with_stack=<n> process=<name> pid=<pid> sample_us=<interval>``.

The kernel and the user stack of a sample are separate ``Stack`` fragments in the dump, each numbered from 1;
they are concatenated in the order they appear (kernel first). A sample without any Stack line is kept with
its leaf only.
"""
import re
import sys
from collections import Counter, defaultdict

APP_MODULES = ('asteroids_native.exe', 'asteroids_nvrhi.exe')


def main():
    dump, process, out = sys.argv[1:4]
    pid = None
    counts = Counter()
    thread_start = {}
    total = 0
    with_stack = 0
    cur = None  # (tid, frames)
    cur_key = None
    timestamps = []

    def flush():
        nonlocal with_stack
        if cur is None:
            return
        tid, frames = cur
        if len(frames) > 1:
            with_stack += 1
        counts[(tid, ';'.join(frames))] += 1

    sampled_re = re.compile(r'^\s*SampledProfile,\s*(\d+),\s*(.+?) \(\s*(\d+)\),\s*(\d+),\s*(0x[0-9a-fA-F]+),\s*(\d+),\s*(.*?),\s*(.*?),\s*(\d+),\s*(\w+)\s*$')
    stack_re = re.compile(r'^\s*Stack,\s*(\d+),\s*(\d+),\s*(\d+),\s*(0x[0-9a-fA-F]+),\s*(.*?)\s*$')
    with open(dump, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('         SampledProfile,'):
                m = sampled_re.match(line)
                if not m:
                    continue
                ts, pname, p, tid, pc, cpu, start, leaf, cnt, kind = m.groups()
                if pname != process:
                    flush(); cur = None; cur_key = None
                    continue
                if pid is None:
                    pid = int(p)
                elif int(p) != pid:
                    # a second process with the same name: keep the first one seen
                    flush(); cur = None; cur_key = None
                    continue
                flush()
                total += 1
                if len(timestamps) < 2 or total % 997 == 0:
                    timestamps.append(int(ts))
                thread_start[tid] = start
                cur = (tid, [fmt(leaf, pc)])
                cur_key = (ts, tid)
            elif line.startswith('                  Stack,'):
                if cur is None:
                    continue
                m = stack_re.match(line)
                if not m:
                    continue
                ts, tid, no, addr, name = m.groups()
                if (ts, tid) != cur_key:
                    continue
                fr = fmt(name, addr)
                frames = cur[1]
                # the first frame of the first fragment repeats the leaf
                if len(frames) == 1 and no == '1' and fr == frames[0]:
                    continue
                frames.append(fr)
            else:
                # any other event ends the current sample's stack fragments
                if cur is not None and not line.startswith('                  Stack,'):
                    pass
    flush()
    per_thread = defaultdict(int)
    for (tid, _), c in counts.items():
        per_thread[tid] += c
    interval = 0
    if len(timestamps) >= 2:
        interval = 0  # not used; kept for a future version
    with open(out, 'w', encoding='utf-8') as o:
        o.write(f'# samples_total={total} samples_with_stack={with_stack} process={process} pid={pid}\n')
        for tid, n in sorted(per_thread.items(), key=lambda kv: -kv[1]):
            o.write(f'# thread {tid} samples={n} start={thread_start.get(tid, "?")}\n')
        for (tid, stack), c in sorted(counts.items(), key=lambda kv: -kv[1]):
            o.write(f'{tid}\t{thread_start.get(tid, "?")}\t{c}\t{stack}\n')
    print(f'{out}: pid={pid} samples={total} with_stack={with_stack} threads={len(per_thread)} distinct_stacks={len(counts)}')


def fmt(name, addr):
    # xperf writes ',' inside a symbol name as ';' in Stack lines (template arguments); the SampledProfile line
    # keeps the ','. Normalise to ',' so that ';' can separate frames and the leaf matches its first stack frame.
    name = name.strip().replace(';', ',')
    if name.endswith(']'):
        name = re.sub(r'\s*\[.*\]$', '', name)  # "-add_inline" list: every function inlined anywhere into the frame
    mod = name.split('!', 1)[0]
    if mod in APP_MODULES:
        return f'{name}@{addr}'
    return name


if __name__ == '__main__':
    main()
