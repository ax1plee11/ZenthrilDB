"""New fixtures only; reference CLI is used BEFORE any selector exists."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('cli', type=Path)
parser.add_argument('tests', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--reference', type=Path)
args = parser.parse_args()
cli, tests = args.cli.resolve(strict=True), args.tests.resolve(strict=True)
root = args.output.resolve()
root.mkdir(exist_ok=False)

def run(executable, *parts):
    start = time.perf_counter()
    result = subprocess.run([str(executable), *map(str, parts)], text=True, capture_output=True)
    elapsed = (time.perf_counter() - start) * 1000
    with (root / 'commands.jsonl').open('a', encoding='utf-8') as log:
        log.write(json.dumps(dict(executable=str(executable), arguments=list(map(str, parts)),
                                 ms=elapsed, exit=result.returncode, stdout=result.stdout, stderr=result.stderr)) + '\n')
    if result.returncode:
        raise RuntimeError(result.stderr + repr(parts))
    return result.stdout, elapsed

source = root / 'reference.zdb'
run(cli, 'create', source)
run(cli, 'create-table', source, 'records')
for record in range(10):
    run(cli, 'put', source, 'records', str(record), '010203')
before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, source.with_suffix('.wal'))]
summary = {}
for label, executable in [('reference_cli', args.reference), ('current_cli', cli)]:
    if executable:
        actual = executable.resolve(strict=True)
        samples = [run(actual, 'check', source)[1] for _ in range(3)]
        summary[label] = dict(samples_ms=samples, median_ms=statistics.median(samples))
assert before == [hashlib.sha256(p.read_bytes()).hexdigest() for p in (source, source.with_suffix('.wal'))]
summary['in_process'] = json.loads(run(tests, '--lifecycle-benchmark', root / 'in-process')[0])
summary['source_preserved'] = True
summary['reference_scope'] = 'Same closed v1 fixture, no installation selector; CLI includes process startup and close'
(root / 'measurements.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
print(json.dumps(summary, indent=2))
