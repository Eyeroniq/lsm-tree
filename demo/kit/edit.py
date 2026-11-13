"""Turns a recording, its marks and the narration into the finished video.

    python edit.py BUILD_DIR NARRATION_JSON OUT_MP4 [--bg "#0b1020,#1b1240"]

BUILD_DIR holds recording.webm and marks.json from the recorder, and receives
voice/, music.wav and the intermediate files. Needs ffmpeg (with libass),
edge-tts (via tts.py), numpy and Pillow.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

KIT = Path(__file__).resolve().parent
W, H, FPS = 1920, 1080, 30
WIN_W, WIN_H, RADIUS = 1600, 900, 24
WIN_X, WIN_Y = (W - WIN_W) // 2, (H - WIN_H) // 2
FADE = 0.35            # cross-fade between scenes
LEAD_IN = 0.45         # voice starts this long after its scene
TAIL = 1.1             # silence after a line before the next scene
MAX_SPEED = 2.5
FONT_DIR = "/mnt/c/Windows/Fonts"
SUB_Y = WIN_Y + WIN_H + (H - WIN_Y - WIN_H) // 2   # centre of the space under the window


def run(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    result = subprocess.run(args, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-4000:])
        raise RuntimeError(f"command failed: {' '.join(args[:6])} ...")
    return result


def duration(path: Path) -> float:
    out = run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
               "-of", "default=nw=1:nk=1", str(path)]).stdout
    return float(out.strip())


def subtract(start: float, end: float, cuts: list[list[float]]) -> list[tuple[float, float]]:
    keep = [(start, end)]
    for a, b in sorted(cuts):
        next_keep = []
        for s, e in keep:
            if b <= s or a >= e:
                next_keep.append((s, e))
                continue
            if a > s:
                next_keep.append((s, a))
            if b < e:
                next_keep.append((b, e))
        keep = next_keep
    return [(s, e) for s, e in keep if e - s > 0.04]


# -- subtitles ----------------------------------------------------------------

MAX_WORDS, MAX_CHARS = 7, 40


def sentences(words: list[list]) -> list[list[list]]:
    groups, current = [], []
    for word in words:
        current.append(word)
        if re.search(r"[.!?]$", word[0]):
            groups.append(current)
            current = []
    if current:
        groups.append(current)
    return groups


def phrase_split(words: list[list]) -> list[list[list]]:
    """Splits one sentence into balanced phrases within the limits, avoiding
    phrases of one or two words."""
    n = len(words)

    def text(chunk):
        return " ".join(w[0] for w in chunk)

    for minimum in (3, 2, 1):
        for count in range(1, n + 1):
            best = None
            for cuts in itertools.combinations(range(1, n), count - 1):
                bounds = (0, *cuts, n)
                chunks = [words[a:b] for a, b in zip(bounds, bounds[1:])]
                if any(len(c) > MAX_WORDS or len(text(c)) > MAX_CHARS for c in chunks):
                    continue
                if count > 1 and any(len(c) < minimum for c in chunks):
                    continue
                lengths = [len(text(c)) for c in chunks]
                score = max(lengths) - min(lengths)
                if best is None or score < best[0]:
                    best = (score, chunks)
            if best:
                return best[1]
    return [words]


def ass_time(seconds: float) -> str:
    seconds = max(0.0, seconds)
    hours = int(seconds // 3600)
    minutes = int(seconds % 3600 // 60)
    return f"{hours}:{minutes:02d}:{seconds % 60:05.2f}"


def write_subtitles(path: Path, lines: list[tuple[float, list[list]]]) -> None:
    phrases = []
    for offset, words in lines:
        for sentence in sentences(words):
            for chunk in phrase_split(sentence):
                phrases.append([offset + chunk[0][1] - 0.05, offset + chunk[-1][2] + 0.25,
                                " ".join(w[0] for w in chunk)])
    for current, following in zip(phrases, phrases[1:]):
        current[1] = min(current[1], following[0] - 0.02)
    for phrase in phrases:
        phrase[1] = max(phrase[1], phrase[0] + 0.7)
    header = f"""[Script Info]
ScriptType: v4.00+
PlayResX: {W}
PlayResY: {H}
WrapStyle: 2
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Caption,Segoe UI Semibold,38,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,5,0,0,0,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""
    events = []
    for start, end, text in phrases:
        clean = text.replace("{", "(").replace("}", ")")
        # A soft shadow: the same line, blurred and darkened, under the text.
        # No outline and no box, so the caption has no black border.
        events.append(f"Dialogue: 0,{ass_time(start)},{ass_time(end)},Caption,,0,0,0,,"
                      f"{{\\an5\\pos({W // 2 + 1},{SUB_Y + 3})\\1c&H000000&\\1a&H40&\\blur9\\fad(120,120)}}{clean}")
        events.append(f"Dialogue: 1,{ass_time(start)},{ass_time(end)},Caption,,0,0,0,,"
                      f"{{\\an5\\pos({W // 2},{SUB_Y})\\fad(120,120)}}{clean}")
    path.write_text(header + "\n".join(events) + "\n", encoding="utf-8")


# -- framing assets -----------------------------------------------------------

def framing_images(work: Path) -> tuple[Path, Path, tuple[int, int]]:
    mask = Image.new("L", (WIN_W, WIN_H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, WIN_W - 1, WIN_H - 1), RADIUS, fill=255)
    mask_path = work / "mask.png"
    mask.save(mask_path)
    pad = 90
    shadow = Image.new("RGBA", (WIN_W + 2 * pad, WIN_H + 2 * pad), (0, 0, 0, 0))
    alpha = Image.new("L", shadow.size, 0)
    ImageDraw.Draw(alpha).rounded_rectangle((pad, pad, pad + WIN_W, pad + WIN_H), RADIUS + 6, fill=170)
    alpha = alpha.filter(ImageFilter.GaussianBlur(34))
    shadow.putalpha(alpha)
    shadow_path = work / "shadow.png"
    shadow.save(shadow_path)
    return mask_path, shadow_path, (WIN_X - pad, WIN_Y - pad + 16)


# -- main ---------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("narration", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--bg", default="#0b1020,#1b1240")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    build = args.build
    work = build / "work"
    work.mkdir(parents=True, exist_ok=True)
    narration = json.loads(args.narration.read_text())
    marks = json.loads((build / "marks.json").read_text())
    recording = build / "recording.webm"

    print("narration...", flush=True)
    run([sys.executable, str(KIT / "tts.py"), str(args.narration), str(build / "voice")])

    scenes = marks["scenes"]
    names = [name for name, _ in scenes]
    assert names == list(narration), f"scene marks {names} != narration keys {list(narration)}"
    plan = []
    for index, (name, start) in enumerate(scenes):
        end = scenes[index + 1][1] if index + 1 < len(scenes) else marks["end"]
        keep = subtract(start, end, marks["cuts"])
        raw = sum(e - s for s, e in keep)
        voice = duration(build / "voice" / f"{name}.mp3")
        target = voice + LEAD_IN + TAIL
        speed = min(MAX_SPEED, raw / target) if raw > target else 1.0
        length = raw / speed
        hold = max(0.0, target - length)
        plan.append({"name": name, "keep": keep, "raw": raw, "voice": voice,
                     "speed": speed, "hold": hold, "length": length + hold})
        print(f"  {name:11s} raw {raw:6.2f}s voice {voice:5.2f}s speed {speed:4.2f}x "
              f"hold {hold:4.2f}s -> {length + hold:6.2f}s", flush=True)

    print("scenes...", flush=True)
    for index, scene in enumerate(plan):
        parts = []
        for j, (s, e) in enumerate(scene["keep"]):
            parts.append(f"[0:v]trim=start={s:.3f}:end={e:.3f},setpts=PTS-STARTPTS,fps={FPS}[p{j}]")
        joined = "".join(f"[p{j}]" for j in range(len(scene["keep"])))
        graph = ";".join(parts) + (
            f";{joined}concat=n={len(scene['keep'])}:v=1:a=0[c];"
            f"[c]setpts=PTS/{scene['speed']:.5f},fps={FPS},"
            f"tpad=stop_mode=clone:stop_duration={scene['hold'] + 0.5:.3f},"
            f"trim=duration={scene['length']:.3f},setpts=PTS-STARTPTS,format=yuv420p[v]")
        run(["ffmpeg", "-y", "-v", "error", "-i", str(recording), "-filter_complex", graph,
             "-map", "[v]", "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "14",
             "-r", str(FPS), str(work / f"scene{index:02d}.mp4")])
        scene["length"] = duration(work / f"scene{index:02d}.mp4")

    print("cross-fades...", flush=True)
    inputs, graph, label, total = [], [], "0:v", plan[0]["length"]
    starts = [0.0]
    for index in range(len(plan)):
        inputs += ["-i", str(work / f"scene{index:02d}.mp4")]
    for index in range(1, len(plan)):
        offset = total - FADE
        starts.append(offset)
        out_label = f"x{index}"
        graph.append(f"[{label}][{index}:v]xfade=transition=fade:duration={FADE}:offset={offset:.3f}[{out_label}]")
        label = out_label
        total = offset + plan[index]["length"]
    run(["ffmpeg", "-y", "-v", "error", *inputs, "-filter_complex", ";".join(graph),
         "-map", f"[{label}]", "-c:v", "libx264", "-preset", "veryfast", "-crf", "14",
         "-r", str(FPS), str(work / "body.mp4")])
    total = duration(work / "body.mp4")

    print("subtitles and framing...", flush=True)
    lines = []
    for scene, start in zip(plan, starts):
        words = json.loads((build / "voice" / f"{scene['name']}.words.json").read_text())
        lines.append((start + LEAD_IN, words))
    write_subtitles(work / "captions.ass", lines)
    mask, shadow, shadow_at = framing_images(work)
    c0, c1 = args.bg.split(",")
    graph = (
        f"gradients=s={W}x{H}:r={FPS}:c0={c0}:c1={c1}:x0=0:y0=0:x1={W}:y1={H}"
        f":speed=0.012:d={total:.3f}[bg];"
        f"[0:v]scale={WIN_W}:{WIN_H}:flags=lanczos,format=rgba[app];"
        f"[1:v]format=gray[mask];[app][mask]alphamerge[win];"
        f"[bg][2:v]overlay={shadow_at[0]}:{shadow_at[1]}[bgs];"
        f"[bgs][win]overlay={WIN_X}:{WIN_Y}:format=auto,"
        f"ass=filename={work / 'captions.ass'}:fontsdir={FONT_DIR},format=yuv420p[v]"
    )
    run(["ffmpeg", "-y", "-v", "error", "-i", str(work / "body.mp4"), "-i", str(mask),
         "-i", str(shadow), "-filter_complex", graph, "-map", "[v]", "-t", f"{total:.3f}",
         "-c:v", "libx264", "-preset", "medium", "-crf", "17", "-profile:v", "high",
         "-pix_fmt", "yuv420p", "-r", str(FPS), str(work / "framed.mp4")])

    print("voice and music...", flush=True)
    inputs, graph = [], []
    for index, (scene, start) in enumerate(zip(plan, starts)):
        inputs += ["-i", str(build / "voice" / f"{scene['name']}.mp3")]
        delay = int(round((start + LEAD_IN) * 1000))
        graph.append(f"[{index}:a]aresample=48000,aformat=channel_layouts=stereo,adelay={delay}|{delay}[a{index}]")
    graph.append("".join(f"[a{i}]" for i in range(len(plan))) +
                 f"amix=inputs={len(plan)}:normalize=0:duration=longest,apad,atrim=0:{total:.3f}[voice]")
    run(["ffmpeg", "-y", "-v", "error", *inputs, "-filter_complex", ";".join(graph),
         "-map", "[voice]", str(work / "voice.wav")])
    run([sys.executable, str(KIT / "music.py"), f"{total + 1:.2f}", str(work / "music.wav"), str(args.seed)])
    mix = (
        f"[0:a]atrim=0:{total:.3f},volume=0.30,afade=t=in:d=1.5,"
        f"afade=t=out:st={max(0, total - 3):.3f}:d=3[music];"
        f"[1:a]asplit[sidechain][voice];"
        f"[music][sidechain]sidechaincompress=threshold=0.015:ratio=7:attack=12:release=420[ducked];"
        f"[ducked][voice]amix=inputs=2:normalize=0:duration=first[mix]"
    )
    run(["ffmpeg", "-y", "-v", "error", "-i", str(work / "music.wav"), "-i", str(work / "voice.wav"),
         "-filter_complex", mix, "-map", "[mix]", str(work / "mix.wav")])
    measured = run(["ffmpeg", "-hide_banner", "-i", str(work / "mix.wav"), "-af",
                    "loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"]).stderr
    stats = json.loads(measured[measured.rindex("{"):measured.rindex("}") + 1])
    loudnorm = ("loudnorm=I=-16:TP=-1.5:LRA=11:linear=true:"
                f"measured_I={stats['input_i']}:measured_TP={stats['input_tp']}:"
                f"measured_LRA={stats['input_lra']}:measured_thresh={stats['input_thresh']}:"
                f"offset={stats['target_offset']}")
    run(["ffmpeg", "-y", "-v", "error", "-i", str(work / "mix.wav"), "-af", loudnorm + ",aresample=48000",
         "-c:a", "aac", "-b:a", "192k", str(work / "audio.m4a")])

    print("mux...", flush=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    run(["ffmpeg", "-y", "-v", "error", "-i", str(work / "framed.mp4"), "-i", str(work / "audio.m4a"),
         "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "copy", "-shortest",
         "-movflags", "+faststart", str(args.out)])
    final = duration(args.out)
    (build / "plan.json").write_text(json.dumps(
        {"total": final, "starts": starts, "scenes": plan}, indent=1))
    print(f"done: {args.out} ({final:.1f}s)", flush=True)


if __name__ == "__main__":
    main()
