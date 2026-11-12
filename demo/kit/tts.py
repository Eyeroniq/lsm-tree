"""Narration: one neural-voice line per scene, with word timings.

    python tts.py narration.json OUT_DIR

Writes OUT_DIR/<scene>.mp3 and OUT_DIR/<scene>.words.json, a list of
[display_word, start_s, end_s] in the order of the narration text. Words are
spoken with a few pronunciation fixes, but timed and shown as written.
"""

from __future__ import annotations

import asyncio
import json
import re
import sys
from pathlib import Path

import edge_tts

VOICE = "en-US-AndrewNeural"
RATE = "+8%"
# How a written word should be spoken (matched without punctuation).
SAY = {
    "lsm-tree": "L S M tree",
    "lsm-tree,": "L S M tree",
    "SIGKILL": "sig kill",
    "fsync": "F sync",
    "fsynced": "F synced",
    "ThreadSanitizer": "Thread Sanitizer",
    "MANIFEST": "manifest",
    "OpenAPI": "Open A P I",
    "REST": "rest",
    "C++17,": "C plus plus seventeen,",
    "C++17": "C plus plus seventeen",
    "C++.": "C plus plus.",
    "C++,": "C plus plus,",
}


def normalize(text: str) -> str:
    return re.sub(r"[^0-9a-z]", "", text.lower())


def spoken_form(word: str) -> str:
    if word in SAY:
        return SAY[word]
    core = word.rstrip(".,;:!?")
    tail = word[len(core):]
    return SAY[core] + tail if core in SAY else word


async def synthesize(text: str, mp3: Path) -> list[tuple[str, float, float]]:
    # The free service occasionally returns nothing; retry with backoff.
    for attempt in range(5):
        try:
            communicate = edge_tts.Communicate(text, VOICE, rate=RATE, boundary="WordBoundary")
            boundaries = []
            with mp3.open("wb") as out:
                async for chunk in communicate.stream():
                    if chunk["type"] == "audio":
                        out.write(chunk["data"])
                    elif chunk["type"] == "WordBoundary":
                        start = chunk["offset"] / 1e7
                        boundaries.append((chunk["text"], start, start + chunk["duration"] / 1e7))
            return boundaries
        except (edge_tts.exceptions.NoAudioReceived, OSError) as error:
            if attempt == 4:
                raise
            wait = 2 * (attempt + 1)
            print(f"  retrying in {wait}s after: {error}", flush=True)
            await asyncio.sleep(wait)
    raise AssertionError("unreachable")


def align(display: list[str], boundaries) -> list[list]:
    """Times each display word from the boundaries of its spoken words."""
    spoken = [spoken_form(word) for word in display]
    # Character span of every display word in the normalized spoken text.
    spans = []
    joined = ""
    for words in spoken:
        start = len(joined)
        joined += normalize(words)
        spans.append((start, len(joined)))
    cursor = 0
    timed: list[tuple[int, int, float, float]] = []
    for text, start, end in boundaries:
        token = normalize(text)
        if not token:
            continue
        at = joined.find(token, cursor)
        if at < 0:
            continue
        timed.append((at, at + len(token), start, end))
        cursor = at + len(token)
    result = []
    for word, (a, b) in zip(display, spans):
        hits = [(s, e) for (x, y, s, e) in timed if x < b and y > a]
        result.append([word, hits[0][0] if hits else None, hits[-1][1] if hits else None])
    # Words the voice service did not time get times between their neighbours.
    for index, entry in enumerate(result):
        if entry[1] is None:
            before = next((result[j][2] for j in range(index - 1, -1, -1) if result[j][2] is not None), 0.0)
            after = next((result[j][1] for j in range(index + 1, len(result)) if result[j][1] is not None), before + 0.3)
            entry[1], entry[2] = before, max(before + 0.05, after)
    return result


async def main() -> None:
    narration = json.loads(Path(sys.argv[1]).read_text())
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    for scene, text in narration.items():
        display = text.split()
        spoken_text = " ".join(spoken_form(word) for word in display)
        cache = out_dir / f"{scene}.txt"
        if (cache.exists() and cache.read_text() == f"{VOICE}|{RATE}|{spoken_text}"
                and (out_dir / f"{scene}.mp3").exists() and (out_dir / f"{scene}.words.json").exists()):
            print(f"{scene}: unchanged", flush=True)
            continue
        print(f"{scene}...", flush=True)
        boundaries = await synthesize(spoken_text, out_dir / f"{scene}.mp3")
        words = align(display, boundaries)
        (out_dir / f"{scene}.words.json").write_text(json.dumps(words, indent=1))
        cache.write_text(f"{VOICE}|{RATE}|{spoken_text}")
        print(f"  {len(display)} words, {len(boundaries)} boundaries, "
              f"ends at {words[-1][2]:.2f}s", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
