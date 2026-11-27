"""Make the mp4adts test fixtures: short synthetic tones (no real audio) as MP4/M4A in the layouts YouTube serves,
plus the ADTS the Python agent's Mp4ToAdts makes from each (the reference the Rust port must match byte for byte).

    py -3.14 fixtures/make_fixtures.py        (from pc-agent-rs/; needs ffmpeg with the aac encoder)

Run again only if a fixture is added; the outputs are committed.
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent.parent / "pc-agent"))
from meowbox_agent.mp4adts import Mp4ToAdts  # noqa: E402

FF = shutil.which("ffmpeg") or str(Path.home() / "AppData/Local/MeowBox/bin/ffmpeg/ffmpeg.exe")
TONE = ["-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-f", "lavfi", "-i", "sine=frequency=660:duration=2"]

CASES = {
    # plain MP4, moov before mdat (what -movflags +faststart writes)
    "plain_stereo_44k": [*TONE, "-filter_complex", "[0][1]amerge=inputs=2", "-ar", "44100", "-ac", "2",
                         "-movflags", "+faststart"],
    # fragmented like YouTube's DASH audio (ftyp, moov with mvex/trex, moof+mdat ...)
    "frag_stereo_44k": [*TONE, "-filter_complex", "[0][1]amerge=inputs=2", "-ar", "44100", "-ac", "2",
                        "-movflags", "frag_keyframe+empty_moov+default_base_moof", "-frag_duration", "400000"],
    "frag_mono_48k": [*TONE[:4], "-ar", "48000", "-ac", "1",
                      "-movflags", "frag_keyframe+empty_moov+default_base_moof", "-frag_duration", "300000"],
}

for name, args in CASES.items():
    m4a = HERE / f"{name}.m4a"
    subprocess.run([FF, "-y", "-hide_banner", "-loglevel", "error", *args, "-c:a", "aac", "-b:a", "32k", "-f", "mp4",
                    str(m4a)], check=True)
    conv = Mp4ToAdts()
    adts = b"".join(conv.feed(m4a.read_bytes()))
    (HERE / f"{name}.adts").write_bytes(adts)
    print(f"{name}: {m4a.stat().st_size} B mp4 -> {len(adts)} B adts")
