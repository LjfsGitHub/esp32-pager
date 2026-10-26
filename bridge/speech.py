"""Turn a 16 kHz PCM clip from the watch into text.

Prefer the local faster-whisper medium model on the GPU. The Windows speech
engine is only used when that model cannot be started.
"""
from __future__ import annotations

import array
import asyncio
import logging
import os
import re
import struct
import shutil
import subprocess
import tempfile
import threading
from pathlib import Path

log = logging.getLogger("pager.speech")

_PS = r'''
param([string]$Wav, [string]$Out)
$ErrorActionPreference = "Stop"
function WriteOut([string]$text) {
    [System.IO.File]::WriteAllText($Out, $text, (New-Object System.Text.UTF8Encoding $false))
}
try {
    Add-Type -AssemblyName System.Speech
    $engine = $null
    try {
        $zh = [System.Globalization.CultureInfo]::GetCultureInfo("zh-CN")
        $engine = New-Object System.Speech.Recognition.SpeechRecognitionEngine($zh)
    } catch {
        $engine = $null
    }
    if (-not $engine) {
        $engine = New-Object System.Speech.Recognition.SpeechRecognitionEngine
    }
    $engine.SetInputToWaveFile($Wav)
    $engine.LoadGrammar((New-Object System.Speech.Recognition.DictationGrammar))
    $engine.InitialSilenceTimeout = [TimeSpan]::FromSeconds(2)
    $engine.BabbleTimeout = [TimeSpan]::FromSeconds(5)
    $engine.EndSilenceTimeout = [TimeSpan]::FromMilliseconds(700)
    $result = $engine.Recognize()
    if ($result -and $result.Text) {
        WriteOut $result.Text
    } else {
        WriteOut ""
    }
} catch {
    WriteOut ("ERR:" + $_.Exception.Message)
}
'''


def _wav(pcm: bytes, rate: int) -> bytes:
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(pcm),
        b"WAVE",
        b"fmt ",
        16,
        1,
        1,
        rate,
        rate * 2,
        2,
        16,
        b"data",
        len(pcm),
    )
    return header + pcm


# 小智同款：微软晓晓神经网络语音，聊天语速略慢一点，小喇叭上不那么冲。
# 整段都念。过长的回复在句号处收到约两千字，再按句切成小段合成。
# 网络不通时退回系统慧慧。
_VOICES = {
    "xiaoxiao": "zh-CN-XiaoxiaoNeural",
    "xiaoyi": "zh-CN-XiaoyiNeural",
    "yunxi": "zh-CN-YunxiNeural",
    # 流浪地球 MOSS：edge 没有原声。云健底声清楚，只加一点点机械感。
    "moss": "zh-CN-YunjianNeural",
    "yunyang": "zh-CN-YunjianNeural",
}
_PROSODY = {
    "xiaoxiao": ("-6%", "+2Hz"),
    "xiaoyi": ("-8%", "+0Hz"),
    "yunxi": ("-4%", "+1Hz"),
    "moss": ("-8%", "-4Hz"),
    "yunyang": ("-8%", "-4Hz"),
}


def normalize_voice(name: str | None) -> str:
    key = (name or "").strip().lower()
    if key in ("yunyang", "mos", "摩斯"):
        return "moss"
    return key if key in _VOICES else "xiaoxiao"


def take_sentences(buf: str) -> tuple[list[str], str]:
    """Pull finished sentences out of a growing reply and keep the unfinished tail."""
    if not buf:
        return [], ""
    parts: list[str] = []
    start = 0
    fence = False
    marks = set("。！？；\n")
    i = 0
    while i < len(buf):
        if buf.startswith("```", i):
            fence = not fence
            i += 3
            continue
        if not fence and buf[i] in marks and i + 1 - start >= 2:
            piece = buf[start:i + 1].strip()
            if piece:
                parts.append(piece)
            start = i + 1
        i += 1
    rest = buf[start:]
    if not fence and len(rest) >= 24:
        cut = 24
        for mark in ("，", "、", "：", " "):
            at = rest.rfind(mark)
            if 12 <= at < len(rest):
                cut = at + 1
                break
        piece = rest[:cut].strip()
        if piece:
            parts.append(piece)
        rest = rest[cut:]
    return parts, rest
def _resolve_ffmpeg() -> Path:
    env = os.environ.get("PAGER_FFMPEG", "").strip()
    if env:
        return Path(env)
    found = shutil.which("ffmpeg")
    if found:
        return Path(found)
    return Path("ffmpeg")


_FFMPEG = _resolve_ffmpeg()
_MAX_CHARS = 2000
_PIECE_CHARS = 80


def _clean(text: str) -> str:
    text = re.sub(r"```[\s\S]*?```", " ", text or "")
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = text.replace("**", "").replace("*", "")
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"^\s*[-#>]+", "", text, flags=re.M)
    return re.sub(r"\s+", " ", text).strip()


def speech_segments(text: str) -> list[str]:
    """Cleaned sentences, each short enough for one TTS call."""
    text = _clean(text)
    if not text:
        return []
    if len(text) > _MAX_CHARS:
        window = text[:_MAX_CHARS]
        cut = len(window)
        for mark in ("。", "！", "？", "；", "\n"):
            at = window.rfind(mark)
            if at >= _MAX_CHARS // 2:
                cut = at + 1
                break
        log.info("speak trim %d -> %d chars", len(text), cut)
        text = window[:cut].strip()
    parts: list[str] = []
    buf: list[str] = []
    n = 0
    for ch in text:
        buf.append(ch)
        n += 1
        if ch in "。！？；\n" and n >= 36:
            piece = "".join(buf).strip()
            if piece:
                parts.append(piece)
            buf = []
            n = 0
        elif n >= _PIECE_CHARS:
            piece = "".join(buf).strip()
            if piece:
                parts.append(piece)
            buf = []
            n = 0
    tail = "".join(buf).strip()
    if tail:
        if parts and len(tail) < 12 and len(parts[-1]) + len(tail) <= 110:
            parts[-1] = (parts[-1] + tail).strip()
        else:
            parts.append(tail)
    log.info("speak text %d chars / %d segments", len(text), len(parts))
    return parts


async def _edge_save(text: str, path: Path, voice: str) -> None:
    import edge_tts

    key = normalize_voice(voice)
    chosen = _VOICES.get(key, _VOICES["xiaoxiao"])
    rate, pitch = _PROSODY.get(key, ("-6%", "+2Hz"))
    speaker = edge_tts.Communicate(text, chosen, rate=rate, pitch=pitch)
    await speaker.save(str(path))


def _edge_pcm(text: str, voice: str = "xiaoxiao") -> bytes:
    try:
        import edge_tts  # noqa: F401
    except ImportError:
        return b""
    tag = f"{os.getpid()}-{threading.get_ident()}"
    mp3 = Path(tempfile.gettempdir()) / f"pager-tts-{tag}.mp3"
    pcm_path = Path(tempfile.gettempdir()) / f"pager-tts-{tag}.pcm"
    ffmpeg = _FFMPEG if _FFMPEG.is_file() else Path("ffmpeg")
    try:
        asyncio.run(_edge_save(text, mp3, voice))
        key = normalize_voice(voice)
        cmd = [
            str(ffmpeg), "-y", "-loglevel", "error",
            "-i", str(mp3),
            "-ac", "1", "-ar", "16000",
        ]
        if key == "moss":
            # 字要清楚：不降采样、不砍高频。只抬一点齿音，加极短金属反射。
            cmd += [
                "-af",
                "highpass=f=80,equalizer=f=3200:t=h:w=1.3:g=3,"
                "acompressor=threshold=-20dB:ratio=2:attack=8:release=80,"
                "aecho=0.85:0.75:7:0.08",
            ]
        cmd += ["-f", "s16le", "-acodec", "pcm_s16le", str(pcm_path)]
        proc = subprocess.run(
            cmd,
            capture_output=True,
            timeout=40,
        )
        if proc.returncode != 0 or not pcm_path.is_file():
            err = (proc.stderr or b"").decode("utf-8", "replace")[-200:]
            log.warning("neural tts decode %s %s", proc.returncode, err)
            return b""
        return pcm_path.read_bytes()
    except Exception:
        log.exception("neural tts failed")
        return b""


def _sapi_pcm(text: str, male: bool = False) -> bytes:
    try:
        import comtypes.client
    except ImportError:
        log.warning("comtypes missing, no tts")
        return b""
    try:
        fmt = comtypes.client.CreateObject("SAPI.SpAudioFormat")
        fmt.Type = 18  # 16 kHz, 16-bit, mono
        stream = comtypes.client.CreateObject("SAPI.SpMemoryStream")
        stream.Format = fmt
        voice = comtypes.client.CreateObject("SAPI.SpVoice")
        want = "Kangkang" if male else "Huihui"
        for token in voice.GetVoices():
            if want in str(token.GetDescription()):
                voice.Voice = token
                break
        voice.AudioOutputStream = stream
        voice.Speak(text)
        return bytes(stream.GetData())
    except Exception:
        log.exception("tts failed")
        return b""


def _shape(pcm: bytes, *, fade_in: bool = True, fade_out: bool = True) -> bytes:
    """Gentle level and a short fade so the little speaker doesn't click."""
    n = len(pcm) // 2
    if n < 160:
        return b""
    cap = 16000 * 40
    if n > cap:
        n = cap
    buf = array.array("h")
    buf.frombytes(pcm[: n * 2])
    n = len(buf)
    peak = max((abs(sample) for sample in buf), default=1)
    if peak < 22000:
        gain = min(2.0, 22000 / peak)
        for i, sample in enumerate(buf):
            shaped = int(sample * gain)
            if shaped > 32767:
                shaped = 32767
            elif shaped < -32767:
                shaped = -32767
            buf[i] = shaped
    fade = min(160, n // 4)
    for i in range(fade):
        gain = i / fade
        if fade_in:
            buf[i] = int(buf[i] * gain)
        if fade_out:
            buf[-1 - i] = int(buf[-1 - i] * gain)
    return buf.tobytes()


def synthesize_segment(
    text: str,
    *,
    fade_in: bool = False,
    fade_out: bool = False,
    voice: str = "xiaoxiao",
) -> bytes:
    """16 kHz mono PCM for one sentence. Neural voice first."""
    text = (text or "").strip()
    if not text:
        return b""
    voice = normalize_voice(voice)
    male = voice.startswith("yun") or voice == "moss"
    pcm = _edge_pcm(text, voice)
    kind = voice
    if len(pcm) < 1600:
        pcm = _sapi_pcm(text, male=male)
        kind = "sapi"
    pcm = _shape(pcm, fade_in=fade_in, fade_out=fade_out)
    if len(pcm) < 1600:
        return b""
    log.info("tts %s %d ms", kind, len(pcm) // 32)
    return pcm


def synthesize_pcm(text: str) -> bytes:
    """16 kHz mono PCM for the whole utterance."""
    parts = speech_segments(text)
    out = bytearray()
    last = len(parts) - 1
    for i, part in enumerate(parts):
        out.extend(synthesize_segment(part, fade_in=(i == 0), fade_out=(i == last)))
    return bytes(out)


_STT_PY = Path(r"H:\Projects\local-stt\.venv\Scripts\python.exe")
_STT_WORKER = Path(__file__).with_name("stt_worker.py")
_stt_lock = threading.Lock()
_stt_proc: subprocess.Popen[str] | None = None


def _lift(pcm: bytes) -> bytes:
    """Bring a quiet clip up toward a comfortable peak. Leave loud clips alone."""
    n = len(pcm) & ~1
    if n < 4:
        return pcm
    buf = array.array("h")
    buf.frombytes(pcm[:n])
    peak = 0
    for sample in buf:
        level = sample if sample >= 0 else -sample
        if level > peak:
            peak = level
    if peak < 600 or peak >= 10000:
        return pcm
    gain = min(4.0, 14000 / peak)
    for i, sample in enumerate(buf):
        value = int(sample * gain)
        if value > 32767:
            value = 32767
        elif value < -32768:
            value = -32768
        buf[i] = value
    return buf.tobytes()


def _stop_stt() -> None:
    global _stt_proc
    proc = _stt_proc
    _stt_proc = None
    if proc is None or proc.poll() is not None:
        return
    try:
        if proc.stdin:
            proc.stdin.write("QUIT\n")
            proc.stdin.flush()
        proc.wait(timeout=2)
    except (OSError, subprocess.TimeoutExpired):
        proc.kill()


def _ensure_stt() -> subprocess.Popen[str] | None:
    global _stt_proc
    if _stt_proc is not None and _stt_proc.poll() is None:
        return _stt_proc
    if not _STT_PY.exists() or not _STT_WORKER.exists():
        return None
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    env["HF_HUB_DISABLE_XET"] = "1"
    proc = subprocess.Popen(
        [str(_STT_PY), "-u", str(_STT_WORKER)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    ready = ""
    if proc.stdout:
        ready = proc.stdout.readline().strip()
    if ready != "READY" or proc.poll() is not None:
        proc.kill()
        log.warning("whisper worker did not start")
        return None
    _stt_proc = proc
    log.info("whisper ready")
    return proc


def _whisper(wav_path: Path) -> tuple[str, str | None] | None:
    """Return text, or None when the local model is unavailable."""
    with _stt_lock:
        proc = _ensure_stt()
        if proc is None or proc.stdin is None or proc.stdout is None:
            return None
        try:
            proc.stdin.write(str(wav_path) + "\n")
            proc.stdin.flush()
            box: list[str] = []

            def _read() -> None:
                if proc.stdout is not None:
                    box.append(proc.stdout.readline())

            reader = threading.Thread(target=_read, daemon=True)
            reader.start()
            reader.join(12)
            if reader.is_alive():
                log.warning("whisper timeout")
                _stop_stt()
                return None
            line = (box[0] if box else "").strip()
        except OSError as exc:
            log.warning("whisper io: %s", exc)
            _stop_stt()
            return None
    if proc.poll() is not None:
        _stop_stt()
        return None
    if line.startswith("OK\t"):
        text = " ".join(line[3:].split())
        if text:
            log.info("whisper %s", text[:80])
            return text, None
        return "", "没听清"
    if line == "EMPTY":
        return "", "没听清"
    if line.startswith("ERR\t"):
        log.warning("whisper: %s", line[4:])
        return "", "没听清"
    log.warning("whisper reply %s", line[:80])
    return "", "没听清"


def transcribe_pcm(pcm: bytes, rate: int = 16000) -> tuple[str, str | None]:
    if rate <= 0:
        rate = 16000
    if len(pcm) < rate // 2:
        return "", "按短了"
    pcm = _lift(pcm)
    wav_path = Path(tempfile.gettempdir()) / "pager-ptt.wav"
    out_path = Path(tempfile.gettempdir()) / "pager-ptt.txt"
    script_path = Path(tempfile.gettempdir()) / "pager-ptt.ps1"
    try:
        wav_path.write_bytes(_wav(pcm, rate))
    except OSError as exc:
        log.warning("speech wav failed: %s", exc)
        return "", "没听清"
    heard = _whisper(wav_path)
    if heard is not None:
        return heard
    try:
        script_path.write_text(_PS, encoding="utf-8")
        if out_path.exists():
            out_path.unlink()
        proc = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script_path),
                str(wav_path),
                str(out_path),
            ],
            capture_output=True,
            timeout=18,
        )
    except subprocess.TimeoutExpired:
        log.warning("speech timeout")
        return "", "没听清"
    except OSError as exc:
        log.warning("speech start failed: %s", exc)
        return "", "没听清"
    text = ""
    if out_path.exists():
        text = out_path.read_text(encoding="utf-8", errors="replace").strip()
    if text.startswith("ERR:"):
        detail = text[4:].strip()
        log.warning("speech engine: %s", detail)
        low = detail.lower()
        if "culture" in low or "recognizer" in low or "grammar" in low:
            return "", "电脑没有中文语音识别"
        return "", "没听清"
    if proc.returncode != 0 and not text:
        err = (proc.stderr or b"").decode("utf-8", errors="replace")[-300:]
        log.warning("speech exit %s %s", proc.returncode, err)
        return "", "没听清"
    text = " ".join(text.split())
    if not text:
        return "", "没听清"
    log.info("heard %s", text[:80])
    return text, None


_QUESTION = re.compile(
    r"(吗|呢|怎么样|什么样|怎么|为什么|多少|哪里|哪儿|谁|是不是|对不对|好不好|行不行)"
)


def polish_utterance(text: str) -> str:
    """Tidy a transcript: fillers, stutters, written numbers, and known technical terms."""
    raw = (text or "").strip()
    if not raw:
        return ""
    text = re.sub(r"\s+", " ", raw)
    text = re.sub(r"(?<=[\u4e00-\u9fff])\s+(?=[\u4e00-\u9fff])", "", text)
    text = re.sub(r"(?<=[\u4e00-\u9fff]),\s*", "，", text)
    text = re.sub(r"(?<=[\u4e00-\u9fff]);\s*", "；", text)
    text = re.sub(r"(?<=[\u4e00-\u9fff])\?(?=\s|$)", "？", text)
    text = re.sub(r"(?<=[\u4e00-\u9fff])!(?=\s|$)", "！", text)
    text = re.sub(r"(?<=[\u4e00-\u9fff])\.(?=\s|[\u4e00-\u9fff]|$)", "。", text)
    text = re.sub(r"([\u4e00-\u9fff])\1{2,}", r"\1\1", text)
    text = re.sub(r"[，,]{2,}", "，", text)
    text = re.sub(r"[。.]{2,}", "。", text)
    text = re.sub(r"^[，,。！？\s]+", "", text)
    previous = None
    while previous != text:
        previous = text
        text = re.sub(r"^(?:嗯|啊|呃|额|唉|哎|喔|哦|噢|唔)+[，,。\s]*", "", text)
        text = re.sub(r"^(?:那个|就是|然后)[，,\s]+", "", text)
        text = re.sub(r"^我就是", "我", text)
        text = re.sub(r"(?<=[。！？])(?:嗯|啊|呃|额)+[，,\s]*", "", text)
    text = text.strip(" ，,")
    if text in {"我", "你", "他", "她", "它"}:
        return ""
    if not text:
        return ""
    text = _itn_normalize(text)
    text = _apply_terms(text)
    text = _close_sentence(text)
    if text != raw:
        log.info("optimize %s -> %s", raw[:60], text[:60])
    return text


# Spoken form -> written form. Longer patterns first. 纳斯达克 stays intact.
_TERMS = (
    (re.compile(r"优艾斯比"), "USB"),
    (re.compile(r"优盘"), "U盘"),
    (re.compile(r"怀费|歪费"), "Wi-Fi"),
    (re.compile(r"纳斯(?!达克)"), "NAS"),
    (re.compile(r"(?i)(?<![A-Za-z])wifi(?![A-Za-z])"), "Wi-Fi"),
    (re.compile(r"(?i)(?<![A-Za-z])usb(?![A-Za-z])"), "USB"),
)

_itn = None
_itn_failed = False


def _itn_normalize(text: str) -> str:
    """Spoken numbers and amounts into written form. Windows build of WeTextProcessing."""
    global _itn, _itn_failed
    if _itn_failed:
        return text
    if _itn is None:
        try:
            from wetext import Normalizer

            _itn = Normalizer(lang="zh", operator="itn", enable_0_to_9=False)
        except Exception:
            log.warning("itn unavailable", exc_info=True)
            _itn_failed = True
            return text
    try:
        return _itn.normalize(text) or text
    except Exception:
        log.exception("itn failed")
        return text


def _apply_terms(text: str) -> str:
    for pattern, written in _TERMS:
        text = pattern.sub(written, text)
    return text


def _close_sentence(text: str) -> str:
    if len(re.findall(r"[\u4e00-\u9fff]", text)) < 2 and not re.search(r"\d", text):
        return text
    if _QUESTION.search(text[-12:]):
        if text.endswith(("。", ".")):
            return text[:-1] + "？"
        if text[-1] not in "？?！!":
            return text + "？"
        return text
    if text[-1] not in "。！？!?.%":
        return text + "。"
    return text


def _prewarm() -> None:
    try:
        _ensure_stt()
    except Exception:
        log.exception("whisper prewarm failed")


threading.Thread(target=_prewarm, name="whisper-prewarm", daemon=True).start()
