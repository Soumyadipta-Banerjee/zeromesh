#!/usr/bin/env python3
"""Summarize a completed real-CFD series from bench_cfd.py."""
import argparse
import csv
import json
import os
from pathlib import Path
import statistics

from summarize_render import describe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('jsonl', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    rows = [json.loads(line) for line in args.jsonl.read_text().splitlines() if line.strip()]
    assert rows[-1].get('finished')
    config = rows[0]['configuration']
    measured = [r for r in rows if 'tool' in r and not r['warmup']]
    pairs = []
    for pair in range(1, config['pairs'] + 1):
        runs = {r['tool']: r for r in measured if r['pair'] == pair}
        assert set(runs) == {'direct-ssh', 'zm-cfd'}
        d, z = runs['direct-ssh'], runs['zm-cfd']
        ratio = z['outputs_collected_s'] / d['outputs_collected_s']
        v = next(r for r in rows if r.get('pair_validation') == pair)
        pairs.append({'pair': pair, 'first_tool': 'zm-cfd' if pair % 2 else 'direct-ssh',
                      'direct_collected_s': d['outputs_collected_s'], 'zm_cfd_collected_s': z['outputs_collected_s'],
                      'direct_solves_complete_s': d['solves_complete_s'],
                      'direct_collection_s': d['outputs_collected_s'] - d['solves_complete_s'],
                      'zm_submitted_run_s': z['submitted_run_s'],
                      'zm_cli_minus_submitted_s': z['outputs_collected_s'] - z['submitted_run_s'],
                      'zm_over_direct': ratio, 'zm_time_reduction': 1 - ratio,
                      'cross_tool_exact_match': v['cross_tool_exact_match']})
    assert len(measured) == 2 * config['pairs']
    tools = {}
    for tool in ('direct-ssh', 'zm-cfd'):
        runs = [r for r in measured if r['tool'] == tool]
        fields = ('outputs_collected_s', 'solves_complete_s') if tool == 'direct-ssh' else ('outputs_collected_s', 'submitted_run_s')
        tools[tool] = {f: describe([r[f] for r in runs]) for f in fields}
        distribution = {}
        for r in runs:
            for node, count in r['cases_by_node'].items():
                distribution[node] = distribution.get(node, 0) + count
        tools[tool].update({'successful_cases': sum(r['cases'] for r in runs),
                           'reference_exact_runs': sum(r['reference_exact_match'] for r in runs),
                           'reference_max_absolute_error': max(r['reference_max_absolute_error'] for r in runs),
                           'cases_by_node': distribution})
        if tool == 'zm-cfd':
            tools[tool].update({'attempts': sum(r['attempts'] for r in runs),
                               'retried_cases': sum(r['retried_cases'] for r in runs)})
    dm, zm = (tools[t]['outputs_collected_s']['median'] for t in ('direct-ssh', 'zm-cfd'))
    comparison = {'median_time_reduction': 1 - zm / dm, 'median_speed_ratio': dm / zm,
                  'paired_ratios': describe([v['zm_over_direct'] for v in pairs]),
                  'zm_wins': sum(v['zm_over_direct'] < 1 for v in pairs),
                  'direct_wins': sum(v['zm_over_direct'] > 1 for v in pairs),
                  'cross_tool_exact_pairs': sum(v['cross_tool_exact_match'] for v in pairs),
                  'direct_collection_s': describe([v['direct_collection_s'] for v in pairs]),
                  'zm_cli_minus_submitted_s': describe([v['zm_cli_minus_submitted_s'] for v in pairs])}
    result = {'configuration': config, 'metric': 'start to all case outputs collected on laptop',
              'tools': tools, 'pairs': pairs, 'comparison': comparison,
              'warmup': [r for r in rows if 'tool' in r and r['warmup']]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    with args.out.with_suffix('.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(pairs[0]))
        writer.writeheader()
        writer.writerows(pairs)
    os.environ.setdefault('MPLCONFIGDIR', '/tmp/zeromesh-matplotlib')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8, 4), layout='constrained')
    for tool, field, color in (('direct SSH', 'direct_collected_s', '#727b83'), ('zm-cfd', 'zm_cfd_collected_s', '#26759e')):
        values = [v[field] for v in pairs]
        ax.plot([v['pair'] for v in pairs], values, 'o-', color=color, label=tool)
        ax.axhline(statistics.median(values), color=color, ls=':', alpha=.6)
    ax.set(xlabel='Measured pair (alternating order)', ylabel='All case outputs collected (seconds)',
           title=f'Four independent CFD cases · same four VM cores\nMedian: direct SSH {dm:.3f}s / zm-cfd {zm:.3f}s')
    ax.legend()
    ax.grid(axis='y', alpha=.2)
    for ext in ('.png', '.svg'):
        fig.savefig(args.out.with_suffix(ext), dpi=160)
    plt.close(fig)
    print(json.dumps({'tools': tools, 'comparison': comparison}, indent=2))


if __name__ == '__main__':
    main()
