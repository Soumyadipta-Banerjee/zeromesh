#!/usr/bin/env python3
"""Summarize a completed bench_render.py JSONL series and plot measured pairs."""
import argparse
import csv
import json
import os
from pathlib import Path
import statistics


def describe(values):
    return {'n': len(values), 'median': statistics.median(values),
            'mean': statistics.mean(values), 'sample_sd': statistics.stdev(values),
            'min': min(values), 'max': max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('jsonl', type=Path)
    parser.add_argument('--out', type=Path, required=True,
                        help='Output prefix for summary JSON, pair CSV, PNG and SVG')
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.jsonl.read_text().splitlines() if line.strip()]
    config = next(r['configuration'] for r in rows if 'configuration' in r)
    finished = next(r for r in rows if r.get('finished'))
    measured = [r for r in rows if 'tool' in r and not r['warmup']]
    pairs = []
    for pair in range(1, finished['measured_pairs'] + 1):
        runs = {r['tool']: r for r in measured if r['pair'] == pair}
        assert set(runs) == {'flamenco', 'zm-render'}
        f, z = runs['flamenco'], runs['zm-render']
        validation = next(r for r in rows if r.get('pair_validation') == pair)
        pairs.append({'pair': pair, 'first_tool': 'zm-render' if pair % 2 else 'flamenco',
                      'flamenco_collected_s': f['outputs_collected_s'],
                      'zm_render_collected_s': z['outputs_collected_s'],
                      'flamenco_render_complete_s': f['render_complete_s'],
                      'flamenco_collection_s': f['outputs_collected_s'] - f['render_complete_s'],
                      'zm_render_submitted_s': z['submitted_run_s'],
                      'zm_render_cli_minus_submitted_s': z['outputs_collected_s'] - z['submitted_run_s'],
                      'zm_over_flamenco': z['outputs_collected_s'] / f['outputs_collected_s'],
                      'zm_time_reduction': 1 - z['outputs_collected_s'] / f['outputs_collected_s'],
                      'cross_tool_exact_match': validation['cross_tool_exact_match']})
    assert len(measured) == 2 * finished['measured_pairs'] == 2 * config['pairs']
    tools = {}
    for tool in ('flamenco', 'zm-render'):
        runs = [r for r in measured if r['tool'] == tool]
        timing_fields = ('outputs_collected_s', 'render_complete_s') if tool == 'flamenco' else ('outputs_collected_s', 'submitted_run_s')
        tools[tool] = {field: describe([r[field] for r in runs]) for field in timing_fields}
        tools[tool].update({'successful_frames': sum(r['frames'] for r in runs),
                           'reference_exact_runs': sum(r['reference_exact_match'] for r in runs),
                           'reference_max_channel_error': max(r['reference_max_channel_error'] for r in runs)})
        if tool == 'zm-render':
            # Early harness versions emitted zero for an absent report field. Treat it as unknown.
            peaks = [r['peak_mem_bytes'] for r in runs if r.get('peak_mem_bytes')]
            tools[tool].update({'attempts': sum(r['attempts'] for r in runs),
                               'retried_items': sum(r['retried_items'] for r in runs),
                               'peak_mem_bytes': max(peaks) if peaks else None})
        else:
            tools[tool]['tasks'] = sum(r['tasks'] for r in runs)
    fm = tools['flamenco']['outputs_collected_s']['median']
    zm = tools['zm-render']['outputs_collected_s']['median']
    summary = {'configuration': config, 'metric': 'submission/CLI start to all frames collected on requester',
               'tools': tools, 'pairs': pairs,
               'comparison': {'median_time_reduction': 1 - zm / fm, 'median_speed_ratio': fm / zm,
                              'paired_ratios': describe([p['zm_over_flamenco'] for p in pairs]),
                              'zm_wins': sum(p['zm_over_flamenco'] < 1 for p in pairs),
                              'flamenco_wins': sum(p['zm_over_flamenco'] > 1 for p in pairs),
                              'cross_tool_exact_pairs': sum(p['cross_tool_exact_match'] for p in pairs),
                              'flamenco_collection_s': describe([p['flamenco_collection_s'] for p in pairs]),
                              'zm_cli_minus_submitted_s': describe([p['zm_render_cli_minus_submitted_s'] for p in pairs])},
               'warmup': [r for r in rows if 'tool' in r and r['warmup']]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n')
    with args.out.with_suffix('.csv').open('w') as handle:
        writer = csv.DictWriter(handle, fieldnames=list(pairs[0]))
        writer.writeheader()
        writer.writerows(pairs)
    os.environ.setdefault('MPLCONFIGDIR', '/tmp/zeromesh-matplotlib')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), layout='constrained')
    colors = {'flamenco': '#cb6b2d', 'zm-render': '#26759e'}
    for tool, field in (('flamenco', 'flamenco_collected_s'), ('zm-render', 'zm_render_collected_s')):
        values = [p[field] for p in pairs]
        axes[0].plot([p['pair'] for p in pairs], values, 'o-', label=tool, color=colors[tool])
        axes[0].axhline(statistics.median(values), color=colors[tool], ls=':', alpha=.6)
    axes[0].set(xlabel='Measured pair (alternating order)', ylabel='All frames collected (seconds)',
                title='24 CPU Cycles frames · two 2-vCPU VMs')
    axes[0].legend()
    axes[1].bar([p['pair'] for p in pairs], [100 * p['zm_time_reduction'] for p in pairs], color=colors['zm-render'])
    axes[1].axhline(0, color='#666666', lw=1)
    axes[1].set(xlabel='Measured pair', ylabel='zm-render time reduction versus Flamenco (%)',
                title=f'Median times: Flamenco {fm:.3f}s / zm-render {zm:.3f}s')
    for ax in axes:
        ax.grid(axis='y', alpha=.2)
    for extension in ('.png', '.svg'):
        fig.savefig(args.out.with_suffix(extension), dpi=160)
    plt.close(fig)
    print(json.dumps({'tools': tools, 'comparison': summary['comparison']}, indent=2))


if __name__ == '__main__':
    main()
