# GPU-server harness: the same agentic workload on vLLM + LMCache and SGLang HiCache

The sandbox capture (../agent_run.py) runs llama.cpp with per-turn KV slot
save/restore. On a GPU host (alex@192.168.1.83) the identical agent loop can
drive production engines with disk-backed KV tiers. Both configurations below
emit the same oplog.jsonl schema, so ../real2trace.py and the simulator run
unchanged on the captured trace.

## Option A: vLLM + LMCache (local-disk KV backend)

```bash
pip install vllm lmcache
cat > lmcache_disk.yaml <<'EOF'
chunk_size: 256
local_device: "file:///mnt/kvcache/"      # point at the NVMeVirt namespace mount
max_local_disk_size: 12                    # GiB
remote_url: null
EOF
LMCACHE_CONFIG_FILE=lmcache_disk.yaml \
vllm serve Qwen/Qwen2.5-1.5B-Instruct \
  --kv-transfer-config '{"kv_connector":"LMCacheConnectorV1","kv_role":"kv_both"}' \
  --max-model-len 8192 --port 8123
python3 agent_run_gpu.py --api openai --port 8123 --sessions 24 --turns 8
```
LMCache persists evicted KV chunks to the disk path and reloads them on
prefix hits; mount `/mnt/kvcache` on the NVMeVirt conventional or ZNS
namespace (ZNS via zenfs/btrfs-zoned or the raw replayer) to reproduce the
device comparison end to end.

## Option B: SGLang + HiCache L3 (file storage backend)

```bash
pip install "sglang[all]"
python -m sglang.launch_server \
  --model-path Qwen/Qwen2.5-1.5B-Instruct \
  --enable-hierarchical-cache \
  --hicache-ratio 1.5 --hicache-size 8 \
  --hicache-storage-backend file \
  --hicache-storage-backend-extra-config '{"file_path":"/mnt/kvcache"}' \
  --port 8123
python3 agent_run_gpu.py --api openai --port 8123 --sessions 24 --turns 8
```

## Notes

- `agent_run_gpu.py` is `../agent_run.py` with `engine_turn()` swapped for an
  OpenAI-compatible `/v1/completions` call; KV persistence is handled by the
  engine's KV tier rather than explicit slot save/restore, so the oplog
  records the tier's disk directory growth (`du` sampling) alongside per-turn
  timings. The tool implementations (search / mem_read / mem_write / calc)
  and the turn policy are identical.
- To attribute device-level effects, run the block-level replayer
  (../../nvmevirt-kit/) or mount the KV directory on the NVMeVirt namespace
  under each personality (conv / ZNS) and compare `iostat -x` +
  `/sys/block/*/stat` tail latencies.
- Engine versions current as of July 2026: vLLM >= 0.8 with KV-connector API;
  LMCache >= 0.2; SGLang >= 0.4.7 with HiCache file backend.
