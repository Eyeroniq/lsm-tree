"""Playwright helpers for recording a demo of the real running app.

Runs on the machine with Chrome (channel="chrome"), records a 1280x720 video
and logs scene markers and cut ranges, in seconds since the recording
started, to marks.json next to it.
"""

from __future__ import annotations

import json
import random
import re
import shutil
import time
from pathlib import Path

from playwright.sync_api import Page, sync_playwright

KIT = Path(__file__).resolve().parent
VIEWPORT = {"width": 1280, "height": 720}


class Recorder:
    def __init__(self, out_dir: Path):
        self.out_dir = out_dir
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self.raw_dir = out_dir / "raw"
        shutil.rmtree(self.raw_dir, ignore_errors=True)
        self.scenes: list[list] = []
        self.cuts: list[list[float]] = []
        self._cut_open: float | None = None
        self.mouse = (VIEWPORT["width"] / 2, VIEWPORT["height"] / 2)

    # -- lifecycle ----------------------------------------------------------

    def __enter__(self) -> "Recorder":
        self._playwright = sync_playwright().start()
        self.browser = self._playwright.chromium.launch(
            channel="chrome", headless=False,
            args=["--window-size=1300,820", "--hide-scrollbars"],
        )
        self.context = self.browser.new_context(
            viewport=VIEWPORT, device_scale_factor=1,
            record_video_dir=str(self.raw_dir), record_video_size=VIEWPORT,
        )
        self.context.add_init_script(path=str(KIT / "overlay.js"))
        self.page: Page = self.context.new_page()
        self.t0 = time.monotonic()
        return self

    def __exit__(self, *exc) -> None:
        end = self.now()
        if self._cut_open is not None:
            self.cut_end()
        # The path must be read while Playwright is running; the file is
        # complete once the context closes.
        video_path = self.page.video.path() if self.page.video else None
        (self.out_dir / "marks.json").write_text(json.dumps(
            {"scenes": self.scenes, "cuts": self.cuts, "end": end}, indent=2))
        self.context.close()
        self.browser.close()
        self._playwright.stop()
        if video_path is not None:
            shutil.copyfile(video_path, self.out_dir / "recording.webm")

    def now(self) -> float:
        return round(time.monotonic() - self.t0, 3)

    # -- markers ------------------------------------------------------------

    def mark(self, name: str) -> None:
        """Starts a scene; names match narration.json."""
        self.scenes.append([name, self.now()])
        print(f"[{self.now():7.2f}] scene {name}", flush=True)

    def cut_start(self) -> None:
        if self._cut_open is None:
            self._cut_open = self.now()

    def cut_end(self) -> None:
        if self._cut_open is not None:
            start, end = self._cut_open, self.now()
            self._cut_open = None
            if end - start > 0.05:
                self.cuts.append([start, end])

    def drop_since(self, start: float) -> None:
        """Cuts everything from `start` to now (a failed attempt)."""
        self.cuts.append([start, self.now()])

    # -- overlays -----------------------------------------------------------

    def chapter(self, number: int, text: str) -> None:
        self.page.evaluate("([n, t]) => window.__demo.chapter(n, t)", [number, text])

    def hide_chapter(self) -> None:
        self.page.evaluate("() => window.__demo.hideChapter()")

    def intro(self, **card) -> None:
        self.page.evaluate("(c) => window.__demo.intro(c)", card)

    def outro(self, **card) -> None:
        self.page.evaluate("(c) => window.__demo.outro(c)", card)

    def hide_card(self) -> None:
        self.page.evaluate("() => window.__demo.hideCard()")

    # -- human-like input ---------------------------------------------------

    def move_to(self, selector: str, settle: float = 0.25) -> None:
        box = self.page.locator(selector).first.bounding_box()
        if box is None:
            raise RuntimeError(f"not visible: {selector}")
        x = box["x"] + box["width"] / 2 + random.uniform(-3, 3)
        y = box["y"] + box["height"] / 2 + random.uniform(-2, 2)
        steps = max(12, int(((x - self.mouse[0]) ** 2 + (y - self.mouse[1]) ** 2) ** 0.5 / 18))
        self.page.mouse.move(x, y, steps=steps)
        self.mouse = (x, y)
        time.sleep(settle)

    def click(self, selector: str, settle: float = 0.25) -> None:
        self.move_to(selector, settle)
        self.page.mouse.down()
        time.sleep(0.07)
        self.page.mouse.up()

    def type(self, selector: str, text: str, delay_ms: int = 55) -> None:
        self.click(selector, settle=0.15)
        self.page.keyboard.type(text, delay=delay_ms)

    def pause(self, seconds: float) -> None:
        time.sleep(seconds)

    # -- verification -------------------------------------------------------

    def wait_for_text(self, pattern: str, timeout: float, scope: str = "body") -> str:
        """Waits until the page text matches `pattern`; returns the match."""
        deadline = time.monotonic() + timeout
        regex = re.compile(pattern)
        while time.monotonic() < deadline:
            text = self.page.locator(scope).first.inner_text()
            match = regex.search(text)
            if match:
                return match.group(0)
            time.sleep(0.2)
        raise TimeoutError(f"no match for {pattern!r} within {timeout}s")
