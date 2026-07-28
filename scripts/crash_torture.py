#!/usr/bin/env python3
"""Repeatedly kill the LSM writer and verify all acknowledged writes."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import tempfile
import time


def parse_metrics(output: str) -> dict[str, int]:
    return {
        key: int(value)
        for token in output.strip().split()
        for key, value in [token.split("=", 1)]
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20260728)
    args = parser.parse_args()

    binary = args.binary.resolve()
    randomizer = random.Random(args.seed)
    started = time.monotonic()
    high_water = {"acknowledged": 0, "sequence": 0, "sstables": 0}
    environment = os.environ.copy()
    environment["LSM_SPLIT_WRITES"] = "1"

    with tempfile.TemporaryDirectory(prefix="lsm-crash-") as temporary:
        root = Path(temporary)
        database = root / "database"
        oracle = root / "acknowledged.tsv"
        for crash_index in range(args.rounds):
            writer = subprocess.Popen(
                [str(binary), "workload", str(database), str(oracle)],
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(randomizer.uniform(0.001, 0.022))
            if writer.poll() is not None:
                raise RuntimeError(
                    f"writer exited before crash {crash_index}: "
                    f"{writer.stderr.read()!r}"
                )
            writer.send_signal(signal.SIGKILL)
            writer.wait(timeout=5)
            verified = subprocess.run(
                [str(binary), "verify", str(database), str(oracle)],
                check=True,
                capture_output=True,
                text=True,
            )
            metrics = parse_metrics(verified.stdout)
            for name in high_water:
                high_water[name] = max(high_water[name], metrics[name])

    result = {
        "rounds": args.rounds,
        "seed": args.seed,
        "acknowledged_writes": high_water["acknowledged"],
        "recovered_sequence": high_water["sequence"],
        "max_sstables": high_water["sstables"],
        "acknowledged_losses": 0,
        "elapsed_seconds": round(time.monotonic() - started, 3),
    }
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
