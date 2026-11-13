"""Records the backend demo: real commands in a browser terminal.

Runs on Windows with the Playwright venv. The terminal server and every
command run in WSL, in the repository copy at ~/lsm-demo.

    python demo/backend/record.py
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "kit"))
from recorder import Recorder  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "build" / "backend"
PORT = 18180
URL = f"http://localhost:{PORT}/"

# (scene, chapter title, command, regex that proves the result, timeout)
STEPS = [
    ("tests", "Test suite", "make test", r"\d{4} assertions passed", 240),
    ("asan", "AddressSanitizer + UBSan", "make asan-test CXX=clang++", r"\d{4} assertions passed", 600),
    ("sanitizers", "ThreadSanitizer", "make tsan-test CXX=clang++", r"\d{4} assertions passed", 600),
    ("crash", "Crash campaign", "make crash-test CRASH_ROUNDS=100", r'"acknowledged_losses": 0', 300),
    ("powercut", "Power-cut campaign", "make powercut-test POWERCUT_ROUNDS=60", r'"acknowledged_losses": 0', 300),
    ("mutation", "Catching a real bug", "./demo/backend/demo.sh mutation", r"✔ caught", 300),
    ("benchmark", "Performance", "make benchmark", r"read_ops_per_second=[\d.e+]+", 300),
    ("ondisk", "On disk", "ls -goh build/benchmark.db", r"MANIFEST[\s\S]*sst-\d+\.dat[\s\S]*wal-\d+\.log", 30),
    ("service", "Java service", "./demo/backend/demo.sh service", r"Tests run: \d+, Failures: 0, Errors: 0, Skipped: 0\s+BUILD SUCCESS|BUILD SUCCESS", 600),
    ("recovery", "kill -9 recovery", "./demo/backend/demo.sh recovery", r"✔ all three writes survived kill -9", 300),
]

# Keep this much of a long-running command's start and end; cut the middle.
KEEP_HEAD = 3.0
KEEP_TAIL = 3.5


def wsl(command: str, **kwargs) -> subprocess.Popen:
    return subprocess.Popen(["wsl", "-d", "Ubuntu", "--exec", "bash", "-lc", command], **kwargs)


def server_up() -> bool:
    import urllib.request
    try:
        urllib.request.urlopen(URL, timeout=1).read()
        return True
    except OSError:
        return False


def wait_for_server() -> None:
    for _ in range(100):
        if server_up():
            return
        time.sleep(0.2)
    raise RuntimeError("terminal server did not start")


def run_command(rec: Recorder, command: str, proof: str, timeout: float) -> str:
    page = rec.page
    rec.move_to("#input-line", settle=0.2)
    page.focus("#input")
    page.keyboard.type(command, delay=58)
    time.sleep(0.35)
    started = rec.now()
    page.keyboard.press("Enter")
    page.wait_for_function("() => window.term.running === true", timeout=5000)
    # Waits for the command; long waits are cut from the video.
    deadline = time.monotonic() + timeout
    cut_opened = False
    while time.monotonic() < deadline:
        if page.evaluate("() => window.term.running") is False:
            break
        if not cut_opened and rec.now() - started > KEEP_HEAD + KEEP_TAIL + 1.0:
            rec.cuts.append([started + KEEP_HEAD, None])  # closed below
            cut_opened = True
        time.sleep(0.2)
    else:
        raise TimeoutError(f"{command!r} did not finish in {timeout}s")
    finished = rec.now()
    if cut_opened:
        rec.cuts[-1][1] = round(finished - KEEP_TAIL, 3)
    exit_code = page.evaluate("() => window.term.lastExit")
    if exit_code != 0:
        raise RuntimeError(f"{command!r} exited with {exit_code}")
    return rec.wait_for_text(proof, 5, scope="#lines")


def main() -> None:
    # Use a terminal server that is already running (for example in a
    # terminal tab); otherwise start one for the length of the recording.
    server = None if server_up() else wsl(
        f"cd ~/lsm-demo && exec python3 demo/backend/terminal_server.py {PORT}")
    try:
        wait_for_server()
        results = {}
        with Recorder(OUT) as rec:
            rec.page.goto(URL)
            rec.pause(0.6)

            rec.mark("intro")
            rec.intro(
                eyebrow="Backend & systems demo",
                title="lsm-tree",
                pitch="A crash-safe LSM-tree storage engine in C++17, proven with real tests, "
                      "crash campaigns and simulated power cuts.",
                uses=["Real commands", "WSL Ubuntu 26.04", "g++ 15 · clang 21", "No mocks, no edits"],
            )
            rec.pause(10.5)
            rec.hide_card()
            rec.pause(0.6)

            for number, (scene, title, command, proof, timeout) in enumerate(STEPS, start=1):
                rec.page.evaluate("() => window.term.clear()")
                rec.mark(scene)
                rec.chapter(number, title)
                rec.pause(0.8)
                results[scene] = run_command(rec, command, proof, timeout)
                print(f"   proof: {results[scene]}", flush=True)
                # Real footage of the result for the narration to play over,
                # rather than a frozen last frame.
                rec.pause(5.5)

            rec.hide_chapter()
            rec.mark("outro")
            rec.outro(
                title="What you just saw",
                features=[
                    "8,000+ assertions, clean under TSan",
                    "0 writes lost across SIGKILL rounds",
                    "0 lost across simulated power cuts",
                    "A real fsync bug, caught on cut 0",
                    "Leveled compaction + MANIFEST log",
                    "Snapshots and concurrent readers",
                    "Block cache, Bloom filters, v3 tables",
                    "kill -9 recovery over HTTP",
                ],
                foot="<strong>github.com/Eyeroniq/lsm-tree</strong> · C++17 · Java 25 · Spring Boot",
            )
            rec.pause(10.5)
        print("results:", results, flush=True)
    finally:
        if server is not None:
            server.terminate()
            subprocess.run(["wsl", "-d", "Ubuntu", "--exec", "bash", "-lc",
                            "pkill -f terminal_server.py; true"])


if __name__ == "__main__":
    main()
