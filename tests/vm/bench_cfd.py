#!/usr/bin/env python3
"""Compare zm-cfd with direct parallel SSH execution on the same two VMs.

This is an orchestration baseline for independent serial OpenFOAM cases, not a
TaskVine/HTCondor benchmark and not an MPI benchmark of one distributed solve.
"""
import argparse
import collections
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
HOSTS = ('192.168.122.254', '192.168.122.174')
SSH = ['ssh', '-i', '/home/yash/Desktop/zm-qemu/id_zm', '-o', 'StrictHostKeyChecking=no',
       '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR', '-o', 'ConnectTimeout=5']
REMOTE = '/home/zm/zeromesh-cfd-bench'
SOLVER = '/opt/zeromesh-cfd-bench/bin/solve-case'
CASES = ('reynolds-10', 'reynolds-20', 'reynolds-50', 'reynolds-100')


def emit(row):
    print(json.dumps(row), flush=True)


def field_values(path):
    text = path.read_text()
    m = re.search(r'internalField\s+nonuniform\s+List<(scalar|vector)>\s+(\d+)\s*\((.*?)\)\s*;', text, re.S)
    if not m:
        raise RuntimeError(f'{path}: unsupported field representation')
    values = np.array([float(s) for s in re.findall(r'[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?', m[3])])
    expected = int(m[2]) * (3 if m[1] == 'vector' else 1)
    if len(values) != expected or int(m[2]) != 3600 or not np.isfinite(values).all():
        raise RuntimeError(f'{path}: invalid cell count or nonfinite data')
    values[values == 0] = 0  # Normalize signed zero for numerical hashes.
    return values


def validate(out, reference):
    hashes, differences, steps = {}, {}, {}
    for case in CASES:
        log = (out / case / 'icoFoam.log').read_text()
        times = re.findall(r'^Time = (.+)$', log, re.M)
        if len(times) != 2000 or float(times[-1]) != 10 or not log.rstrip().endswith('End'):
            raise RuntimeError(f'{case}: solver did not complete 2000 steps to t=10')
        steps[case] = len(times)
        for field in ('U', 'p'):
            values = field_values(out / case / '10' / field)
            ref = field_values(reference / case / '10' / field)
            delta = float(np.max(np.abs(values - ref)))
            if delta > 1e-6:
                raise RuntimeError(f'{case}/{field}: max absolute difference {delta} exceeds 1e-6')
            key = case + '/' + field
            hashes[key] = hashlib.sha256(values.tobytes()).hexdigest()
            differences[key] = delta
    return {'cases': len(CASES), 'steps': steps, 'final_field_hashes': hashes,
            'reference_exact_match': not any(differences.values()),
            'reference_max_absolute_error': max(differences.values()),
            'reference_field_errors': differences, 'validation_abs_tolerance': 1e-6}


def direct(args, run_id, out):
    remote_code = '''import concurrent.futures,json,shutil,subprocess,time
from pathlib import Path
root=Path(%s)
names=%s
dest=root/'runs'/%s
dest.mkdir(parents=True,exist_ok=False)
def solve(name):
    case=dest/name
    shutil.copytree(root/'cases'/name,case)
    start=time.monotonic()
    result=subprocess.run([%s],cwd=case,timeout=%s)
    if result.returncode:raise RuntimeError(name+' exited '+str(result.returncode))
    return {'case':name,'solver_wall_s':time.monotonic()-start}
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    print(json.dumps(list(pool.map(solve,names))))
'''

    def run_host(host_and_names):
        host, names = host_and_names
        code = remote_code % (repr(REMOTE), repr(names), repr(run_id), repr(SOLVER), args.timeout)
        r = subprocess.run(SSH + ['zm@' + host, 'python3 -c ' + shlex.quote(code)], capture_output=True,
                           text=True, timeout=args.timeout)
        (out / (host + '.log')).write_text(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError(f'Direct solver on {host}: ' + r.stdout + r.stderr)
        return host, json.loads(r.stdout)

    start = time.monotonic()
    assignment = [(HOSTS[0], list(CASES[::2])), (HOSTS[1], list(CASES[1::2]))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        timings = dict(pool.map(run_host, assignment))
    complete = time.monotonic() - start

    def collect(host):
        r = subprocess.run(['rsync', '-a', '-e', shlex.join(SSH),
                            f'zm@{host}:{REMOTE}/runs/{run_id}/', str(out) + '/'],
                           capture_output=True, text=True, timeout=args.timeout)
        if r.returncode:
            raise RuntimeError('Collecting direct outputs: ' + r.stderr)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(collect, HOSTS))
    return {'tool': 'direct-ssh', 'outputs_collected_s': time.monotonic() - start,
            'solves_complete_s': complete, 'case_timings_by_host': timings,
            'cases_by_node': {'zma': 2, 'zmb': 2}, 'out': str(out)}


def zeromesh(args, run_id, out):
    env = dict(os.environ, BUILD_DIR=str(ROOT / 'build-rel'),
               PYTHONPATH=str(ROOT / 'sdk/python') + ':' + str(ROOT / 'apps/zm-cfd'))
    env['PATH'] = str(args.launcher_dir) + ':/home/yash/Desktop/zm-deps/venv/bin:' + env['PATH']
    cmd = [str(ROOT / 'tests/vm/host.sh'), 'zm', 'cfd', str(args.cases),
           '--solver', SOLVER, '--cores', '1', '--batch', '1', '--remote-only',
           '--no-cache', '-o', str(out)]
    start = time.monotonic()
    with (out / 'console.log').open('w') as log:
        r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
    wall = time.monotonic() - start
    if r.returncode:
        raise RuntimeError('zm-cfd exited ' + str(r.returncode) + ': ' + (out / 'console.log').read_text())
    report = json.loads((out / 'report.json').read_text())
    items = report['items']
    if {i['id'] for i in items} != set(CASES) or any(i['state'] != 'ITEM_SUCCEEDED' or i.get('cache', {}).get('hit') for i in items):
        raise RuntimeError('Every case must freshly succeed')
    distribution = collections.Counter(i['attempts'][-1]['hostname'] for i in items)
    if set(distribution) != {'zma', 'zmb'}:
        raise RuntimeError(f'Both VMs must solve: {distribution}')
    return {'tool': 'zm-cfd', 'outputs_collected_s': wall,
            'submitted_run_s': (int(report['run']['finished_ms']) - int(report['run']['started_ms'])) / 1000,
            'cases_by_node': dict(distribution), 'attempts': sum(len(i['attempts']) for i in items),
            'retried_cases': sum(len(i['attempts']) > 1 for i in items),
            'run_id': report['run']['run_id'], 'command': cmd, 'out': str(out)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cases', type=Path, default=ROOT / 'build-rel/cfd-bench/cases')
    p.add_argument('--reference', type=Path, default=ROOT / 'build-rel/cfd-bench/reference')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--pairs', type=int, default=10)
    p.add_argument('--timeout', type=int, default=120)
    args = p.parse_args()
    args.cases, args.reference, args.out = args.cases.resolve(), args.reference.resolve(), args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    args.launcher_dir = args.out / 'bin'
    args.launcher_dir.mkdir()
    python = shutil.which('python', path='/home/yash/Desktop/zm-deps/venv/bin:' + os.environ['PATH'])
    launcher = args.launcher_dir / 'zm-cfd'
    launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(python) + ' -m zm_cfd "$@"\n')
    launcher.chmod(0o755)
    config = json.loads((ROOT / 'build-rel/cfd-bench/configuration.json').read_text())
    config.update({'pairs': args.pairs, 'warmup_pairs': 1, 'cores_per_case': 1,
                   'worker_cores_total': 4, 'baseline': 'direct parallel SSH; not TaskVine or HTCondor',
                   'baseline_input_mode': 'pre-staged source cases; per-run copy included in timing',
                   'item_cache': False, 'out': str(args.out)})
    emit({'configuration': config})
    nonce = time.time_ns()
    for pair in range(args.pairs + 1):
        order = ('direct-ssh', 'zm-cfd') if pair % 2 == 0 else ('zm-cfd', 'direct-ssh')
        hashes = {}
        for tool in order:
            out = args.out / f'{pair}-{tool}'
            out.mkdir()
            run_id = f'zmcfd-{nonce}-{pair}-{tool}'
            result = direct(args, run_id, out) if tool == 'direct-ssh' else zeromesh(args, run_id, out)
            result.update(validate(out, args.reference))
            result.update({'pair': pair, 'warmup': pair == 0})
            hashes[tool] = result['final_field_hashes']
            emit(result)
        emit({'pair_validation': pair, 'cross_tool_exact_match': hashes['direct-ssh'] == hashes['zm-cfd']})
    emit({'finished': True, 'measured_pairs': args.pairs})


if __name__ == '__main__':
    main()
