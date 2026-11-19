"""Metadata shared by the desktop packagers. Never point a packager at a player's folder."""
import hashlib
import re
from pathlib import Path

LOADER = Path(__file__).resolve().parents[2] / "loader"


def launcher_version(build_dir):
    version = (LOADER / "VERSION").read_text(encoding="ascii").strip()
    stamp = build_dir / "launcher-version.txt"
    if not re.fullmatch(r"(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}", version):
        raise SystemExit("loader/VERSION must be MAJOR.MINOR.PATCH")
    if not stamp.is_file() or stamp.read_text(encoding="ascii").strip() != version:
        raise SystemExit(f"rebuild the launcher in {build_dir} for version {version} before packaging")
    return version


def prepare_output(path, *, allow_ds_store=False):
    out = Path(path).resolve()
    if out.exists() and any(not allow_ds_store or p.name != ".DS_Store" for p in out.iterdir()):
        raise SystemExit(f"refusing to write to {out}: use a new or empty staging folder")
    out.mkdir(parents=True, exist_ok=True)
    return out


def write_metadata(out, package, platform, version):
    # Version is for update discovery. The build ID is independent of it: updating only the
    # launcher / renderer shouldn't make players recompile. The ABI epoch covers changes that
    # aren't visible in the staged headers, manifests, import lists or reference hashes.
    abi = (LOADER / "GAME_ABI").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[1-9][0-9]*", abi):
        raise SystemExit("loader/GAME_ABI must be a positive integer")
    inputs = package / "tools/game_dll"
    stamp = inputs / "build-id.txt"
    digest = hashlib.sha256(f"gtav-game-build-v2\n{platform}\n{abi}\n".encode("ascii"))
    # A compiler/LLD or builtin header change affects the local library even if generated
    # C++ is the same. Runtime-only files deliberately do not take part in this stamp.
    for folder in (inputs, package / "tools/llvm"):
        for path in sorted(folder.rglob("*"), key=lambda p: p.as_posix()):
            if not path.is_file() or path == stamp:
                continue
            digest.update(path.relative_to(package).as_posix().encode("utf-8") + b"\0")
            content = hashlib.sha256()
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    content.update(block)
            digest.update(content.digest())
    stamp.write_text(digest.hexdigest() + "\n", encoding="ascii")
    (out / "gtav_release.txt").write_text(f"gtav-recomp release folder {version} ({platform})\n", encoding="ascii")
