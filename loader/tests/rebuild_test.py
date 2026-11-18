"""Exercise the Linux launcher with tiny invented files and stand-ins for its bundled tools."""
import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/release"))
from release_metadata import launcher_version, prepare_output, write_metadata

TITLE = "545408A7"
PARTS = Path("0000000000000000") / TITLE / "00000002"


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def executable(path, script):
    put(path, (f"#!{sys.executable}\n" + script).encode())
    path.chmod(0o755)


def package(name, data):
    # One tiny STFS file table and one member. No game data.
    contents = bytearray(0xD000)
    contents[:4] = b"LIVE"
    contents[0x340:0x344] = (0xA000).to_bytes(4, "big")
    contents[0x37B:0x37D] = bytes((1, 1))  # one hash table per level, one file table block
    table = memoryview(contents)[0xB000:0xC000]
    table[:len(name)] = name.encode("ascii")
    table[0x28:0x2A] = bytes((0x40 | len(name), 1))  # contiguous, one data block
    table[0x2F] = 1
    table[0x32:0x34] = b"\xff\xff"  # file table root
    table[0x34:0x38] = len(data).to_bytes(4, "big")
    contents[0xC000:0xC000 + len(data)] = data
    return contents


def create_fixture(root, inputs):
    disc1, disc2, tu = (inputs / d for d in ("Disc 1", "Disc 2", "TU27"))
    reference = []

    def record(kind, name, data):
        reference.append(f"{kind} {len(data)} {hashlib.sha256(data).hexdigest()} {name}\n")

    for name in ("xbox360b.rpf", "audio_rel.rpf", "default.xex"):
        data = f"fixture bytes for {name}".encode()
        record("disc2", name, data)
        put(disc2 / name, data)
    for i in range(4):
        name, pkg, data = f"part{i}.rpf", f"{TITLE}0000000{i}", f"fixture partition {i}".encode()
        record("part", f"{pkg}/{name}", data)
        put(disc1 / "content" / PARTS / pkg, package(name, data))
    for name in ("update.rpf", "default.xexp"):
        data = f"fixture update {name}".encode()
        record("tu", name, data)
        put(tu / "disc002" / name, data)
    record("gen", "gtav_init.cpp", b"// fixture\n")
    game_tools = root / "tools/game_dll"
    put(game_tools / "expected.txt", "".join(reference).encode())
    for f in ("gtav_manifest.toml", "codegen.partition.json", "fs/gtav_pch.h", "fs/layout_check.h",
              "fs/game_dll.cpp", "fs/fs_crt.cpp", "interface/gtav_hooks.h", "interface/gtav_game.exports"):
        put(game_tools / f, b"fixture build input\n")
    put(root / "librexruntime.so", b"")
    put(root / "librexgpu-xenos.so", b"")
    executable(root / "gtav_fast", "import sys\nassert '--fixture-game-arg' in sys.argv\nprint('fake game started')\n")
    executable(root / "rexglue", "from pathlib import Path\nPath('generated/default/gtav_init.cpp').write_bytes(b'// fixture\\n')\n")
    executable(root / "tools/llvm/bin/clang++", """import sys
from pathlib import Path
assert Path(sys.argv[sys.argv.index('-c') + 1]).is_file()
Path(sys.argv[sys.argv.index('-o') + 1]).write_bytes(b'fixture object')
""")
    executable(root / "tools/llvm/bin/ld.lld", """import os, sys, time
from pathlib import Path
out = Path(sys.argv[sys.argv.index('-o') + 1])
out.write_bytes(b'fixture library')
if os.getenv('FIXTURE_BLOCK_LINK'):
    time.sleep(60)
if os.getenv('FIXTURE_FAIL_LINK'):
    sys.exit('fixture link failure')
""")
    return disc1, disc2, tu


def exercise(launcher, library_dir):
    with tempfile.TemporaryDirectory(prefix="gtav-launcher-test-") as tmp:
        folder = Path(tmp)
        root = prepare_output(folder / "Récompilation locale")
        inputs = folder / "Données originales"
        sources = create_fixture(root, inputs)
        root_exe = root / "gtav_launcher"
        shutil.copy2(launcher, root_exe)
        fake_bin = folder / "fake-bin"
        executable(fake_bin / "curl", """import os, sys
assert sys.argv[1] == '-q' and '--location' not in sys.argv
assert sys.argv[-1] == 'https://api.github.com/repos/NuxsGit/hooky-GTA-Recomp/releases/latest'
if 'FIXTURE_HTTP' not in os.environ or os.environ['FIXTURE_HTTP'] == 'offline':
    sys.exit(6)
body = 'x' * (2**20 + 1) if os.getenv('FIXTURE_OVERSIZE') else os.getenv('FIXTURE_BODY', '')
sys.stdout.write(body + '\\n' + os.environ['FIXTURE_HTTP'])
""")
        env = dict(os.environ, HOME=str(folder / "home"), XDG_DATA_HOME=str(folder / "xdg"),
                   LD_LIBRARY_PATH=str(library_dir), PATH=str(fake_bin) + os.pathsep + os.environ["PATH"])
        for key in ("FIXTURE_BLOCK_LINK", "FIXTURE_FAIL_LINK", "FIXTURE_OVERSIZE", "FIXTURE_HTTP", "FIXTURE_BODY"):
            env.pop(key, None)

        def run(*args, expect=0, contains=None, **vars):
            result = subprocess.run([root_exe, *args], input="", text=True, capture_output=True,
                                    timeout=20, env=dict(env, **vars))
            output = result.stdout + result.stderr
            assert result.returncode == expect, output
            if contains:
                assert contains in output, output
            return result.stdout.strip()

        version = run("--version")
        assert launcher_version(launcher.parent) == version
        write_metadata(root, root, "linux-x64", version)
        build_id = root / "tools/game_dll/build-id.txt"
        first_id = build_id.read_bytes()
        next_version = f"{int(version.split('.')[0]) + 1}.0.0"
        release = dict(tag_name="v" + next_version, draft=False, prerelease=False, assets=[dict(
            name="gtav-recomp-linux-x64.tar.gz", state="uploaded", browser_download_url="https://example.invalid/")])
        empty = sorted(root.iterdir())
        available = run("--check-updates", contains="Update available", FIXTURE_HTTP="200", FIXTURE_BODY=json.dumps(release))
        assert available.endswith("https://github.com/NuxsGit/hooky-GTA-Recomp/releases/tag/v" + next_version)
        assert "example.invalid" not in available
        run("--check-updates", contains="No stable release", FIXTURE_HTTP="404")
        run("--check-updates", expect=1, contains="HTTP 403", FIXTURE_HTTP="403")
        run("--check-updates", expect=1, contains="Could not contact GitHub", FIXTURE_HTTP="offline")
        run("--check-updates", expect=1, contains="too large", FIXTURE_HTTP="200", FIXTURE_OVERSIZE="1")
        run("--check-updates", expect=1, contains="need curl", PATH=str(folder / "no-curl"))
        assert sorted(root.iterdir()) == empty and not (root / "data").exists()

        # Wrong roles and a wrong TU of the right size fail before copying anything.
        disc1, disc2, tu = sources
        run("--setup", disc2, disc1, tu, expect=1, contains="Swap the two discs")
        patch = tu / "disc002/default.xexp"
        correct_patch = patch.read_bytes()
        patch.write_bytes(b"X" * len(correct_patch))
        run("--setup", *sources, expect=1, contains="not the supported TU27")
        assert not (root / "data/game").exists()
        patch.write_bytes(correct_patch)
        marker = root / "data/installed.txt"
        library = root / "gtav_game.so"
        run("--setup", *sources, expect=1, contains="fixture link failure", FIXTURE_FAIL_LINK="1")
        assert not marker.exists() and not library.exists()
        assert (root / "data/install_state.txt").is_file()
        shutil.rmtree(inputs)  # A cancelled/failed compile can be retried without the sources.

        save = root / "data/user/local-save"
        config = root / "gtav.toml"
        put(save, b"local save fixture")
        put(config, b"configuration fixture")
        protected = [save, config, root / "data/game/default.xex", root / "data/game/default.xexp",
                     root / "data/update/update.rpf", root / "data/launcher.ini"]
        before = {p: p.read_bytes() for p in protected}
        run("--rebuild", contains="Game build ready")
        assert not (root / "data/build").exists() and not (root / "data/install_state.txt").exists()
        current_marker = marker.read_bytes()
        assert current_marker.splitlines()[1] == first_id.strip()
        run("--", "--fixture-game-arg", contains="fake game started")

        # A runtime-only release retains the same ID; a changed build input and an unproven old
        # install marker both send the player back to setup instead of starting the wrong library.
        write_metadata(root, root, "linux-x64", next_version)
        assert build_id.read_bytes() == first_id
        run("--", "--fixture-game-arg", contains="fake game started")
        marker.write_bytes(b"gtav-recomp install 1\n")
        run("--cli", expect=1, contains="Existing game files found")
        marker.write_bytes(current_marker)
        put(root / "tools/game_dll/fs/layout_check.h", b"changed input")
        write_metadata(root, root, "linux-x64", next_version)
        assert build_id.read_bytes() != first_id
        run("--cli", expect=1, contains="Existing game files found")
        run("--rebuild", contains="Game build ready")
        run("--", "--fixture-game-arg", contains="fake game started")

        old_library, old_stamp = library.read_bytes(), marker.read_bytes()
        local_patch = root / "data/game/default.xexp"
        local_patch.write_bytes(b"X" * len(correct_patch))
        run("--rebuild", expect=1, contains="does not match")
        local_patch.write_bytes(correct_patch)

        staged = library.with_suffix(".so.new")
        proc = subprocess.Popen([root_exe, "--rebuild"], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=dict(env, FIXTURE_BLOCK_LINK="1"))
        try:
            deadline = time.monotonic() + 10
            while not staged.is_file() and proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            assert staged.is_file(), "did not reach link stage"
            run("--rebuild", expect=1, contains="Could not lock the game folder")
            proc.send_signal(signal.SIGINT)
            stdout, stderr = proc.communicate(timeout=8)
            assert proc.returncode == 1 and "Cancelled" in stderr, stdout + stderr
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.communicate()
        assert not staged.exists()
        assert library.read_bytes() == old_library and marker.read_bytes() == old_stamp
        assert {p: p.read_bytes() for p in protected} == before
        try:
            prepare_output(root)  # even with gtav_release.txt this is not a staging folder anymore
        except SystemExit as error:
            assert "new or empty staging folder" in str(error)
        else:
            raise AssertionError("packager accepted a live installation")
        assert save.read_bytes() == before[save]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("launcher", type=Path)
    parser.add_argument("test_library_dir", type=Path)
    args = parser.parse_args()
    exercise(args.launcher.resolve(), args.test_library_dir.resolve())
    print("Launcher fixture passed")
