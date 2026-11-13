"""Records the dashboard demo by driving the real UI.

Runs on Windows with the Playwright venv; the server runs in WSL from the
repository copy at ~/lsm-demo, on a fresh database with the demo profile.

    python demo/dashboard/record.py
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "kit"))
from recorder import Recorder  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "build" / "dashboard"
PORT = 18080
BASE = f"http://localhost:{PORT}"
JAR = "target/lsm-tree-server-0.1.0.jar"
SERVER = (f"cd ~/lsm-demo/server && exec java --enable-native-access=ALL-UNNAMED -jar {JAR} "
          f"--spring.profiles.active=demo --server.port={PORT}")


def wsl(command: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "-d", "Ubuntu", "--exec", "bash", "-lc", command],
                          capture_output=True, text=True)


def start_server() -> subprocess.Popen:
    return subprocess.Popen(["wsl", "-d", "Ubuntu", "--exec", "bash", "-lc", SERVER],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_healthy(timeout: float = 60) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if b'"UP"' in urllib.request.urlopen(f"{BASE}/actuator/health", timeout=1).read():
                return
        except OSError:
            pass
        time.sleep(0.25)
    raise RuntimeError("server did not become healthy")


def scroll_to(rec: Recorder, selector: str) -> None:
    rec.page.evaluate(
        "(s) => document.querySelector(s).scrollIntoView({behavior: 'smooth', block: 'center'})", selector)
    rec.pause(0.9)


STYLE_JSON = """(path) => {
  // Presents the endpoint's real JSON response, pretty-printed and styled
  // like the dashboard, instead of the browser's plain JSON viewer.
  const data = JSON.parse(document.body.innerText.replace(/^Pretty-print\\s*/, ''));
  const text = JSON.stringify(data, null, 2)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/("[^"]*")(\\s*:)/g, '<span style="color:#a78bfa">$1</span>$2')
    .replace(/: ("[^"]*")/g, ': <span style="color:#4ade80">$1</span>')
    .replace(/: (-?[\\d.]+)/g, ': <span style="color:#22d3ee">$1</span>');
  document.head.innerHTML = '';
  document.body.innerHTML = `<div style="padding:46px 56px;font:17px/1.6 'Cascadia Mono',Consolas,monospace;color:#d6dbe8">
    <div style="font:600 15px 'Segoe UI',sans-serif;color:#8a90a2;margin-bottom:18px">
      <span style="color:#0b0d14;background:linear-gradient(135deg,#8b5cf6,#22d3ee);padding:4px 10px;border-radius:7px;font-weight:800;margin-right:10px">GET</span>${path}</div>
    <pre style="margin:0;white-space:pre-wrap">${text}</pre></div>`;
  document.documentElement.style.cssText =
    'min-height:100%;background:radial-gradient(900px 500px at 0% 0%, rgba(139,92,246,.14), transparent 60%) fixed, #0a0d16';
  document.body.style.cssText = 'margin:0;min-height:100vh;background:transparent';
}"""


def show_json(rec: Recorder, path: str) -> None:
    rec.page.goto(f"{BASE}{path}")
    rec.page.evaluate(STYLE_JSON, path)


def clear_and_type(rec: Recorder, selector: str, text: str) -> None:
    rec.click(selector, settle=0.15)
    rec.page.keyboard.press("Control+A")
    rec.page.keyboard.press("Backspace")
    rec.page.keyboard.type(text, delay=60)


def main() -> None:
    wsl("pkill -f '[l]sm-tree-server-0.1.0.jar' ; rm -rf ~/lsm-demo/server/demo-data ; true")
    server = start_server()
    wait_healthy()
    proofs = {}
    try:
        with Recorder(OUT) as rec:
            page = rec.page
            page.goto(f"{BASE}/")
            rec.wait_for_text(r"Live", 10, scope="#status")

            rec.mark("intro")
            rec.intro(
                eyebrow="Full-stack demo",
                title="lsm-tree",
                pitch="A crash-safe database engine in C++17, served by Spring Boot, "
                      "with a live dashboard of what happens inside.",
                uses=["Real engine", "Java 25 · Spring Boot 4", "Live data, no mocks", "WSL Ubuntu"],
            )
            rec.pause(11.0)
            rec.hide_card()
            rec.pause(0.5)

            rec.mark("overview")
            rec.chapter(1, "Live dashboard")
            for selector in ["#stat-writes", "#stat-tables", "#stat-cache", "#levels"]:
                rec.move_to(selector, settle=0.55)
            rec.click("#theme-toggle")
            rec.pause(1.6)
            assert page.evaluate("() => document.documentElement.dataset.theme") == "light"
            rec.click("#theme-toggle")
            rec.pause(0.8)
            assert page.evaluate("() => document.documentElement.dataset.theme") == "dark"
            rec.move_to("#load", settle=0.5)

            rec.mark("write")
            rec.chapter(2, "Write 5,000 keys")
            rec.click("#load")
            clicked = rec.now()
            proofs["write"] = rec.wait_for_text(r"Wrote 5,000 keys in [\d.]+ s", 180, scope="#feed")
            # Each write is fsynced, so this can take a while: keep the start
            # and the end of the tree filling up, and cut the middle.
            if rec.now() - clicked > 20:
                rec.cuts.append([round(clicked + 9, 3), round(rec.now() - 7, 3)])
            rec.wait_for_text(r"Compaction rewrote", 10, scope="#feed")
            tables = int(page.inner_text("#stat-tables").replace(",", ""))
            assert tables > 0, "no tables after writing"
            rec.move_to("#levels", settle=0.4)
            rec.pause(2.5)

            rec.mark("read")
            rec.chapter(3, "Read 5,000 keys")
            rec.click("#read")
            proofs["read"] = rec.wait_for_text(r"Read 5,000 keys \([\d,]+ found\)", 60, scope="#feed")
            rec.pause(1.2)
            bloom = page.inner_text("#stat-bloom")
            cache = page.inner_text("#stat-cache")
            assert re.fullmatch(r"\d+%", bloom) and re.fullmatch(r"\d+%", cache), (bloom, cache)
            proofs["read"] += f" · bloom {bloom} · cache {cache}"
            rec.move_to("#stat-bloom", settle=1.4)
            rec.move_to("#stat-cache", settle=1.6)
            rec.move_to("#levels", settle=1.4)
            rec.move_to("#feed", settle=2.2)

            rec.mark("explorer")
            rec.chapter(4, "Key explorer")
            scroll_to(rec, "#kv-form")
            clear_and_type(rec, "#kv-key", "user:42")
            clear_and_type(rec, "#kv-value", "Ada Lovelace")
            rec.click('#kv-form [data-action="put"]')
            rec.wait_for_text(r"(?i)saved\s*user:42 = Ada Lovelace", 10, scope="#kv-result")
            rec.pause(0.6)
            clear_and_type(rec, "#kv-value", "")
            rec.click('#kv-form [data-action="get"]')
            proofs["explorer"] = rec.wait_for_text(r"(?i)found\s*user:42 = Ada Lovelace", 10, scope="#kv-result")
            rec.move_to("#kv-result", settle=2.6)

            rec.mark("delete")
            rec.chapter(5, "Delete")
            clear_and_type(rec, "#kv-key", "user:7")
            clear_and_type(rec, "#kv-value", "Grace Hopper")
            rec.click('#kv-form [data-action="put"]')
            rec.wait_for_text(r"(?i)saved\s*user:7 = Grace Hopper", 10, scope="#kv-result")
            rec.pause(0.8)
            rec.click('#kv-form [data-action="delete"]')
            rec.wait_for_text(r"(?i)deleted\s*user:7", 10, scope="#kv-result")
            rec.pause(0.9)
            clear_and_type(rec, "#kv-value", "")
            rec.click('#kv-form [data-action="get"]')
            proofs["delete"] = rec.wait_for_text(r"(?i)not found\s*user:7", 10, scope="#kv-result")
            rec.move_to("#kv-result", settle=2.4)

            rec.mark("scan")
            rec.chapter(6, "Range scan")
            clear_and_type(rec, "#scan-prefix", "demo:000")
            rec.click("#scan-form .button")
            proofs["scan"] = rec.wait_for_text(r"\d+ keys|first \d+", 10, scope="#scan-count")
            keys = page.locator("#scan-results .scan-key").all_inner_texts()
            assert keys and keys == sorted(keys) and all(k.startswith("demo:000") for k in keys), keys[:5]
            rec.move_to("#scan-results", settle=1.2)
            for _ in range(3):
                page.mouse.wheel(0, 170)
                rec.pause(1.3)
            page.mouse.wheel(0, -600)
            rec.pause(1.4)

            rec.mark("compact")
            rec.chapter(7, "Compaction")
            scroll_to(rec, ".tree")
            before = page.evaluate("() => [...document.querySelectorAll('#levels .level-count')].map(e => e.textContent)")
            rec.click("#compact")
            rec.wait_for_text(r"Full compaction finished", 60, scope="#toasts")
            rec.pause(1.4)
            after = page.evaluate("() => [...document.querySelectorAll('#levels .level-count')].map(e => e.textContent)")
            nonzero = [level for level, count in enumerate(after) if count != "0"]
            assert len(nonzero) == 1, after
            proofs["compact"] = f"levels {before} -> {after}"
            rec.move_to("#levels", settle=2.2)
            rec.move_to("#feed", settle=2.6)

            rec.mark("crash")
            rec.chapter(8, "kill -9 and recover")
            rec.pause(0.8)
            killed = wsl("pkill -9 -f '[l]sm-tree-server-0.1.0.jar'; echo $?")
            assert killed.stdout.strip() == "0", killed
            server.wait(timeout=10)
            rec.wait_for_text(r"Offline", 10, scope="#status")
            rec.pause(1.6)
            cut_from = rec.now()
            server = start_server()
            wait_healthy()
            rec.wait_for_text(r"Live", 10, scope="#status")
            rec.cuts.append([cut_from + 0.4, rec.now() - 0.6])
            rec.pause(0.8)
            scroll_to(rec, "#kv-form")
            clear_and_type(rec, "#kv-key", "user:42")
            clear_and_type(rec, "#kv-value", "")
            rec.click('#kv-form [data-action="get"]')
            proofs["crash"] = rec.wait_for_text(r"(?i)found\s*user:42 = Ada Lovelace", 10, scope="#kv-result")
            rec.pause(1.8)

            rec.mark("api")
            rec.chapter(9, "REST API")
            page.goto(f"{BASE}/swagger-ui.html")
            rec.wait_for_text(r"LSM-tree key-value API", 20)
            rec.chapter(9, "REST API")
            rec.pause(0.8)
            rec.move_to("text=Key-value", settle=0.6)
            page.mouse.wheel(0, 360)
            rec.pause(1.0)
            # Swagger UI marks each operation with its path; CSS, not a text
            # selector, because "text=/..." would be read as a regex.
            endpoint = "[data-path='/api/v1/kv/{key}']"
            if page.locator(endpoint).count() == 0:
                endpoint = ".opblock-summary"
            rec.move_to(f"{endpoint} >> nth=0", settle=1.2)
            page.mouse.wheel(0, 300)
            rec.pause(1.6)
            page.mouse.wheel(0, 300)
            rec.pause(1.8)

            rec.mark("metrics")
            show_json(rec, "/actuator/metrics/lsm.sstables")
            proofs["metrics"] = rec.wait_for_text(r'"name"\s*:\s*"lsm\.sstables"', 10)
            rec.chapter(10, "Actuator metrics")
            rec.move_to("pre", settle=3.2)
            show_json(rec, "/actuator/health")
            rec.wait_for_text(r'"status"\s*:\s*"UP"', 10)
            rec.chapter(10, "Actuator metrics")
            rec.move_to("pre", settle=3.6)

            rec.hide_chapter()
            rec.mark("outro")
            rec.outro(
                title="What you just saw",
                features=[
                    "Live view of an LSM tree",
                    "Flushes and leveled compaction",
                    "Bloom filters and block cache",
                    "REST API with OpenAPI docs",
                    "Sorted range scans",
                    "kill -9 recovery, nothing lost",
                    "C++17 engine via Java FFM",
                    "Crash- and power-cut-tested",
                ],
                foot="<strong>github.com/Eyeroniq/lsm-tree</strong> · C++17 · Java 25 · Spring Boot · Docker",
            )
            rec.pause(11.0)
        for scene, proof in proofs.items():
            print(f"proof {scene}: {proof}", flush=True)
    finally:
        wsl("pkill -f '[l]sm-tree-server-0.1.0.jar'; true")
        server.terminate()


if __name__ == "__main__":
    main()
