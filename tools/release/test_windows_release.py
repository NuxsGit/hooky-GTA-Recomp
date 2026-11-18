"""Asset-free checks for the Windows packager's SDL dependency gate."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from make_release import FILES, REPO, SDL_DLL, check_shared_sdl


class WindowsReleaseTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.build = root / "build-dll"
        self.bin = root / "sdk" / "bin"
        self.readobj = root / "llvm-readobj.exe"
        for folder in (self.build, self.bin):
            folder.mkdir(parents=True)
            for name in ("rexruntimerd.dll", SDL_DLL):
                (folder / name).write_bytes(name.encode())
        (self.build / "gtav_fast.exe").touch()
        self.imports = {"gtav_fast.exe": SDL_DLL, "rexruntimerd.dll": SDL_DLL.lower()}
        self.machine = "IMAGE_FILE_MACHINE_AMD64 (0x8664)"
        inspection = patch("make_release.subprocess.run", side_effect=self.inspect)
        self.run_readobj = inspection.start()
        self.addCleanup(inspection.stop)

    def inspect(self, args, **_):
        dll = self.imports.get(Path(args[-1]).name)
        fmt = f"ImageFileHeader {{\n  Machine: {self.machine}\n}}\n"
        if dll:
            fmt += f"Import {{\n  Name: {dll}\n  Symbol: SDL_Init (0)\n}}\n"
        return subprocess.CompletedProcess(args, 0, fmt, "")

    def check(self):
        check_shared_sdl(self.readobj, self.build, self.bin)

    def test_correct_shared_dll(self):
        self.check()
        self.assertEqual(self.run_readobj.call_count, 3)

    def test_no_static_or_other_sdl(self):
        for name, original in self.imports.items():
            for unexpected in (None, "SDL3.dll"):
                with self.subTest(binary=name, dependency=unexpected):
                    self.imports[name] = unexpected
                    with self.assertRaisesRegex(SystemExit, r"must import the shared SDL3rd\.dll"):
                        self.check()
            self.imports[name] = original

    def test_wrong_architecture(self):
        self.machine = "IMAGE_FILE_MACHINE_I386 (0x14C)"
        with self.assertRaisesRegex(SystemExit, "Not a Windows x64 binary"):
            self.check()

    def test_staged_dll_must_match_sdk(self):
        (self.build / SDL_DLL).write_bytes(b"different SDK")
        with self.assertRaisesRegex(SystemExit, "must match the patched SDK install"):
            self.check()
        self.run_readobj.assert_not_called()

    def test_partition_seed_is_from_source_tree(self):
        seed = dict((dest, src) for src, dest in FILES)["tools/game_dll/codegen.partition.json"]
        self.assertEqual(seed, REPO / "tools" / "game_dll" / "codegen.partition.json")
        self.assertTrue(seed.is_file())


if __name__ == "__main__":
    unittest.main()
