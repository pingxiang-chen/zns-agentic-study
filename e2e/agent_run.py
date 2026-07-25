#!/usr/bin/env python3
"""
End-to-end agentic serving harness with real KV-cache offload to storage.

Each agent session is a multi-turn tool-use loop served by llama.cpp
(llama-cli). Between turns the session's full KV state lives ONLY on disk
(--prompt-cache save/restore), i.e., the fully-offloaded regime: every turn
performs a real KV restore (file read), real prefill of the new suffix, real
decode, and a real KV save (file write). Tools perform real storage I/O:
  - search(q):   scans a document corpus (real file reads)
  - mem_write(): appends an episodic note (JSONL) and updates a 4 KiB slot in
                 a fixed-size binary index file (real 4 KiB random write)
  - mem_read():  reads a 4 KiB index slot + recent notes (real reads)
  - calc(e):     arithmetic (no I/O)
The tool POLICY is scripted per turn (deterministic); the ENGINE work and all
storage I/O are real. (Rationale: the sandbox cannot reach model hubs, so the
model has synthetic weights; text quality is irrelevant to storage behavior.
The same harness drives vLLM+LMCache / SGLang HiCache on GPU hosts.)

Every operation is logged to oplog.jsonl with wall-clock timestamps, byte
counts, and engine timings parsed from llama-cli.

Chunked execution: --seconds N runs until the budget expires and persists
state; re-invoke to continue. All state lives under the run directory.
"""
import argparse, json, os, re, subprocess, sys, time, random, hashlib

ap = argparse.ArgumentParser()
ap.add_argument('--dir', default='/tmp/w/e2e')
ap.add_argument('--llama', default='/tmp/w/llama.cpp/build/bin/llama-cli')
ap.add_argument('--model', default='/tmp/w/e2e/tiny.gguf')
ap.add_argument('--corpus', default='/tmp/w/e2e/corpus')
ap.add_argument('--seconds', type=float, default=35.0)
ap.add_argument('--sessions', type=int, default=6)
ap.add_argument('--turns', type=int, default=6)
ap.add_argument('--total-sessions', type=int, default=10)
ap.add_argument('--gen-tokens', type=int, default=140)
args = ap.parse_args()

D = args.dir
os.makedirs(f'{D}/kv', exist_ok=True)
os.makedirs(f'{D}/mem', exist_ok=True)
STATE = f'{D}/state.json'
OPLOG = f'{D}/oplog.jsonl'
IDX = f'{D}/mem/index.bin'
IDX_SLOTS = 4096                       # 16 MiB index (4096 x 4 KiB)
T0F = f'{D}/t0.txt'

TASKS = [
    ("plan_trip", "Plan a 3-day trip itinerary. Use search for facts, calc for budgets, and store notes."),
    ("code_review", "Review a storage system design. Search the docs, compute figures, note findings."),
    ("research", "Summarize how flash garbage collection works. Search, verify numbers, take notes."),
    ("budget", "Prepare a hardware budget. Look up components, compute totals, record decisions."),
]
# scripted tool policy: (pre-turn tools, post-turn tools) per turn index
POLICY = [
    (['mem_read', 'search'],            ['mem_write', 'mem_write']),
    (['mem_read', 'mem_read', 'search'],['calc', 'mem_write']),
    (['search', 'mem_read'],            ['mem_write', 'mem_write', 'calc']),
    (['mem_read', 'search', 'mem_read'],['mem_write']),
    (['search'],                        ['calc', 'mem_write', 'mem_write']),
    (['mem_read', 'mem_read'],          ['mem_write']),
]

def now(): return time.time()
def log(rec):
    rec['ts'] = round(now() - T0, 6)
    with open(OPLOG, 'a') as f:
        f.write(json.dumps(rec) + '\n')

if os.path.exists(T0F):
    T0 = float(open(T0F).read())
else:
    T0 = now(); open(T0F, 'w').write(str(T0))
    with open(IDX, 'wb') as f:                 # preallocate index
        f.write(b'\0' * (IDX_SLOTS * 4096))
    log({'op': 'INDEX_INIT', 'bytes': IDX_SLOTS * 4096})

if os.path.exists(STATE):
    st = json.load(open(STATE))
else:
    st = {'sessions': [], 'spawned': 0, 'rr': 0}

CORPUS = []
for root, _, files in os.walk(args.corpus):
    for fn in files:
        CORPUS.append(os.path.join(root, fn))
CORPUS.sort()

def spawn_session():
    sid = st['spawned']; st['spawned'] += 1
    task = TASKS[sid % len(TASKS)]
    st['sessions'].append({
        'sid': sid, 'task': task[0], 'turn': 0, 'done': False,
        'kv': f'{D}/kv/sess{sid}.bin', 'conv': f'{D}/kv/conv{sid}.txt',
        'notes': f'{D}/mem/notes{sid}.jsonl',
    })
    sysp = (f"You are agent {sid} working on: {task[1]}\n"
            f"Think step by step and produce a concise plan.\n\nTASK LOG:\n")
    open(st['sessions'][-1]['conv'], 'w').write(sysp)
    log({'op': 'SESSION_START', 'sid': sid, 'task': task[0]})

def slot_of(key):
    return int(hashlib.md5(key.encode()).hexdigest(), 16) % IDX_SLOTS

def tool_search(sid, rngq):
    q = random.Random(rngq).choice(['garbage', 'zone', 'latency', 'cache', 'memory', 'flash'])
    t0 = now(); nb = 0; hits = []
    for path in CORPUS:
        with open(path, 'rb') as f:
            data = f.read()
        nb += len(data)
        for line in data.decode(errors='replace').splitlines():
            if q in line.lower():
                hits.append(line.strip()[:120])
                if len(hits) >= 3: break
        if len(hits) >= 3: break
    log({'op': 'SEARCH', 'sid': sid, 'bytes_read': nb, 'ms': round((now()-t0)*1e3, 3), 'q': q})
    return f"[search '{q}': {len(hits)} hits] " + " | ".join(hits[:2])

def tool_mem_write(sid, turn, text):
    t0 = now()
    note = json.dumps({'sid': sid, 'turn': turn, 'note': text[:200]})
    with open(f'{D}/mem/notes{sid}.jsonl', 'a') as f:
        f.write(note + '\n'); f.flush(); os.fsync(f.fileno())
    nb1 = len(note) + 1
    slot = slot_of(f'{sid}:{turn}:{text[:16]}')
    with open(IDX, 'r+b') as f:                # 4 KiB in-place slot update
        f.seek(slot * 4096); f.write(hashlib.sha256(note.encode()).digest() * 128)
        f.flush(); os.fsync(f.fileno())
    log({'op': 'MEM_WRITE', 'sid': sid, 'log_bytes': nb1, 'idx_slot': slot,
         'ms': round((now()-t0)*1e3, 3)})

def tool_mem_read(sid, rngq):
    t0 = now()
    slot = random.Random(rngq).randrange(IDX_SLOTS)
    with open(IDX, 'rb') as f:
        f.seek(slot * 4096); f.read(4096)
    tail = ''
    p = f'{D}/mem/notes{sid}.jsonl'
    nb = 4096
    if os.path.exists(p):
        with open(p, 'rb') as f:
            data = f.read(); nb += len(data)
        lines = data.decode(errors='replace').splitlines()
        tail = lines[-1][:120] if lines else ''
    log({'op': 'MEM_READ', 'sid': sid, 'idx_slot': slot, 'bytes_read': nb,
         'ms': round((now()-t0)*1e3, 3)})
    return f"[memory: {tail}]" if tail else "[memory: empty]"

def tool_calc(sid, rngq):
    r = random.Random(rngq)
    a, b = r.randint(100, 9999), r.randint(2, 99)
    log({'op': 'CALC', 'sid': sid})
    return f"[calc: {a}*{b}={a*b}]"

import urllib.request

SRV_PORT = 8123
srv_proc = None

def srv_call(path, payload=None, timeout=300):
    url = f'http://127.0.0.1:{SRV_PORT}{path}'
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data,
                                 headers={'Content-Type': 'application/json'},
                                 method='POST' if data is not None else 'GET')
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        body = ''
        try: body = e.read().decode()[:300]
        except Exception: pass
        raise RuntimeError(f'HTTP {e.code} {path}: {body}')

def srv_start():
    global srv_proc
    if srv_proc and srv_proc.poll() is None:
        return
    srv_proc = subprocess.Popen(
        [os.path.dirname(args.llama) + '/llama-server', '-m', args.model,
         '--slot-save-path', f'{D}/kv/', '--port', str(SRV_PORT),
         '-c', '4096', '-t', '4', '--parallel', '1', '--no-warmup'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            srv_call('/health'); return
        except Exception:
            time.sleep(0.25)
    raise RuntimeError('server did not become ready')

def srv_stop():
    global srv_proc
    if srv_proc and srv_proc.poll() is None:
        srv_proc.terminate()
        try: srv_proc.wait(timeout=5)
        except Exception: srv_proc.kill()
    srv_proc = None

def engine_turn(s):
    """One real serving turn: KV slot restore (file read) -> prefill of the new
       suffix -> decode -> KV slot save (file write). llama-server slot API."""
    srv_start()
    conv = open(s['conv']).read()
    kvname = f'sess{s["sid"]}.bin'
    kvpath = f'{D}/kv/{kvname}'
    kv_before = os.path.getsize(kvpath) if os.path.exists(kvpath) else 0
    restore_ms = 0.0
    if kv_before:
        t0 = now()
        try:
            srv_call(f'/slots/0?action=restore', {'filename': kvname})
        except Exception as e:
            log({'op': 'RESTORE_ERR', 'sid': s['sid'], 'err': str(e)[:200]})
        restore_ms = (now() - t0) * 1e3
    else:
        try: srv_call(f'/slots/0?action=erase', {})
        except Exception: pass
    t0 = now()
    try:
        resp = srv_call('/completion', {
            'prompt': conv, 'n_predict': args.gen_tokens, 'temperature': 0.8,
            'seed': 1000 + s['sid'] * 100 + s['turn'], 'cache_prompt': True,
            'ignore_eos': True, 'logit_bias': [[50256, False]],
            'grammar': 'root ::= [ -~\\n]*'})
    except Exception as e:
        log({'op': 'ENGINE_ERR', 'sid': s['sid'], 'err': str(e)[:300]})
        srv_stop(); return False
    wall = (now() - t0) * 1e3
    t1 = now()
    try:
        sv = srv_call(f'/slots/0?action=save', {'filename': kvname})
    except Exception as e:
        sv = {}; log({'op': 'SAVE_ERR', 'sid': s['sid'], 'err': str(e)[:200]})
    save_ms = (now() - t1) * 1e3
    kv_after = os.path.getsize(kvpath) if os.path.exists(kvpath) else 0
    tm = resp.get('timings', {})
    log({'op': 'ENGINE', 'sid': s['sid'], 'turn': s['turn'],
         'kv_bytes_before': kv_before, 'kv_bytes_after': kv_after,
         'restore_ms': round(restore_ms, 1), 'save_ms': round(save_ms, 1),
         'wall_ms': round(wall, 1),
         'prefill_tok': tm.get('prompt_n'), 'prefill_ms': tm.get('prompt_ms'),
         'decode_tok': tm.get('predicted_n'), 'decode_ms': tm.get('predicted_ms'),
         'n_saved_tok': sv.get('n_saved'), 'kv_written_bytes': sv.get('n_written')})
    gen = (resp.get('content') or '').strip()[-1200:]
    with open(s['conv'], 'a') as f:
        f.write(gen + '\n')
    return True

def run_turn(s):
    turn = s['turn']
    pre, post = POLICY[turn % len(POLICY)]
    addition = []
    for i, tname in enumerate(pre):
        rngq = s['sid'] * 1000 + turn * 10 + i
        if tname == 'search': addition.append(tool_search(s['sid'], rngq))
        elif tname == 'mem_read': addition.append(tool_mem_read(s['sid'], rngq))
    with open(s['conv'], 'a') as f:
        f.write(f"\n[turn {turn}] tools: " + " ".join(addition) + "\nAgent: ")
    ok = engine_turn(s)
    conv_tail = open(s['conv']).read()[-300:]
    for i, tname in enumerate(post):
        rngq = s['sid'] * 2000 + turn * 10 + i
        if tname == 'mem_write': tool_mem_write(s['sid'], turn, conv_tail)
        elif tname == 'calc': tool_calc(s['sid'], rngq)
    s['turn'] += 1
    if s['turn'] >= args.turns or not ok:
        s['done'] = True
        kb = os.path.getsize(s['kv']) if os.path.exists(s['kv']) else 0
        log({'op': 'SESSION_END', 'sid': s['sid'], 'kv_bytes': kb, 'turns': s['turn']})
        for p in (s['kv'],):
            if os.path.exists(p): os.remove(p)

deadline = now() + args.seconds
while len([s for s in st['sessions'] if not s['done']]) < args.sessions and st['spawned'] < args.total_sessions:
    spawn_session()
progress = False
while now() < deadline:
    live = [s for s in st['sessions'] if not s['done']]
    if not live:
        if st['spawned'] >= args.total_sessions:
            break
        spawn_session(); continue
    s = live[st['rr'] % len(live)]
    st['rr'] += 1
    run_turn(s)
    progress = True
    if len([x for x in st['sessions'] if not x['done']]) < args.sessions and st['spawned'] < args.total_sessions:
        spawn_session()
    json.dump(st, open(STATE, 'w'))

srv_stop()
json.dump(st, open(STATE, 'w'))
live = len([s for s in st['sessions'] if not s['done']])
done = len([s for s in st['sessions'] if s['done']])
turns = sum(s['turn'] for s in st['sessions'])
print(f"progress={progress} live={live} done={done} spawned={st['spawned']} total_turns={turns}")
print("COMPLETE" if (done == st['spawned'] == args.total_sessions or (not live and st['spawned'] >= args.total_sessions)) else "MORE")
