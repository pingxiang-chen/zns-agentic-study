#!/usr/bin/env python3
"""
Generate a Llama-architecture GGUF with synthetic (random) weights for the
end-to-end harness. The public model hubs are unreachable from the analysis
sandbox, so we build a 12-layer / 1024-hidden / GQA(16Q,4KV) model whose KV
geometry matches small Llama-class models (12 KiB per token at FP16). Random
weights leave every storage-relevant mechanism real: llama.cpp performs full
prefill/decode compute at this size, and --prompt-cache save/restore performs
real KV-state file I/O. Token text is meaningless; the agent harness therefore
uses a scripted tool policy (documented in the paper).

Vocab is copied from a tokenizer-only GGUF shipped in the llama.cpp repo.
Usage: python3 gen_tiny_gguf.py <vocab.gguf> <out.gguf>
"""
import sys
import numpy as np
from gguf import GGUFReader, GGUFWriter

vocab_path, out_path = sys.argv[1], sys.argv[2]

N_LAYER = 12
N_EMBD = 1024
N_HEAD = 16
N_KV_HEAD = 4
HEAD_DIM = N_EMBD // N_HEAD          # 64
N_FF = 2816
CTX = 4096

r = GGUFReader(vocab_path)
def fval(f):
    v = f.parts[f.data[0]] if len(f.data) == 1 else None
    return v

kv = {}
for field in r.fields.values():
    kv[field.name] = field

def get_str(name):
    f = kv[name]
    return bytes(f.parts[f.data[0]]).decode('utf-8')
def get_strs(name):
    f = kv[name]
    return [bytes(f.parts[i]).decode('utf-8') for i in f.data]
def get_arr(name, dtype):
    f = kv[name]
    return [dtype(f.parts[i][0]) for i in f.data]
def has(name): return name in kv

tokens = get_strs('tokenizer.ggml.tokens')
n_vocab = len(tokens)
print(f"vocab: {n_vocab} tokens, model dims: L={N_LAYER} d={N_EMBD} kv_heads={N_KV_HEAD} "
      f"-> KV/token = {N_LAYER*N_KV_HEAD*HEAD_DIM*2*2} B")

w = GGUFWriter(out_path, 'llama')
w.add_name('tiny-agentic-e2e-120M')
w.add_context_length(CTX)
w.add_embedding_length(N_EMBD)
w.add_block_count(N_LAYER)
w.add_feed_forward_length(N_FF)
w.add_head_count(N_HEAD)
w.add_head_count_kv(N_KV_HEAD)
w.add_rope_dimension_count(HEAD_DIM)
w.add_layer_norm_rms_eps(1e-5)
w.add_vocab_size(n_vocab)
w.add_rope_freq_base(10000.0)

w.add_tokenizer_model(get_str('tokenizer.ggml.model'))
if has('tokenizer.ggml.pre'):
    w.add_tokenizer_pre(get_str('tokenizer.ggml.pre'))
w.add_token_list(tokens)
if has('tokenizer.ggml.token_type'):
    w.add_token_types(get_arr('tokenizer.ggml.token_type', int))
if has('tokenizer.ggml.merges'):
    w.add_token_merges(get_strs('tokenizer.ggml.merges'))
if has('tokenizer.ggml.scores'):
    w.add_token_scores(get_arr('tokenizer.ggml.scores', float))
for name, fn in [('tokenizer.ggml.bos_token_id', w.add_bos_token_id),
                 ('tokenizer.ggml.eos_token_id', w.add_eos_token_id),
                 ('tokenizer.ggml.unknown_token_id', w.add_unk_token_id),
                 ('tokenizer.ggml.padding_token_id', w.add_pad_token_id)]:
    if has(name):
        fn(int(kv[name].parts[kv[name].data[0]][0]))

rng = np.random.default_rng(42)
def T(name, shape):
    a = (rng.standard_normal(np.prod(shape)).astype(np.float32) * 0.02).reshape(shape)
    if len(shape) == 1:
        a = np.abs(a) + 1.0            # norm weights: F32, positive around 1
        w.add_tensor(name, a.astype(np.float32))
    else:
        w.add_tensor(name, a.astype(np.float16))

T('token_embd.weight', (n_vocab, N_EMBD))
T('output_norm.weight', (N_EMBD,))
T('output.weight', (n_vocab, N_EMBD))
KVD = N_KV_HEAD * HEAD_DIM
for i in range(N_LAYER):
    p = f'blk.{i}.'
    T(p + 'attn_norm.weight', (N_EMBD,))
    T(p + 'attn_q.weight', (N_EMBD, N_EMBD))
    T(p + 'attn_k.weight', (KVD, N_EMBD))
    T(p + 'attn_v.weight', (KVD, N_EMBD))
    T(p + 'attn_output.weight', (N_EMBD, N_EMBD))
    T(p + 'ffn_norm.weight', (N_EMBD,))
    T(p + 'ffn_gate.weight', (N_FF, N_EMBD))
    T(p + 'ffn_up.weight', (N_FF, N_EMBD))
    T(p + 'ffn_down.weight', (N_EMBD, N_FF))

w.write_header_to_file()
w.write_kv_data_to_file()
w.write_tensors_to_file()
w.close()
print('wrote', out_path)
