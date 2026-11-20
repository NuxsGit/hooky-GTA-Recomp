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
    generated = {"gtav_init.cpp": '// fixture\n', "gtav_init.h": '// init\n', "gtav_funcs.h": '// init funcs\n',
                 "gtav_recomp.0.cpp": '// first unit\n', "gtav_funcs.0.h": '// first unit header\n',
                 "gtav_recomp.1.cpp": '// second unit\n', "gtav_funcs.1.h": '// second unit header\n'}
    for name, text in generated.items():
        record("gen", name, text.encode())
    game_tools = root / "tools/game_dll"
    put(game_tools / "fixture-gen.json", json.dumps(generated).encode())
    for file in ("libTracyClient.so", "LICENSES/synthetic.txt", "README.txt"):
        put(root / file, b"synthetic package\n")
    put(game_tools / "expected.txt", "".join(reference).encode())
    for f in ("gtav_manifest.toml", "codegen.partition.json", "fs/gtav_pch.h", "fs/layout_check.h",
              "fs/game_dll.cpp", "fs/fs_crt.cpp", "interface/gtav_hooks.h", "interface/gtav_game.exports"):
        put(game_tools / f, b"fixture build input\n")
    put(root / "librexruntime.so", b"")
    put(root / "librexgpu-xenos.so", b"")
    executable(root / "gtav_fast", """import os, sys, time
from pathlib import Path
root = Path(__file__).resolve().parent
assert Path.cwd() == root
assert f'--user_data_root={root}/data/user' in sys.argv
assert '--fixture-game-arg' in sys.argv
modes = [arg for arg in sys.argv if arg.startswith('--gtav_mode=')]
assert len(modes) == 1, sys.argv
assert f'--gtav_settings_path={root}/data/config/graphics.ini' in sys.argv
if '--no-headless' in sys.argv:
    assert '--headless=true' not in sys.argv
else:
    assert '--headless=true' in sys.argv
if '--gtav_native=shadow' in sys.argv:
    assert '--gtav_native=on' not in sys.argv and '--log_level=info' not in sys.argv
    assert sys.argv[sys.argv.index('--log_level') + 1] == 'debug', sys.argv
assert '--gtav_fps_limit=30' not in sys.argv and '--gtav_native_vsync=false' not in sys.argv
print('fake game started:', modes[0])
if os.getenv('FIXTURE_BLOCK_GAME'):
    (root / 'data/logs/fixture-playing').touch()
    time.sleep(60)
""")
    executable(root / "rexglue", """import json, os
from pathlib import Path
root = Path(__file__).resolve().parent
with (root / 'calls').open('a') as log:
    log.write('codegen\\n')
for name, text in json.loads((root / 'tools/game_dll/fixture-gen.json').read_text()).items():
    Path('generated/default', name).write_text(text)
if os.getenv('FIXTURE_BAD_GEN'):
    Path('generated/default/gtav_init.cpp').write_text('bad generated code')
""")
    executable(root / "tools/llvm/bin/clang++", """import os, sys, time
from pathlib import Path
root = Path(__file__).resolve().parents[3]
source = Path(sys.argv[sys.argv.index('-c') + 1])
assert source.is_file()
with (root / 'calls').open('a') as log:
    log.write(source.name + '\\n')
Path(sys.argv[sys.argv.index('-o') + 1]).write_bytes(b'fixture object for ' + source.read_bytes())
if os.getenv('FIXTURE_BLOCK_COMPILE'):
    time.sleep(60)
""")
    executable(root / "tools/llvm/bin/ld.lld", """import os, sys, time
from pathlib import Path
root = Path(__file__).resolve().parents[3]
with (root / 'calls').open('a') as log:
    log.write('link\\n')
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
        incoming = root
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
        for key in ("FIXTURE_BLOCK_LINK", "FIXTURE_BLOCK_COMPILE", "FIXTURE_BAD_GEN", "FIXTURE_FAIL_LINK",
                    "FIXTURE_OVERSIZE", "FIXTURE_HTTP", "FIXTURE_BODY", "FIXTURE_BLOCK_GAME", "CPATH", "CPLUS_INCLUDE_PATH",
                    "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "SDKROOT"):
            env.pop(key, None)

        def run(*args, expect=0, contains=None, **vars):
            result = subprocess.run([root_exe, *args], input="", text=True, capture_output=True,
                                    timeout=20, env=dict(env, **vars))
            output = result.stdout + result.stderr
            assert result.returncode == expect, output
            if contains:
                assert contains in output, output
            return result.stdout.strip()

        calls = root / "calls"

        def take_calls():
            result = calls.read_text().splitlines() if calls.exists() else []
            calls.unlink(missing_ok=True)
            return sorted(result)

        def invokes(*expected):
            assert take_calls() == sorted(expected), f"expected tools: {expected}"

        all_units = ("gtav_init.cpp", "gtav_recomp.0.cpp", "gtav_recomp.1.cpp", "game_dll.cpp", "fs_crt.cpp")
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

        # The extracted launcher seeds Games then delegates. Even a setup that fails validation
        # created no second data tree next to the archive. Version checks above wrote nothing.
        disc1, disc2, tu = sources
        home = Path(env["HOME"]) / "Games/GTAV Recomp"
        assert not home.exists()
        run("--setup", disc2, disc1, tu, expect=1, contains="Swap the two discs")
        assert (home / "gtav_launcher").is_file() and not (incoming / "data").exists()
        root = home
        calls = home / "calls"
        build_id = home / "tools/game_dll/build-id.txt"
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
        invokes("codegen", *all_units, "link")
        shutil.rmtree(inputs)  # A cancelled/failed compile can be retried without the sources.

        save = root / "data/user/local-save"
        config = root / "gtav.toml"
        put(save, b"local save fixture")
        put(config, b"configuration fixture")
        protected = [save, config, root / "data/game/default.xex", root / "data/game/default.xexp",
                     root / "data/update/update.rpf", root / "data/launcher.ini"]
        before = {p: p.read_bytes() for p in protected}
        run("--rebuild", contains="5 reused, 0 to compile")
        invokes("link")
        build = root / "data/build"
        assert build.is_dir() and not (build / "assets").exists()
        assert not (root / "data/install_state.txt").exists()
        current_marker = marker.read_bytes()
        assert current_marker.splitlines()[1] == first_id.strip()
        # Reopen from the archive or the installed directory. Neither route moves local saves.
        run("--play", "--", "--fixture-game-arg", contains="fake game started: --gtav_mode=menu")
        run("--play", "--", "--fixture-game-arg", "--gtav_mode=campaign", contains="fake game started: --gtav_mode=campaign")
        run("--play", "--", "--fixture-game-arg", "--gtav_native=shadow", "--log_level", "debug",
            "--no-headless", "--gtav_fps_limit=60", contains="fake game started: --gtav_mode=menu")
        entry = Path(env["XDG_DATA_HOME"]) / "applications/gtav-recomp.desktop"
        assert f'Exec="{home / "gtav_launcher"}"' in entry.read_text()
        assert not (incoming / "data").exists() and not (incoming / "gtav_game.so").exists()
        root_exe = home / "gtav_launcher"
        shutil.rmtree(incoming)  # deleting the unpacked ZIP/tarball folder does not break Play
        run("--play", "--", "--fixture-game-arg", contains="fake game started")

        running = home / "data/logs/fixture-playing"
        game = subprocess.Popen([root_exe, "--play", "--", "--fixture-game-arg"],
                                env=dict(env, FIXTURE_BLOCK_GAME="1"), stdout=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 8
            while not running.exists() and game.poll() is None and time.monotonic() < deadline:
                time.sleep(.02)
            assert running.exists() and game.poll() is None
            run("--rebuild", expect=1, contains="Close the game or other setup")
            run("--play", "--cli", "--", "--fixture-game-arg", expect=1, contains="being used by another game")
        finally:
            game.terminate()
            game.wait(timeout=4)

        # A runtime-only release retains the same ID; a changed build input and an unproven old
        # install marker both send the player back to setup instead of starting the wrong library.
        write_metadata(root, root, "linux-x64", next_version)
        assert build_id.read_bytes() == first_id
        run("--", "--fixture-game-arg", contains="fake game started")
        marker.write_bytes(b"gtav-recomp install 1\n")
        run("--cli", expect=1, contains="Existing game files found")
        marker.write_bytes(current_marker)
        invokes()  # a runtime-only update launches the existing library without running any tools

        def change_build_input(path, content):
            put(root / path, content)
            previous = build_id.read_bytes()
            write_metadata(root, root, "linux-x64", next_version)
            assert build_id.read_bytes() != previous

        # Link-only inputs are not part of the compile key.
        change_build_input("tools/game_dll/interface/gtav_game.exports", b"fixture exports v2")
        run("--cli", expect=1, contains="Existing game files found")
        run("--rebuild", contains="Game assets: 7 unchanged, 0 rehashed")
        invokes("link")
        change_build_input("tools/game_dll/fs/game_dll.cpp", b"one changed source")
        run("--rebuild", contains="4 reused, 1 to compile")
        invokes("game_dll.cpp", "link")

        def change_generated(name, text):
            fixture = root / "tools/game_dll/fixture-gen.json"
            values = json.loads(fixture.read_text())
            values[name] = text
            fixture.write_text(json.dumps(values))
            reference = root / "tools/game_dll/expected.txt"
            replacement = f"gen {len(text)} {hashlib.sha256(text.encode()).hexdigest()} {name}"
            lines = [replacement if line.endswith(" " + name) else line for line in reference.read_text().splitlines()]
            change_build_input(reference.relative_to(root), ("\n".join(lines) + "\n").encode())

        change_generated("gtav_recomp.0.cpp", "// revised partition\n")
        run("--rebuild", contains="4 reused, 1 to compile")
        invokes("codegen", "gtav_recomp.0.cpp", "link")
        change_generated("gtav_funcs.1.h", "// revised partition header\n")
        run("--rebuild", contains="4 reused, 1 to compile")
        invokes("codegen", "gtav_recomp.1.cpp", "link")
        change_generated("gtav_funcs.h", "// revised global declarations\n")
        run("--rebuild", contains="4 reused, 1 to compile")
        invokes("codegen", "gtav_init.cpp", "link")
        gen = build / "generated/default/gtav_recomp.1.cpp"
        put(gen, b"?" * gen.stat().st_size)
        run("--rebuild", expect=1, contains="differ from the reference build", FIXTURE_BAD_GEN="1")
        invokes("codegen")
        run("--rebuild", contains="5 reused, 0 to compile")
        invokes("codegen", "link")

        obj = build / "obj/gtav_recomp.0.cpp.o"
        original = obj.read_bytes()
        put(obj, b"X" * len(original))  # Same-length object corruption must not be reused.
        run("--rebuild", contains="4 reused, 1 to compile")
        invokes("gtav_recomp.0.cpp", "link")
        assert obj.read_bytes() == original

        # Common headers or compiler contents invalidate all affected objects.
        change_build_input("tools/game_dll/fs/layout_check.h", b"changed input")
        run("--rebuild", contains="0 reused, 5 to compile")
        invokes(*all_units, "link")
        clang = root / "tools/llvm/bin/clang++"
        change_build_input(clang.relative_to(root), clang.read_bytes() + b"\n# new compiler fixture\n")
        run("--rebuild", contains="0 reused, 5 to compile")
        invokes(*all_units, "link")
        lld = root / "tools/llvm/bin/ld.lld"
        change_build_input(lld.relative_to(root), lld.read_bytes() + b"\n# new linker fixture\n")
        run("--rebuild", contains="5 reused, 0 to compile")
        invokes("link")
        run("--", "--fixture-game-arg", contains="fake game started: --gtav_mode=menu")
        run("--", "--fixture-game-arg", "--gtav_mode=campaign", contains="fake game started: --gtav_mode=campaign")

        # A changed non-code asset is checked without disc re-import; matching receipt entries
        # for large, untouched assets don't need a full rehash.
        asset = root / "data/update/update.rpf"
        good = asset.read_bytes()
        put(asset, bytes(len(good)))
        os.utime(asset, (time.time() + 3,) * 2)
        run("--rebuild", expect=1, contains="differ from a known good dump")
        invokes()
        put(asset, good)
        run("--rebuild", contains="6 unchanged, 1 rehashed")
        invokes("link")

        old_library, old_stamp = library.read_bytes(), marker.read_bytes()
        local_patch = root / "data/game/default.xexp"
        local_patch.write_bytes(b"X" * len(correct_patch))
        run("--rebuild", expect=1, contains="does not match")
        local_patch.write_bytes(correct_patch)

        def cancel_at(staged, **variables):
            proc = subprocess.Popen([root_exe, "--rebuild"], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, env=dict(env, **variables))
            try:
                deadline = time.monotonic() + 10
                while not staged.is_file() and proc.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                assert staged.is_file(), f"did not stage {staged}"
                run("--rebuild", expect=1, contains="Could not lock the game folder")
                proc.send_signal(signal.SIGINT)
                stdout, stderr = proc.communicate(timeout=8)
                assert proc.returncode == 1 and "Cancelled" in stderr, stdout + stderr
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()
            assert not staged.exists()

        obj.unlink()
        cancel_at(obj.with_suffix(".o.new"), FIXTURE_BLOCK_COMPILE="1")
        invokes("gtav_recomp.0.cpp")
        assert not obj.exists()
        run("--rebuild", expect=1, contains="fixture link failure", FIXTURE_FAIL_LINK="1")
        invokes("gtav_recomp.0.cpp", "link")
        cancel_at(library.with_suffix(".so.new"), FIXTURE_BLOCK_LINK="1")
        invokes("link")
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
