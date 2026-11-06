"""Offline trace verification: read-only archives, zlib vs project libdeflate.
Usage: py -3.12 tools/verify_pak_trace.py TRACE DATA_DIR DEFLATE_DLL REPORT
Does not prove equality with the original engine output (not captured in CSV).
"""
import csv
import ctypes as c
import hashlib
import json
from pathlib import Path
import sys
import zlib


def decode_reference(data, expected):
    dec = zlib.decompressobj()
    out = dec.decompress(data, expected + 1)
    if not dec.eof or dec.unused_data or dec.unconsumed_tail or len(out) != expected:
        raise ValueError('Invalid zlib stream, trailing bytes, or output size mismatch')
    return out


def main():
    trace, root, dll, report = map(Path, sys.argv[1:])
    lib = c.CDLL(str(dll.resolve()))
    lib.libdeflate_alloc_decompressor.restype = c.c_void_p
    lib.libdeflate_free_decompressor.argtypes = [c.c_void_p]
    fn = lib.libdeflate_zlib_decompress
    fn.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    fn.restype = c.c_int
    ctx = lib.libdeflate_alloc_decompressor()
    if not ctx:
        raise MemoryError('libdeflate allocation failed')

    def verify(data, size):
        reference = decode_reference(data, size)
        output = c.create_string_buffer(size)
        actual = c.c_size_t()
        result = fn(ctx, data, len(data), output, size, c.byref(actual))
        if result != 0 or actual.value != size or output.raw != reference:
            raise ValueError('libdeflate differs from zlib')
        return hashlib.sha256(reference).hexdigest()

    result = {'trace': str(trace), 'trace_sha256': hashlib.sha256(trace.read_bytes()).hexdigest(),
              'library_sha256': hashlib.sha256(dll.read_bytes()).hexdigest(),
              'mode': 'offline x64; no engine-output comparison', 'blocks': [], 'errors': []}
    try:
        sample = b'G3O trace validation\0' * 1000
        verify(zlib.compress(sample), len(sample))
        for invalid, size in [(b'bad', 10), (zlib.compress(sample)[:-1], len(sample)),
                              (zlib.compress(sample), len(sample) - 1),
                              (zlib.compress(sample) + b'extra', len(sample))]:
            try:
                decode_reference(invalid, size)
            except (ValueError, zlib.error):
                pass
            else:
                raise AssertionError('Invalid fixture accepted')
        result['self_tests'] = 'passed'
        rows = list(csv.DictReader(trace.open(newline='', encoding='utf-8-sig')))
        result['records'] = len(rows)
        seen = set()
        for row in rows:
            key = (row['pak'], row['offset'], row['in_bytes'], row['out_bytes'])
            if key in seen:
                continue
            seen.add(key)
            try:
                name = row['pak']
                if Path(name).name != name or '/' in name or '\\' in name or ':' in name:
                    raise ValueError('Unsafe archive name')
                archive = root / name
                offset, packed, size = int(row['offset'], 16), int(row['in_bytes']), int(row['out_bytes'])
                if not (0 < packed <= 128*1024*1024 and 0 < size <= 128*1024*1024 and offset >= 0):
                    raise ValueError('Block outside verifier size bounds')
                before = archive.stat()
                if offset + packed > before.st_size:
                    raise ValueError('Block outside archive')
                with archive.open('rb') as f:
                    f.seek(offset)
                    data = f.read(packed)
                if len(data) != packed:
                    raise ValueError('Short read')
                digest = verify(data, size)
                after = archive.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise ValueError('Archive changed during read')
                result['blocks'].append({'pak': name, 'offset': row['offset'], 'in_bytes': packed,
                                         'out_bytes': size, 'sha256': digest,
                                         'archive_size': before.st_size, 'archive_mtime_ns': before.st_mtime_ns})
            except (OSError, ValueError, zlib.error) as exc:
                result['errors'].append({'block': key, 'error': str(exc)})
        result['unique_blocks'] = len(seen)
        result['verified'] = len(result['blocks'])
        result['passed'] = bool(rows) and not result['errors'] and result['verified'] == len(seen)
        report.parent.mkdir(parents=True, exist_ok=True)
        with report.open('x', encoding='utf-8') as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print(json.dumps({k: v for k, v in result.items() if k not in ('blocks', 'errors')}))
        print('Errors:', result['errors'][:10])
        return 0 if result['passed'] else 1
    finally:
        lib.libdeflate_free_decompressor(ctx)


if __name__ == '__main__':
    sys.exit(main())
