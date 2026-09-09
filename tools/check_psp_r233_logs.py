#!/usr/bin/env python3
"""r233 correctness gates; emulator GE/ME coverage is explicitly limited."""
import argparse
import hashlib
import json
from pathlib import Path
from check_psp_quad_vfpu_logs import fields,read_log


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['hardware_reference','full_control','full_audit','full_retained','six_b_audit','release']:
        p.add_argument(name)
    a=p.parse_args()
    _,hardware=read_log(a.hardware_reference)
    _,control=read_log(a.full_control)
    assert len(control)>=182
    report={'scope':'SIM checkpoints, audited XY, allocation lifetime; not real ME/GE execution','checks':[]}
    for path in [a.full_audit,a.full_retained,a.six_b_audit,a.release]:
        lines,sim=read_log(path)
        rec={'path':path,'sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest()}
        feature=fields(next(s for s in lines if s.startswith('FEATURE ')))
        safety=fields(next(s for s in lines if s.startswith('PLATFORM_SAFETY ')))
        assert safety['callback_stack']=='16384' and safety['watchdog_start']=='0'
        assert feature['QUAD_VFPU']=='0' and feature['LASER_TRIG_CACHE']=='1'
        assert feature['REPLAY_SURFACE_RECOVERY']=='1' and feature['REPLAY_SURFACE_FAILS']=='0'
        assert not any(s.startswith(('NEW_FAIL','TERMINATE','GAME_ERROR','SURFACE_DECODE_ERROR')) for s in lines)
        if path==a.release:
            assert feature['DEBUG_START_STAGE']=='0' and feature['LASER_TRIG_AUDIT']=='0'
            assert feature['AUTO_CADENCE']=='1'
            assert not any(s.startswith(('DEBUG_AUTOSTART','DEBUG_STREAM_LEASE')) for s in lines)
            assert any(s.startswith('SIM_CHECK calls=600 ') for s in lines)
            rec['release_smoke']='PASS'
        else:
            assert feature['DEBUG_START_STAGE']=='1' and feature['LASER_TRIG_AUDIT']=='1'
            if path==a.six_b_audit:
                required=set(range(600,29401,600))
                assert required<=sim.keys() and all(sim[k]==hardware[k] for k in required)
                rec['matched_checkpoints']=49
                assert 'DEBUG_AUTOSTART replay_stage=7 start=7' in lines
            else:
                assert sim==control
                rec['matched_checkpoints']=len(control)
                assert 'DEBUG_AUTOSTART replay_stage=-1 start=0' in lines
            audit=[fields(s) for s in lines if s.startswith('LASER_TRIG stats ')]
            assert audit and int(audit[-1]['compared'])>=600000
            assert all(s['calls']==s['compared'] and s['mismatch']=='0' and s['rotation_mismatch']=='0' for s in audit)
            rec['laser_audit']=audit[-1]
            assert sum(s.startswith('REPLAY_MENU scan done') for s in lines)==1
            rec['menu_return']='PASS'
        rows=[fields(s) for s in lines if s.startswith('PSPGL_STREAM_ARENA stats ')]
        assert rows and all(s['allocation_failures']=='0' and s['release_failures']=='0' and s['faulted']=='0' for s in rows)
        if path==a.full_retained:
            assert 'DEBUG_STREAM_LEASE retain=1 test_only=1' in lines
            live=[s for s in rows if int(s['lease_frames'])>0]
            assert len(live)>180 and all(s['resident_bytes']=='262144' for s in live)
            # First bootstrap Present has no lease yet, hence zero resident.
            rec['retained_256k_samples']=len(live)
        rec['arena_failures']=0
        report['checks'].append(rec)
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
