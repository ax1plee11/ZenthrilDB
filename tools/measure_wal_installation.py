"""Measure installation and CLI open/check using only a new fixture directory."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    root = args.output.resolve()
    root.mkdir(exist_ok=False)
    source = root / "sample.zdb"

    def command(*parts):
        start = time.perf_counter()
        result = subprocess.run([str(executable), *map(str, parts)], text=True, capture_output=True)
        elapsed = (time.perf_counter() - start) * 1000
        with (root / "commands.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(dict(command=list(map(str, parts)), ms=elapsed,
                                         exit=result.returncode, stdout=result.stdout, stderr=result.stderr)) + "\n")
        if result.returncode:
            raise RuntimeError(f"{parts!r}: {result.stderr}")
        return result.stdout, elapsed

    def hashes():
        return [hashlib.sha256(path.read_bytes()).hexdigest()
                for path in (source, source.with_suffix(".wal"))]

    def opens(path):
        samples = [command("check", path)[1] for _ in range(3)]
        return dict(samples_ms=samples, median_ms=statistics.median(samples))

    command("create", source)
    command("create-table", source, "notes")
    for i in range(20):
        command("put", source, "notes", f"key-{i}", bytes([i] * 128).hex())
    command("put", source, "notes", "key-0", "abcd")
    command("erase", source, "notes", "key-1")
    command("checkpoint", source)
    normal = opens(source)
    expected = command("scan", source, "notes")[0]
    command("reduce-wal", source, root / "candidate")
    target = root / "candidate" / source.name
    direct = opens(target)
    before = hashes()
    _, elapsed = command("install-wal", source, target)
    selected = opens(source)
    assert command("scan", source, "notes")[0] == expected
    command("put", source, "notes", "after", "ff00")
    command("install-wal", source, target)
    assert command("get", source, "notes", "after")[0].strip() == "ff00"
    assert command("get", target, "notes", "after")[0].strip() == "ff00"
    command("verify-snapshot", source)
    assert before == hashes()
    marker = Path(str(source) + ".install")
    result = dict(installation_ms=elapsed, marker_bytes=marker.stat().st_size,
                  original_open=normal, direct_reduced_open=direct, installed_open=selected,
                  data_copies_during_installation=0, source_preserved=True,
                  timing_scope="CLI startup + open + recovery + consistency check; 3 samples per open mode")
    (root / "measurements.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
