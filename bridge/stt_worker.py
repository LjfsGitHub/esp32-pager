"""Keep one faster-whisper model loaded. One wav path per stdin line.

Prints READY once, then OK<TAB>text, EMPTY, or ERR<TAB>message.
"""
from __future__ import annotations

import os
import sys
import wave
from pathlib import Path

# The CUDA wheel looks for cuBLAS/cuDNN beside the NVIDIA pip packages.
_NVIDIA = Path(sys.prefix) / "Lib" / "site-packages" / "nvidia"
for _bin in _NVIDIA.glob("*/bin"):
    if _bin.is_dir():
        os.add_dll_directory(str(_bin))
        os.environ["PATH"] = str(_bin) + os.pathsep + os.environ.get("PATH", "")

import numpy as np
from faster_whisper import WhisperModel

_MODEL = Path(__file__).with_name("models") / "faster-whisper-medium"
_HALLUCINATIONS = (
    "谢谢观看",
    "感谢观看",
    "感谢收听",
    "字幕",
    "订阅",
    "点赞",
    "下期再见",
    "请不吝",
    "明镜与点点",
)


def _warmup(model: WhisperModel) -> None:
    silence = np.zeros(16000, dtype=np.float32)
    segments, _info = model.transcribe(
        silence, language="zh", beam_size=1, vad_filter=False, temperature=0.0
    )
    for _seg in segments:
        break


def _load() -> WhisperModel:
    name = str(_MODEL) if (_MODEL / "model.bin").exists() else "medium"
    try:
        model = WhisperModel(name, device="cuda", compute_type="float16")
        _warmup(model)
        return model
    except Exception as exc:
        print(f"cuda unavailable: {exc}", file=sys.stderr, flush=True)
        model = WhisperModel(name, device="cpu", compute_type="int8")
        _warmup(model)
        return model


def _pcm(path: str) -> np.ndarray:
    """16 kHz float audio. The installed PyAV cannot open our wavs."""
    with wave.open(path, "rb") as src:
        rate = src.getframerate()
        channels = src.getnchannels()
        width = src.getsampwidth()
        frames = src.readframes(src.getnframes())
    if width != 2:
        raise ValueError(f"need 16-bit wav, got {width * 8}")
    audio = np.frombuffer(frames, dtype=np.int16).astype(np.float32)
    if channels > 1:
        audio = audio.reshape(-1, channels).mean(axis=1)
    audio /= 32768.0
    if rate != 16000 and rate > 0 and audio.size > 1:
        out_n = max(1, int(audio.size * 16000 / rate))
        src_x = np.linspace(0.0, 1.0, audio.size, endpoint=False)
        dst_x = np.linspace(0.0, 1.0, out_n, endpoint=False)
        audio = np.interp(dst_x, src_x, audio).astype(np.float32)
    return np.ascontiguousarray(audio, dtype=np.float32)


def _text(model: WhisperModel, path: str) -> str:
    segments, _info = model.transcribe(
        _pcm(path),
        language="zh",
        task="transcribe",
        beam_size=1,
        vad_filter=False,
        condition_on_previous_text=False,
        temperature=0.0,
        no_speech_threshold=0.6,
        compression_ratio_threshold=2.4,
        log_prob_threshold=-1.0,
        initial_prompt="普通话短指令。",
        hotwords="NAS Wi-Fi USB ESP32 HDMI BLE Pager Hermes",
    )
    parts: list[str] = []
    for seg in segments:
        if seg.no_speech_prob is not None and seg.no_speech_prob > 0.6:
            continue
        if seg.avg_logprob is not None and seg.avg_logprob < -1.0:
            continue
        piece = (seg.text or "").strip()
        if piece:
            parts.append(piece)
    text = "".join(parts).strip()
    if any(mark in text for mark in _HALLUCINATIONS) and len(text) <= 12:
        return ""
    if text in {"普通话短指令。", "普通话短指令"}:
        return ""
    return text


def main() -> None:
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    model = _load()
    print("READY", flush=True)
    for line in sys.stdin:
        path = line.strip().strip('"')
        if not path:
            continue
        if path == "QUIT":
            break
        try:
            text = _text(model, path).replace("\t", " ").replace("\n", " ")
        except Exception as exc:
            print("ERR\t" + str(exc).replace("\n", " ")[:180], flush=True)
            continue
        if text:
            print("OK\t" + text, flush=True)
        else:
            print("EMPTY", flush=True)


if __name__ == "__main__":
    main()
