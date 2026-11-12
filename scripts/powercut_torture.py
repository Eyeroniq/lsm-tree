#!/usr/bin/env python3
"""Simulated power cuts: only fsynced data and names survive each crash.

Each round runs the writer with the powercut LD_PRELOAD shim, which records
what an fsync made durable. After SIGKILL, the database directory is rebuilt
from those records alone -- unsynced writes and unsynced file names vanish,
as they would after losing power -- and every write the writer acknowledged
must still be readable. Linux only.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import shutil
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


def materialize(database: Path, shadow: Path) -> None:
    """Replaces `database` with exactly what the shadow says is durable."""
    shutil.rmtree(database, ignore_errors=True)
    database.mkdir()
    names = shadow / "names"
    if not names.exists():
        return
    for line in names.read_text().splitlines():
        name, key = line.split("\t")
        contents = shadow / "inodes" / key
        # A name made durable whose contents never were: the file survives
        # but its data does not.
        data = contents.read_bytes() if contents.exists() else b""
        (database / name).write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--preload", required=True, type=Path)
    parser.add_argument("--snapshot", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20261112)
    args = parser.parse_args()

    binary = args.binary.resolve()
    preload = args.preload.resolve()
    snapshot = args.snapshot.resolve()
    randomizer = random.Random(args.seed)
    started = time.monotonic()
    high_water = {"acknowledged": 0, "sequence": 0, "sstables": 0}

    with tempfile.TemporaryDirectory(prefix="lsm-powercut-") as temporary:
        root = Path(temporary)
        database = root / "database"
        shadow = root / "shadow"
        oracle = root / "acknowledged.tsv"
        database.mkdir()
        subprocess.run([str(snapshot), str(database), str(shadow)], check=True)
        environment = os.environ.copy()
        environment.update(
            LD_PRELOAD=str(preload),
            POWERCUT_ROOT=str(database),
            POWERCUT_SHADOW=str(shadow),
            LSM_SPLIT_WRITES="1",
        )
        for cut in range(args.rounds):
            fresh = cut % 25 == 0
            if fresh and cut > 0:
                # Start over from an empty database now and then, so cuts
                # also land before the first flush of a new database.
                shutil.rmtree(database)
                database.mkdir()
                oracle.unlink(missing_ok=True)
                subprocess.run(
                    [str(snapshot), str(database), str(shadow)], check=True
                )
            # A third of the cuts land right after a randomly chosen fsync,
            # so each durability step is hit directly; the rest land at
            # random times, short or long enough for flushes and compactions.
            # The first round of a fresh database always cuts early.
            mode = 0.0 if fresh else randomizer.random()
            round_environment = dict(environment)
            if mode < 1 / 3:
                # Count every fsync, or only one kind of step; metadata
                # steps are few per round, so their counts stay small.
                kind, limit = (
                    ("any", 30) if fresh else randomizer.choice(
                        [("any", 250), ("manifest", 12), ("dir", 12),
                         ("table", 8)]
                    )
                )
                round_environment["POWERCUT_KIND"] = kind
                round_environment["POWERCUT_AFTER_FSYNCS"] = str(
                    randomizer.randint(1, limit)
                )
            writer = subprocess.Popen(
                [str(binary), "workload", str(database), str(oracle)],
                env=round_environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            if mode < 1 / 3:
                try:
                    writer.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
            elif mode < 2 / 3:
                time.sleep(randomizer.uniform(0.001, 0.022))
            else:
                time.sleep(randomizer.uniform(0.022, 0.25))
            if writer.poll() is not None and writer.returncode != -signal.SIGKILL:
                raise RuntimeError(
                    f"writer exited before power cut {cut}: "
                    f"{writer.stderr.read()!r}"
                )
            if writer.poll() is None:
                writer.send_signal(signal.SIGKILL)
            writer.wait(timeout=5)

            materialize(database, shadow)
            verified = subprocess.run(
                [str(binary), "verify", str(database), str(oracle)],
                capture_output=True,
                text=True,
            )
            if verified.returncode != 0:
                raise RuntimeError(
                    f"power cut {cut} lost data: {verified.stderr.strip()!r}"
                )
            metrics = parse_metrics(verified.stdout)
            for name in high_water:
                high_water[name] = max(high_water[name], metrics[name])
            # The recovered directory is the durable starting point of the
            # next round.
            subprocess.run([str(snapshot), str(database), str(shadow)], check=True)

    if args.rounds >= 20 and high_water["sstables"] == 0:
        raise RuntimeError(
            "power-cut campaign never observed an SSTable, so no cut landed "
            "in a flush or compaction"
        )
    print(json.dumps({
        "rounds": args.rounds,
        "seed": args.seed,
        "acknowledged_writes": high_water["acknowledged"],
        "recovered_sequence": high_water["sequence"],
        "max_sstables": high_water["sstables"],
        "acknowledged_losses": 0,
        "elapsed_seconds": round(time.monotonic() - started, 3),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
