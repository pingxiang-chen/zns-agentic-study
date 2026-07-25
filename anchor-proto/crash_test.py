#!/usr/bin/env python3
"""Crash-injection harness for the Anchor durability prototype.

Each trial: start the crashable workload, SIGKILL it after a random delay,
run recovery, verify every acknowledged operation against the oracle log.
State persists across invocations (chunked execution); results accumulate
in results.json.
"""
import json, os, random, signal, subprocess, sys, time

BIN = sys.argv[1] if len(sys.argv) > 1 else './anchor_meta'
DIR = sys.argv[2] if len(sys.argv) > 2 else '/tmp/w/anchor_t'
BUDGET = float(sys.argv[3]) if len(sys.argv) > 3 else 35.0
RES = DIR + '_results.json'

st = json.load(open(RES)) if os.path.exists(RES) else dict(
    trials=0, ok=0, lost_committed=0, verify_fail=0, recover_ms=[], seed=1000)
deadline = time.time() + BUDGET
rng = random.Random(st['seed'])

while time.time() < deadline:
    subprocess.run(['rm', '-rf', DIR])
    subprocess.run([BIN, 'format', DIR, '48'], capture_output=True)
    p = subprocess.Popen([BIN, 'work', DIR, str(rng.randrange(1 << 30)), '100000'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(rng.uniform(0.03, 0.6))
    p.send_signal(signal.SIGKILL); p.wait()
    r = subprocess.run([BIN, 'recover', DIR], capture_output=True, text=True)
    st['trials'] += 1
    if r.returncode == 3:
        st['lost_committed'] += 1
    elif r.returncode != 0:
        st['verify_fail'] += 1
    else:
        for tok in r.stdout.split():
            if tok.startswith('ms='):
                st['recover_ms'].append(float(tok[3:]))
        v = subprocess.run([BIN, 'verify', DIR], capture_output=True, text=True)
        if v.returncode == 0:
            st['ok'] += 1
        else:
            st['verify_fail'] += 1
            sys.stderr.write(v.stderr[-300:] + '\n')
    st['seed'] = rng.randrange(1 << 30)
    json.dump(st, open(RES, 'w'))

ms = sorted(st['recover_ms'])
print(json.dumps(dict(trials=st['trials'], ok=st['ok'],
                      lost_committed=st['lost_committed'],
                      verify_fail=st['verify_fail'],
                      recover_ms_p50=ms[len(ms)//2] if ms else 0,
                      recover_ms_max=ms[-1] if ms else 0)))
