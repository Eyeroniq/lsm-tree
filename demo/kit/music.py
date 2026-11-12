"""Original background music, synthesized from scratch (no samples).

    python music.py SECONDS OUT.wav [seed]

Upbeat electronic at 124 BPM: four-on-the-floor kick, claps on 2 and 4,
hi-hats, offbeat bass, a plucked 16th-note arpeggio and pads with a sidechain
pump, over an A-minor four-chord loop (Am - F - C - G), with a four-bar intro
build. Soft-limited, normalized, faded in and out.
"""

from __future__ import annotations

import sys
import wave

import numpy as np

SR = 48000
BPM = 124
BEAT = 60 / BPM
BAR = 4 * BEAT
SIXTEENTH = BEAT / 4
# One chord per bar: MIDI notes (root, third, fifth).
CHORDS = [(57, 60, 64), (53, 57, 60), (60, 64, 67), (55, 59, 62)]  # Am F C G
INTRO_BARS = 4


def hz(midi: float) -> float:
    return 440.0 * 2 ** ((midi - 69) / 12)


def additive_saw(freq: float, n: int, harmonics: int, phase: float = 0.0) -> np.ndarray:
    t = np.arange(n) / SR
    out = np.zeros(n)
    for k in range(1, harmonics + 1):
        if k * freq > SR / 3:
            break
        out += np.sin(2 * np.pi * k * freq * t + phase * k) / k
    return out * (2 / np.pi)


def spectral_filter(x: np.ndarray, low: float | None = None, high: float | None = None) -> np.ndarray:
    """Zero-phase band filter with soft edges, applied to the whole signal."""
    spectrum = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(len(x), 1 / SR)
    gain = np.ones_like(freqs)
    if high is not None:  # low-pass
        gain *= 1 / np.sqrt(1 + (freqs / high) ** 8)
    if low is not None:  # high-pass
        gain *= 1 / np.sqrt(1 + (low / np.maximum(freqs, 1e-3)) ** 8)
    return np.fft.irfft(spectrum * gain, n=len(x))


def place(track: np.ndarray, sound: np.ndarray, at: float, gain: float = 1.0) -> None:
    start = int(at * SR)
    if start >= len(track):
        return
    end = min(len(track), start + len(sound))
    track[start:end] += sound[: end - start] * gain


def kick() -> np.ndarray:
    n = int(0.42 * SR)
    t = np.arange(n) / SR
    freq = 46 + 95 * np.exp(-t / 0.035)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    body = np.sin(phase) * np.exp(-t / 0.20)
    click = np.random.default_rng(1).standard_normal(n) * np.exp(-t / 0.003) * 0.25
    return body + click


def clap(rng) -> np.ndarray:
    n = int(0.32 * SR)
    t = np.arange(n) / SR
    noise = rng.standard_normal(n)
    envelope = np.zeros(n)
    for offset in (0.0, 0.011, 0.022):
        envelope += np.where(t >= offset, np.exp(-(t - offset) / (0.009 if offset < 0.02 else 0.14)), 0)
    return spectral_filter(noise * envelope, low=900, high=3200) * 0.8


def hat(rng, decay: float) -> np.ndarray:
    n = int((decay * 6) * SR)
    t = np.arange(n) / SR
    return spectral_filter(rng.standard_normal(n) * np.exp(-t / decay), low=7500) * 0.5


def pluck(freq: float) -> np.ndarray:
    n = int(0.32 * SR)
    t = np.arange(n) / SR
    tone = additive_saw(freq, n, 14) * 0.6 + np.sin(2 * np.pi * freq * t) * 0.4
    return tone * np.exp(-t / 0.085) * (1 - np.exp(-t / 0.002))


def bass_note(freq: float) -> np.ndarray:
    n = int(0.22 * SR)
    t = np.arange(n) / SR
    tone = additive_saw(freq, n, 10) * 0.7 + np.sin(2 * np.pi * freq * t) * 0.5
    return tone * np.exp(-t / 0.11) * (1 - np.exp(-t / 0.004))


def pad_chord(chord, seconds: float) -> tuple[np.ndarray, np.ndarray]:
    n = int(seconds * SR)
    t = np.arange(n) / SR
    left = np.zeros(n)
    right = np.zeros(n)
    for note in chord:
        for octave in (0, 12):
            for detune, side in ((-0.09, left), (0.0, None), (0.09, right)):
                voice = additive_saw(hz(note + octave + detune), n, 9, phase=detune * 3)
                if side is None:
                    left += voice * 0.5
                    right += voice * 0.5
                else:
                    side += voice
    envelope = np.minimum(1, t / 0.25) * np.minimum(1, (seconds - t) / 0.2).clip(0)
    return left * envelope, right * envelope


def render(seconds: float, seed: int = 7) -> np.ndarray:
    rng = np.random.default_rng(seed)
    bars = int(np.ceil(seconds / BAR)) + 1
    n = int(bars * BAR * SR)
    drums = np.zeros(n)
    bass = np.zeros(n)
    arp_left = np.zeros(n)
    arp_right = np.zeros(n)
    pad_left = np.zeros(n)
    pad_right = np.zeros(n)
    pump = np.ones(n)

    kick_sound = kick()
    clap_sound = clap(rng)
    closed = hat(rng, 0.018)
    open_hat = hat(rng, 0.09)
    t_pump = np.arange(int(BEAT * SR)) / SR
    pump_shape = 1 - 0.65 * np.exp(-t_pump / 0.14)

    for bar in range(bars):
        chord = CHORDS[bar % 4]
        bar_start = bar * BAR
        intro = bar < INTRO_BARS
        # Pads with the sidechain pump, every bar.
        left, right = pad_chord(chord, BAR + 0.15)
        place(pad_left, left, bar_start, 0.11)
        place(pad_right, right, bar_start, 0.11)
        for beat in range(4):
            at = bar_start + beat * BEAT
            if not intro:
                place(drums, kick_sound, at, 0.95)
                start = int(at * SR)
                pump[start:start + len(pump_shape)] = pump_shape[: max(0, min(len(pump_shape), n - start))]
                if beat in (1, 3):
                    place(drums, clap_sound, at, 0.55)
                # Offbeat bass and open hat on the "and".
                place(bass, bass_note(hz(chord[0] - 24)), at + BEAT / 2, 0.55)
                place(drums, open_hat, at + BEAT / 2, 0.16)
            elif bar >= 2:
                place(drums, closed, at + BEAT / 2, 0.20)
            # Closed hats on 16ths once the groove is in.
            if not intro:
                for step in range(4):
                    place(drums, closed, at + step * SIXTEENTH, 0.13 if step % 2 else 0.07)
        # Plucked arpeggio across two octaves, 16ths, with a dotted-8th echo.
        pattern = [0, 1, 2, 4, 3, 2, 1, 2, 0, 2, 1, 4, 3, 1, 2, 1]
        notes = [chord[0], chord[1], chord[2], chord[0] + 12, chord[1] + 12]
        for step, index in enumerate(pattern):
            at = bar_start + step * SIXTEENTH
            sound = pluck(hz(notes[index] + 12))
            accent = 0.16 if step % 4 == 0 else 0.11
            place(arp_left, sound, at, accent)
            place(arp_right, sound, at, accent * 0.8)
            place(arp_right, sound, at + 3 * SIXTEENTH, accent * 0.35)
            place(arp_left, sound, at + 6 * SIXTEENTH, accent * 0.2)

    # Intro build: arp and pads open up from dark to bright over four bars.
    intro_samples = int(INTRO_BARS * BAR * SR)
    sweep = np.ones(n)
    sweep[:intro_samples] = np.linspace(0, 1, intro_samples) ** 1.6
    for channel in (arp_left, arp_right):
        dark = spectral_filter(channel, high=900)
        bright = spectral_filter(channel, high=5200)
        channel[:] = dark * (1 - sweep) + bright * sweep
    pad_left = spectral_filter(pad_left, high=1500) * pump
    pad_right = spectral_filter(pad_right, high=1500) * pump
    bass = spectral_filter(bass, high=380) * (0.6 + 0.4 * pump)
    # A soft noise riser into the drop.
    riser_n = int(2 * BAR * SR)
    riser = spectral_filter(rng.standard_normal(riser_n), low=1500, high=7000)
    riser *= np.linspace(0, 1, riser_n) ** 2 * 0.07
    place(drums, riser, (INTRO_BARS - 2) * BAR)

    left = drums + bass + arp_left + pad_left
    right = drums + bass + arp_right + pad_right
    stereo = np.stack([left, right], axis=1)[: int(seconds * SR)]
    # Soft limit, normalize to -1 dBFS, fade in and out.
    stereo = np.tanh(stereo * 1.4) / np.tanh(1.4)
    stereo /= np.max(np.abs(stereo)) / 10 ** (-1 / 20)
    fade_in = int(1.0 * SR)
    fade_out = int(3.0 * SR)
    stereo[:fade_in] *= np.linspace(0, 1, fade_in)[:, None]
    stereo[-fade_out:] *= np.linspace(1, 0, fade_out)[:, None]
    return stereo


def write_wav(path: str, stereo: np.ndarray) -> None:
    data = (np.clip(stereo, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(SR)
        out.writeframes(data.tobytes())


if __name__ == "__main__":
    seconds = float(sys.argv[1])
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 7
    write_wav(sys.argv[2], render(seconds, seed))
    print(f"wrote {sys.argv[2]} ({seconds:.1f}s)")
