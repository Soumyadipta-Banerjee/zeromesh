#!/usr/bin/env python3
"""Paired real-Blender runs through Flamenco and zm-render on the two test VMs.

Flamenco uses pre-staged local scene files, with Shaman and preview encoding disabled.
Frames are collected from both workers after render completion. This is not a NAS benchmark.
"""
import argparse
import collections
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import time
import urllib.error
import urllib.request

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
HOSTS = ('192.168.122.254', '192.168.122.174')
REMOTE = '/home/zm/zeromesh-render-bench'
BLENDER = '/opt/zeromesh-render-bench/blender/bench-blender'
API = 'http://192.168.122.254:18080/api/v3'
SSH = 'ssh -i /home/yash/Desktop/zm-qemu/id_zm -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5'


def api(path, body=None):
    req = urllib.request.Request(API + path, data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            content = r.read()
            return json.loads(content) if content else {}
    except urllib.error.HTTPError as e:
        raise RuntimeError(f'{path}: HTTP {e.code}: {e.read().decode()}') from e


def emit(row):
    print(json.dumps(row), flush=True)


def validate(out, reference, frames):
    expected = {f'frame_{i:04}.png' for i in range(1, frames + 1)}
    actual = {p.name for p in out.glob('*.png')}
    if actual != expected:
        raise RuntimeError(f'{out}: missing={expected-actual}, extra={actual-expected}')
    hashes = {}
    differing = {}
    max_error = 0
    for name in sorted(expected):
        with Image.open(out / name) as img:
            pixels = np.array(img.convert('RGBA'))
        with Image.open(reference / name) as img:
            ref = np.array(img.convert('RGBA'))
        if pixels.shape != ref.shape:
            raise RuntimeError(f'{name}: dimensions {pixels.shape} vs {ref.shape}')
        delta = np.abs(pixels.astype(np.int16) - ref.astype(np.int16))
        error = int(delta.max())
        if error:
            differing[name] = {'max_channel_error': error, 'differing_pixels': int(np.any(delta != 0, axis=2).sum())}
        max_error = max(max_error, error)
        hashes[name] = hashlib.sha256(pixels.tobytes()).hexdigest()
    return {'frames': len(actual), 'pixel_hashes': hashes, 'reference_exact_match': not differing,
            'reference_max_channel_error': max_error, 'reference_differences': differing}


def flamenco(args, run_id, out):
    render_path = f'{REMOTE}/runs/{run_id}'
    body = {'name': run_id, 'type': 'simple-blender-render', 'type_etag': args.etag,
            'priority': 50, 'submitter_platform': 'linux',
            'settings': {'frames': f'1-{args.frames}', 'chunk_size': args.batch,
                         'blendfile': f'{REMOTE}/scene/benchmark.blend', 'scene': 'Scene',
                         'render_output_root': f'{REMOTE}/runs', 'add_path_components': 0,
                         'render_output_path': render_path + '/frame_####',
                         'format': 'PNG', 'image_file_extension': '.png', 'fps': 0,
                         'has_previews': False}}
    (out / 'submitted-job.json').write_text(json.dumps(body, indent=2) + '\n')
    start = time.monotonic()
    job = api('/jobs', body)
    job_id = job['id']
    deadline = start + args.timeout
    while job['status'] != 'completed':
        if job['status'] in ('failed', 'canceled'):
            (out / 'failed-job.json').write_text(json.dumps(job, indent=2) + '\n')
            tasks = api(f'/jobs/{job_id}/tasks')
            (out / 'failed-tasks.json').write_text(json.dumps(tasks, indent=2) + '\n')
            raise RuntimeError(f'Flamenco job {job_id} {job["status"]}: {tasks}')
        if time.monotonic() > deadline:
            raise TimeoutError(f'Flamenco {job_id} did not complete within {args.timeout}s')
        time.sleep(.1)
        job = api(f'/jobs/{job_id}')
    render_wall = time.monotonic() - start
    tasks = api(f'/jobs/{job_id}/tasks')
    (out / 'job.json').write_text(json.dumps(job, indent=2) + '\n')
    (out / 'tasks.json').write_text(json.dumps(tasks, indent=2) + '\n')
    task_list = tasks['tasks']
    if len(task_list) != (args.frames + args.batch - 1) // args.batch:
        raise RuntimeError(f'Unexpected Flamenco task count {len(task_list)}')
    if any(t['status'] != 'completed' for t in task_list):
        raise RuntimeError('Flamenco has unfinished tasks')
    distribution = collections.Counter(t['worker']['name'] for t in task_list)
    if set(distribution) != {'zma', 'zmb'}:
        raise RuntimeError(f'Flamenco did not use both workers: {distribution}')

    def collect(host):
        dest = out / host
        dest.mkdir()
        command = ['rsync', '-a', '-e', SSH, f'zm@{host}:{render_path}/', str(dest) + '/']
        r = subprocess.run(command, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError('Collecting Flamenco output: ' + r.stderr)
        return host, list(dest.glob('*.png'))

    files_by_host = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for host, files in pool.map(collect, HOSTS):
            files_by_host[host] = len(files)
            for p in files:
                dest = out / p.name
                if dest.exists() and dest.read_bytes() != p.read_bytes():
                    raise RuntimeError(f'Different duplicate output {p.name}')
                dest.write_bytes(p.read_bytes())
    collected_wall = time.monotonic() - start
    return {'tool': 'flamenco', 'job_id': job_id, 'render_complete_s': render_wall,
            'outputs_collected_s': collected_wall, 'tasks_by_node': dict(distribution),
            'files_by_host': files_by_host, 'tasks': len(task_list), 'out': str(out)}


def zeromesh(args, run_id, out):
    env = dict(os.environ, BUILD_DIR=str(ROOT / 'build-rel'),
               PYTHONPATH=str(ROOT / 'sdk/python') + ':' + str(ROOT / 'apps/zm-render'))
    env['PATH'] = str(args.launcher_dir) + ':/home/yash/Desktop/zm-deps/venv/bin:' + env['PATH']
    cmd = [str(ROOT / 'tests/vm/host.sh'), 'zm', 'render', str(args.scene),
           '--frames', f'1-{args.frames}', '--blender', BLENDER,
           '--engine', 'CYCLES', '--cores', '2', '--batch', str(args.batch),
           '--remote-only', '--no-cache', '-o', str(out)]
    start = time.monotonic()
    with (out / 'console.log').open('w') as log:
        r = subprocess.run(cmd, env=env, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                           timeout=args.timeout)
    wall = time.monotonic() - start
    if r.returncode:
        raise RuntimeError('zm-render exited ' + str(r.returncode) + ': ' + (out / 'console.log').read_text())
    rep = json.loads((out / 'report.json').read_text())
    items = rep['items']
    if len(items) != args.frames or any(i['state'] != 'ITEM_SUCCEEDED' or i.get('cache', {}).get('hit') for i in items):
        raise RuntimeError('zm-render did not freshly succeed every frame')
    distribution = collections.Counter(i['attempts'][-1]['hostname'] for i in items)
    if set(distribution) != {'zma', 'zmb'}:
        raise RuntimeError(f'zm-render did not use both workers: {distribution}')
    attempts = [a for i in items for a in i['attempts']]
    peaks = [int(a['peak_mem']) for a in attempts if 'peak_mem' in a]
    return {'tool': 'zm-render', 'run_id': rep['run']['run_id'], 'outputs_collected_s': wall,
            'submitted_run_s': (int(rep['run']['finished_ms']) - int(rep['run']['started_ms'])) / 1000,
            'items_by_node': dict(distribution), 'attempts': len(attempts),
            'retried_items': sum(len(i['attempts']) > 1 for i in items),
            'batches': len({a['batch'] for a in attempts}),
            'peak_mem_bytes': max(peaks) if peaks else None,
            'out': str(out), 'command': cmd}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--scene', type=Path, default=ROOT / 'build-rel/render-bench/scene/benchmark.blend')
    p.add_argument('--reference', type=Path, default=ROOT / 'build-rel/render-bench/reference')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--pairs', type=int, default=10)
    p.add_argument('--frames', type=int, default=24)
    p.add_argument('--batch', type=int, default=6)
    p.add_argument('--timeout', type=int, default=240)
    args = p.parse_args()
    args.scene = args.scene.resolve()
    args.reference = args.reference.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    # Exercise the normal `zm render` dispatcher without installing into the user's venv.
    args.launcher_dir = args.out / 'bin'
    args.launcher_dir.mkdir()
    python = shutil.which('python', path='/home/yash/Desktop/zm-deps/venv/bin:' + os.environ['PATH'])
    if not python:
        raise RuntimeError('No Python interpreter for the render adapter')
    launcher = args.launcher_dir / 'zm-render'
    launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(python) + ' -m zm_render "$@"\n')
    launcher.chmod(0o755)
    types = api('/jobs/types')['job_types']
    args.etag = next(t['etag'] for t in types if t['name'] == 'simple-blender-render')
    workers = api('/worker-mgt/workers')
    (args.out / 'workers-before.json').write_text(json.dumps(workers, indent=2) + '\n')
    assert {w['name'] for w in workers['workers']} == {'zma', 'zmb'}
    nonce = time.time_ns()
    emit({'configuration': {'scene': str(args.scene), 'scene_sha256': hashlib.sha256(args.scene.read_bytes()).hexdigest(),
                            'frames': args.frames, 'batch': args.batch, 'pairs': args.pairs,
                            'engine': 'CYCLES', 'device': 'CPU', 'resolution': [384, 384], 'samples': 8,
                            'threads_per_process': 2, 'worker_slots': 4, 'shaman': False,
                            'flamenco_input_mode': 'pre-staged local files on both workers; no NAS',
                            'video_encoding': False, 'zero_item_cache': False,
                            'flamenco_version': api('/version'), 'out': str(args.out),
                            'poll_interval_s': .1}})
    for pair in range(args.pairs + 1):
        order = ('flamenco', 'zm-render') if pair % 2 == 0 else ('zm-render', 'flamenco')
        pixel_hashes = {}
        for tool in order:
            run_id = f'zmr-{nonce}-{pair}-{tool}'
            out = args.out / f'{pair}-{tool}'
            out.mkdir()
            result = flamenco(args, run_id, out) if tool == 'flamenco' else zeromesh(args, run_id, out)
            result.update(validate(out, args.reference, args.frames))
            result.update({'pair': pair, 'warmup': pair == 0})
            pixel_hashes[tool] = result['pixel_hashes']
            emit(result)
        mismatches = [k for k in pixel_hashes['flamenco'] if pixel_hashes['flamenco'][k] != pixel_hashes['zm-render'][k]]
        emit({'pair_validation': pair, 'cross_tool_exact_match': not mismatches, 'mismatching_frames': mismatches})
    emit({'finished': True, 'measured_pairs': args.pairs})


if __name__ == '__main__':
    main()
