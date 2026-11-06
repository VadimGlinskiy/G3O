import csv

def simulate():
    with open(r'E:\SteamLibrary\steamapps\common\Gothic 3\G3O_PakTrace_Autosave_Slot_0001.csv', 'r') as f:
        reader = csv.DictReader(f)
        records = []
        for r in reader:
            records.append({
                'seq': int(r['seq']),
                'pak': r['pak'],
                'offset': int(r['offset'], 16),
                'in_bytes': int(r['in_bytes']),
                'out_bytes': int(r['out_bytes']),
                'decomp_us': int(r['decomp_us']),
                'load_time_ms': float(r['load_time_ms'])
            })

    # Filter candidates: blocks taking >= 1 ms (1000 us) and in GotoLoadedPosition (load_time_ms >= 12000)
    candidates = [r for r in records if r['load_time_ms'] >= 12000.0 and r['decomp_us'] >= 1000 and r['out_bytes'] <= 96 * 1024 * 1024]
    print(f"Total candidate blocks: {len(candidates)}")
    total_candidate_decomp_time = sum(c['decomp_us'] for c in candidates) / 1000.0
    print(f"Total decompression time in candidates: {total_candidate_decomp_time:.2f} ms")

    # Let's simulate timeline:
    # Load starts at t = 0 ms.
    # Worker starts at t = 500 ms (early in Pre-Load).
    # Worker decompresses at speed of ~300 MB/s uncompressed (libdeflate).
    # Main thread consumes blocks at their actual recorded load_time_ms.
    # Memory budget: 96 MB.
    max_memory = 64 * 1024 * 1024
    current_memory = 0
    worker_t = 500.0 # ms
    hits = 0
    misses = 0
    saved_time_ms = 0.0
    peak_memory = 0

    # Worker queue
    ready_blocks = {} # (pak, offset) -> {out_bytes, decomp_us, ready_t}
    cand_idx = 0
    n_cand = len(candidates)

    # Walk time in steps of main thread requests
    for r in records:
        main_t = r['load_time_ms']

        # Advance worker up to main_t as long as memory budget permits
        while cand_idx < n_cand and worker_t < main_t:
            cand = candidates[cand_idx]
            cand_out = cand['out_bytes']
            cand_dur = cand['decomp_us'] / 1000.0 # ms

            if current_memory + cand_out > max_memory and current_memory > 0:
                # Memory full, worker must wait until main thread consumes something
                break
            # Worker decompresses
            worker_t += cand_dur
            current_memory += cand_out
            if current_memory > peak_memory:
                peak_memory = current_memory
            key = (cand['pak'], cand['offset'])
            ready_blocks[key] = {
                'out_bytes': cand_out,
                'decomp_us': cand['decomp_us'],
                'ready_t': worker_t
            }
            cand_idx += 1

        # Now check if current record is in ready_blocks and ready_t <= main_t
        key = (r['pak'], r['offset'])
        if key in ready_blocks:
            blk = ready_blocks[key]
            if blk['ready_t'] <= main_t:
                hits += 1
                saved_time_ms += (blk['decomp_us'] - 150) / 1000.0 # minus ~0.15ms memcpy
                current_memory -= blk['out_bytes']
                del ready_blocks[key]
            else:
                misses += 1
                current_memory -= blk['out_bytes']
                del ready_blocks[key]

    print(f"\n--- SIMULATION RESULTS (96 MB budget, single worker) ---")
    print(f"Hits: {hits} / {len(candidates)} ({hits / len(candidates) * 100:.1f}%)")
    print(f"Misses: {misses}")
    print(f"Estimated CPU time saved: {saved_time_ms:.2f} ms ({saved_time_ms / 1000.0:.2f} s)!")
    print(f"Peak memory used: {peak_memory / (1024*1024):.2f} MB / {max_memory / (1024*1024):.0f} MB")

simulate()
