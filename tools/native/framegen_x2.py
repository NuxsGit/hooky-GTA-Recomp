#!/usr/bin/env python3
"""Offline x2 motion-compensated interpolation for local, constant-rate captures."""

import argparse
from fractions import Fraction
from pathlib import Path
import subprocess


def command(ffmpeg: str, source: Path, output: Path, fps: Fraction) -> list[str]:
    # Establish the source cadence explicitly; the filter uses future frames and
    # may omit the end of the capture while draining its lookahead.
    target = 2 * fps
    rate = f"{target.numerator}/{target.denominator}"
    filters = (f"settb=expr={fps.denominator}/{fps.numerator},setpts=N,"
               f"minterpolate=fps={rate}:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:"
               "me=epzs:vsbmc=1:scd=fdiff")
    return [ffmpeg, "-nostdin", "-n", "-i", str(source), "-map", "0:v:0", "-an",
            "-vf", filters, "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p", str(output)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="local video capture (constant frame rate)")
    parser.add_argument("output", type=Path, help="new .mp4 file; never overwrites an existing file")
    parser.add_argument("--fps", type=Fraction, default=Fraction(30), help="source FPS, e.g. 30 or 30000/1001")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg with minterpolate and libx264")
    args = parser.parse_args()
    if not args.source.is_file() or args.output.suffix.lower() != ".mp4" or not 0 < args.fps <= 240:
        parser.error("expected an existing source, a .mp4 output and an FPS in (0, 240]")
    try:
        subprocess.run(command(args.ffmpeg, args.source.resolve(), args.output.resolve(), args.fps), check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"x2 interpolation failed: {exc}\n")


if __name__ == "__main__":
    main()
