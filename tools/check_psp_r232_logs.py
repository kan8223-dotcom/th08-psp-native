#!/usr/bin/env python3
"""Validate r232 actual-6B replay/audit/recovery logs, with matched phase timings."""
import argparse
import json
from pathlib import Path
from check_psp_quad_vfpu_logs import fields, read_log


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('hardware_reference')
    p.add_argument('audit')
    p.add_argument('control')
    p.add_argument('one_failure')
    p.add_argument('three_failures')
    a = p.parse_args()
    _, reference = read_log(a.hardware_reference)
    required = set(range(600, 29401, 600))
    assert required <= reference.keys()
    result = {'reference': a.hardware_reference, 'checks': [], 'emulator_phase_comparison': []}
    logs = {}
    for path in [a.audit, a.control, a.one_failure, a.three_failures]:
        lines, sim = read_log(path)
        logs[path] = lines
        assert required <= sim.keys(), f'{path}: incomplete replay'
        assert all(sim[k] == reference[k] for k in required), f'{path}: SIM_CHECK mismatch'
        assert 'DEBUG_AUTOSTART replay_stage=7 start=7' in lines
        assert any(s.startswith('REPLAY_MENU scan done total=') for s in lines), f'{path}: no menu return'
        feature = fields(next(s for s in lines if s.startswith('FEATURE ')))
        assert feature['QUAD_VFPU'] == '0'
        stats = [fields(s) for s in lines if s.startswith('LASER_TRIG stats ')]
        if stats:
            assert all(int(s['mismatch']) == 0 and int(s['rotation_mismatch']) == 0 for s in stats)
            assert all(int(s['calls']) == int(s['hits']) + int(s['misses']) for s in stats)
        record = {'path': path, 'matched_checkpoints': 49, 'last_calls': 29400,
                  'menu_scans': sum(s.startswith('REPLAY_MENU scan done total=') for s in lines)}
        assert record['menu_scans'] == 1
        if path == a.audit:
            assert stats and int(stats[-1]['compared']) > 600000
            assert int(stats[-1]['rotations']) > 300000
            assert all(s['calls'] == s['compared'] for s in stats)
            record['laser_audit'] = stats[-1]
        recovery = [fields(s) for s in lines if s.startswith('REPLAY_SURFACE result=')]
        expected = [('READY', '1')]
        if path == a.one_failure:
            expected = [('RETRY', '1'), ('READY', '2')]
        elif path == a.three_failures:
            expected = [('RETRY', '1'), ('RETRY', '2'), ('KEEP_PREVIOUS', '3')]
        assert [(s['result'], s['attempt']) for s in recovery] == expected, f'{path}: recovery sequence'
        for s in recovery:
            if s['result'] != 'READY': assert s['old_live'] == '1'
        injected = sum(s.startswith('REPLAY_SURFACE injected_failure ') for s in lines)
        assert injected == (1 if path == a.one_failure else 3 if path == a.three_failures else 0)
        assert sum(s.startswith('SURFACE_DECODE_ERROR ') for s in lines) == injected
        record['recovery'] = recovery
        result['checks'].append(record)

    def phases(lines):
        rows = [fields(s) for s in lines if s.startswith('BULLET_UPDATE_SUB ')]
        return {int(r['calls']): {k: int(v) for k, v in r.items()} for r in rows}

    control = phases(logs[a.control])
    for path in [a.one_failure, a.three_failures]:
        candidate = phases(logs[path])
        windows = []
        for end in [8400, 9000]:
            start = end - 600
            assert all(control[k]['sf'] == candidate[k]['sf'] and
                       control[k]['active'] == candidate[k]['active'] and
                       control[k]['lasers'] == candidate[k]['lasers'] for k in [start, end])
            def delta(rows, metric):
                return ((rows[end][metric] - rows[start][metric]) & 0xffffffff) / 600 / 1000
            windows.append({'sf_start': control[start]['sf'], 'sf_end': control[end]['sf'],
                            'control_laser_ms': delta(control, 'laser_us'),
                            'candidate_laser_ms': delta(candidate, 'laser_us')})
        result['emulator_phase_comparison'].append({'candidate': path, 'windows': windows})
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
