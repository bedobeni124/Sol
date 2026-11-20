"""A private stdin/stdout pipe keeps one offline recognizer warm inside Sol."""
import json
import os
from pathlib import Path
import sys


def emit(**message):
    print(json.dumps(message, ensure_ascii=False), flush=True)


def preprocess(audio, whisper_mode):
    import numpy as np

    if audio.size == 0:
        return audio
    # Remove DC offset cleanly without adding external DSP dependencies.
    audio = audio - float(np.mean(audio))
    peak = float(np.max(np.abs(audio)))
    if peak < 1e-5:
        return audio
    rms = float(np.sqrt(np.mean(audio * audio)))
    if whisper_mode:
        # Boost whispered speech smoothly into Whisper's optimal loudness range with soft limiting.
        gain = min(22.0, 0.11 / max(rms, 1e-4))
        boosted = audio * gain
        if float(np.max(np.abs(boosted))) > 0.85:
            boosted = np.tanh(boosted * 1.15) / 1.15
        return boosted.astype(np.float32, copy=False)
    if peak < 0.18:
        # Gently lift weak laptop microphones without clipping normal voices.
        gain = min(4.5, 0.45 / peak)
        return (audio * gain).astype(np.float32, copy=False)
    return audio


def run():
    root = Path(sys.argv[1])
    config = json.loads((root / "engine.json").read_text(encoding="utf-8"))
    device = config["device"]
    if device == "cuda":
        # The dynamic linker reads this path at process startup. Re-exec once
        # before loading CTranslate2, keeping the app's own environment intact.
        import sysconfig
        site = Path(sysconfig.get_path("purelib"))
        dirs = [str(site / "nvidia" / lib / "lib") for lib in ("cublas", "cudnn")]
        current = os.environ.get("LD_LIBRARY_PATH", "").split(os.pathsep)
        if not all(d in current for d in dirs):
            os.environ["LD_LIBRARY_PATH"] = os.pathsep.join(dirs + [p for p in current if p])
            # sys.orig_argv includes the source supplied by Qt with python -c.
            os.execv(sys.executable, [sys.executable, *sys.orig_argv[1:]])

    import numpy as np
    from faster_whisper import WhisperModel
    from faster_whisper.vad import get_vad_model

    def load_model(target):
        return WhisperModel(
            str(root / "models" / config["model"]),
            device=target,
            compute_type="float16" if target == "cuda" else "int8",
            cpu_threads=min(8, max(2, os.cpu_count() or 2)),
            local_files_only=True,
        )

    try:
        model = load_model(device)
    except RuntimeError:
        if device != "cuda":
            raise
        device = "cpu"
        model = load_model(device)
    get_vad_model()
    emit(ready=True, model=config["model"], device=device)

    stream = sys.stdin.buffer
    for line in iter(stream.readline, b""):
        job = json.loads(line)
        partial = bool(job.get("partial", False))
        whisper_mode = bool(job.get("whisper", False))
        preferred = [c.strip().lower() for c in job.get("languages", []) if c and c.strip()]
        req_lang = (job.get("language") or "").strip().lower()
        if not req_lang and len(preferred) == 1:
            req_lang = preferred[0]
        try:
            if job.get("bytes") == 0:
                emit(text="", language="", partial=partial)
                continue
            raw_peak = 0.0
            duration_s = 0.0
            if "bytes" in job:
                # One frame contains an even number of signed 16-bit LE samples.
                pcm = stream.read(job["bytes"])
                if len(pcm) != job["bytes"]:
                    break
                audio = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
                duration_s = audio.size / 16000.0
                if audio.size:
                    raw_peak = float(np.max(np.abs(audio)))
                audio = preprocess(audio, whisper_mode)
            else:
                audio = job["path"]

            vad_params = (
                {"min_speech_duration_ms": 80, "min_silence_duration_ms": 350,
                 "speech_pad_ms": 320, "threshold": 0.15}
                if whisper_mode
                else {"min_speech_duration_ms": 100, "min_silence_duration_ms": 350,
                      "speech_pad_ms": 250, "threshold": 0.35}
            )
            no_speech = 0.78 if whisper_mode else 0.6

            def decode(lang_code, use_vad):
                segs, inf = model.transcribe(
                    audio,
                    task="transcribe",
                    language=lang_code or None,
                    initial_prompt=job.get("prompt") or None,
                    beam_size=1,
                    best_of=1,
                    temperature=0,
                    condition_on_previous_text=False,
                    no_speech_threshold=no_speech,
                    vad_filter=use_vad,
                    vad_parameters=vad_params if use_vad else None,
                )
                return "".join(s.text for s in segs).strip(), inf

            text, info = decode(req_lang, True)

            # Avoid losing very short single-word utterances ("yes", "ok", "thanks") to VAD.
            if not text and not partial and isinstance(audio, np.ndarray) and duration_s <= 4.5 and raw_peak > 0.004:
                text, info = decode(req_lang, False)

            # Constrain auto-detection to the user's preferred languages when confidence is not overwhelming.
            if text and not req_lang and len(preferred) > 1 and info.language not in preferred:
                if getattr(info, "language_probability", 0.0) < 0.88:
                    fallback = preferred[0]
                    probs = getattr(info, "all_language_probs", None)
                    if probs:
                        ranked = [(p, code) for code, p in probs if code in preferred]
                        if ranked:
                            fallback = max(ranked)[1]
                    text, info = decode(fallback, True)

            emit(text=text, language=info.language or req_lang, partial=partial)
        except Exception as error:
            emit(error=str(error), partial=partial)


if __name__ == "__main__":
    try:
        run()
    except Exception as error:
        print(str(error), file=sys.stderr, flush=True)
        sys.exit(1)
