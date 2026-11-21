"""Reproducible CLI workload; only creates fixtures in a NEW output directory."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--records", type=int, default=40)
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()
    if args.records < 10 or args.repeat < 1:
        parser.error("records must be >= 10 and repeat >= 1")
    executable = args.executable.resolve(strict=True)
    root = args.output.resolve()
    root.mkdir(exist_ok=False)
    source = root / "workload.zdb"

    def invoke(*command):
        start = time.perf_counter()
        result = subprocess.run([str(executable), *map(str, command)], capture_output=True, text=True)
        elapsed = (time.perf_counter() - start) * 1000
        with (root / "commands.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(dict(command=list(map(str, command)), exit=result.returncode,
                                      ms=elapsed, stdout=result.stdout, stderr=result.stderr)) + "\n")
        if result.returncode:
            raise RuntimeError(f"CLI failed: {command!r}: {result.stderr}")
        return result.stdout, elapsed

    def hashes(database):
        return [hashlib.sha256(file.read_bytes()).hexdigest()
                for file in (database, database.with_suffix(".wal"))]

    invoke("create", source)
    invoke("create-table", source, "events")
    for index in range(args.records):
        invoke("put", source, "events", f"key-{index:04}", bytes([index % 256] * 128).hex())
    for index in range(10):
        invoke("put", source, "events", f"key-{index:04}", bytes([255 - index] * 128).hex())
    for index in range(5):
        invoke("erase", source, "events", f"key-{index:04}")
    checkpoint_output, checkpoint_ms = invoke("checkpoint", source)
    checkpoint = json.loads(checkpoint_output)
    expected_rows = invoke("scan", source, "events")[0]
    before = json.loads(invoke("check", source)[0])
    source_hashes = hashes(source)
    wal_before = source.with_suffix(".wal").stat().st_size
    _, reduction_ms = invoke("reduce-wal", source, root / "candidate")
    candidate = root / "candidate" / source.name
    manifest = json.loads(invoke("verify-snapshot", candidate)[0])
    after = json.loads(invoke("check", candidate)[0])
    wal_after = candidate.with_suffix(".wal").stat().st_size
    assert hashes(source) == source_hashes
    assert invoke("scan", candidate, "events")[0] == expected_rows

    def measure_recovery(database, label):
        elapsed = []
        original = database.read_bytes()
        for run in range(args.repeat):
            folder = root / f"recovery-{label}-{run}"
            folder.mkdir()
            copy = folder / source.name
            for file in (database, database.with_suffix(".wal"),
                         Path(str(database) + ".baseline"), Path(str(database) + ".snapshot")):
                if file.exists():
                    shutil.copyfile(file, folder / file.name)
            # Keep the 48-byte file header and 8192-byte identity page only.
            # Startup must reconstruct all other pages, catalogs and free-list.
            with copy.open("r+b") as file:
                file.truncate(48 + 8192)
            output, ms = invoke("check", copy)
            assert json.loads(output)["pages_checked"] == before["pages_checked"]
            assert copy.read_bytes() == original
            assert invoke("scan", copy, "events")[0] == expected_rows
            elapsed.append(ms)
        return dict(samples_ms=elapsed, median_ms=statistics.median(elapsed))

    report = dict(records=args.records, updates=10, deletes=5, repeat=args.repeat,
                  pages=before["pages_checked"], wal_bytes_before=wal_before,
                  wal_bytes_after=wal_after, wal_frames_before=before["wal_records_checked"],
                  wal_frames_after=after["wal_records_checked"],
                  replaced_historical_frames=before["wal_records_checked"],
                  net_frames_removed=before["wal_records_checked"] - after["wal_records_checked"],
                  checkpoint_ms=checkpoint_ms, checkpoint=checkpoint,
                  reduction_ms=reduction_ms, manifest=manifest,
                  recovery_before=measure_recovery(source, "before"),
                  recovery_after=measure_recovery(candidate, "after"),
                  timing_scope="CLI process startup + open + full physical recovery + consistency scan",
                  source_untouched=hashes(source) == source_hashes)
    assert report["source_untouched"] and wal_after < wal_before
    (root / "measurements.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
