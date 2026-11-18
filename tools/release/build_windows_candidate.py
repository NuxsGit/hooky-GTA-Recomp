"""Build and smoke-test the Windows x64 player ZIP on a clean Windows checkout.

Run in a Visual Studio x64 Developer Command Prompt with CMake 3.25–3.x,
Ninja, Git and Python. This never reads discs, generates retail game code,
adds a mobile shell, or publishes a release. Keep the release in draft until
an unpacked candidate passes the local game and physical pad tests.
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACKAGE = "gtav-recomp-windows-x64"
DEPS = (
    ("rexglue-sdk", "rexglue/rexglue-sdk", "f5337cdc947ff6d4c4196737e2c807a48f2a1fc2",
     "rexglue-sdk-gtav-linux.patch", "v0.10.0"),
    ("thirdparty/XenosRecomp", "hedge-dev/XenosRecomp", "990d03b28a27b50277ee5d8d942e1c5f873869d1",
     "xenosrecomp-gtav.patch", None),
    ("thirdparty/plume", "renderbag/plume", "0ce184b19270e49236afbb74052a6b267494b86b",
     "plume-gtav.patch", None),
)


def annotate(message):
    if os.environ.get("GITHUB_ACTIONS") == "true":
        message = str(message).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=Windows release build::{message}", flush=True)


def run(*args, capture=False, cwd=REPO, timeout=None):
    args = tuple(map(str, args))
    show_failure = os.environ.get("GITHUB_ACTIONS") == "true"
    print(f"[{cwd}] {subprocess.list2cmdline(args)}", flush=True)
    result = subprocess.run(args, cwd=cwd, text=True, errors="replace", timeout=timeout,
                            stdout=subprocess.PIPE if capture or show_failure else None,
                            stderr=subprocess.STDOUT if capture or show_failure else None)
    if not capture and result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        lines = (result.stdout or "").splitlines()
        if capture:
            print(result.stdout or "", end="", flush=True)
        annotate(f"Exit {result.returncode}: {subprocess.list2cmdline(args)}")
        pattern = re.compile(r"\b(error(:| [CLD]\d+)|CMake Error|FAILED:|not found|missing:|Traceback)", re.I)
        first = [i for i, line in enumerate(lines) if pattern.search(line)]
        details = {j for i in first[:6] for j in range(i, min(i + 3, len(lines)))}
        tail = set(range(max(0, len(lines) - 8), len(lines)))
        if details or tail:
            annotate("\n".join(lines[i][:300] for i in sorted(details | tail))[:6500])
        result.check_returncode()
    return result.stdout


def checkout_dependencies(root):
    for subdir, repo, sha, patch, tag in DEPS:
        tree = root / subdir
        print(f"Fetching {repo} at {sha}", flush=True)
        tree.parent.mkdir(parents=True, exist_ok=True)
        run("git", "init", "-q", tree, cwd=root)
        git = ("git", "-C", tree, "-c", "core.autocrlf=false", "-c", "core.longpaths=true")
        run(*git, "remote", "add", "origin", f"https://github.com/{repo}.git", cwd=root)
        ref = f"refs/tags/{tag}:refs/tags/{tag}" if tag else sha
        run(*git, "fetch", "--depth=1", "origin", ref, cwd=root)
        fetched = run(*git, "rev-parse", "FETCH_HEAD^{commit}", capture=True, cwd=root).strip()
        if fetched != sha:
            raise SystemExit(f"{repo}: expected {sha}, got {fetched}")
        run(*git, "checkout", "--detach", sha, cwd=root)
        run(*git, "submodule", "update", "--init", "--recursive", "--depth=1", "--jobs=4", cwd=root)
        run(*git, "apply", root / patch, cwd=root)


def build_sdk(sdk, install, clang, cache_root):
    # The SDK's Windows D3D12 backend runs the guest command processor. The native
    # renderer uses Vulkan via Plume, independently of that SDK backend.
    backend = ("-DREXGLUE_USE_D3D12=ON", "-DREXGLUE_USE_VULKAN=OFF")
    identity = "\n".join((DEPS[0][2], "RelWithDebInfo", "LLVM 19.1.5", *backend,
                           *(os.environ.get(k, "") for k in ("ImageVersion", "VCToolsVersion", "WindowsSDKVersion"))))
    patch = (REPO / DEPS[0][3]).read_bytes()
    key = hashlib.sha256(identity.encode("utf-8") + b"\0" + patch).hexdigest()
    cache = cache_root.resolve() / key if cache_root else None
    stamp = cache / ".complete" if cache else None
    if stamp and stamp.is_file() and stamp.read_text(encoding="ascii") == key:
        print(f"Reusing installed SDK {key}", flush=True)
        shutil.copytree(cache / "install", install)
        return
    run("cmake", "--preset", "win-amd64", *backend, *clang, cwd=sdk)
    sdk_build = sdk / "out/build/win-amd64"
    # Keep Ninja compiling independent targets after a failure to surface all diagnostics at once.
    run("cmake", "--build", sdk_build, "--config", "RelWithDebInfo", "--parallel", "--", "-k", "0")
    run("cmake", "--install", sdk_build, "--config", "RelWithDebInfo")
    if cache:
        shutil.copytree(install, cache / "install", dirs_exist_ok=True)
        (cache / ".complete").write_text(key, encoding="ascii")


def smoke_package(archive):
    # Test the extracted download, not the build directory (which could hide missing DLLs).
    with tempfile.TemporaryDirectory(prefix="gtav-win-smoke-") as tmp:
        with zipfile.ZipFile(archive) as zf:
            forbidden = {"gtav_game.dll", "default.xex", "default.xexp", "data", "shader_cache"}
            for name in zf.namelist():
                parts = name.casefold().split("/")
                if parts[0] != PACKAGE or forbidden.intersection(parts) or ".." in parts:
                    raise SystemExit(f"Unexpected file in Windows package: {name}")
            zf.extractall(tmp)
        host = Path(tmp) / PACKAGE
        for exe, args in (
            ("rexgluerd.exe", ("--version",)),
            ("tools/XenosRecomp.exe", ()),
            ("tools/dxc.exe", ("-help",)),
            ("tools/llvm/bin/clang++.exe", ("--version",)),
            ("tools/llvm/bin/lld-link.exe", ("/?",)),
        ):
            run(host / exe, *args, capture=True, cwd=host, timeout=30)
        # The unpacked launcher should reach setup and reject the missing config headlessly.
        result = subprocess.run([host / "gtav_launcher.exe", "--unattended"], cwd=host, timeout=30)
        log = host / "data/logs/unattended.txt"
        if result.returncode != 1 or not log.is_file() or "launcher.ini needs three lines" not in log.read_text():
            raise SystemExit(f"Unpacked launcher did not report missing launcher.ini (exit {result.returncode})")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--llvm", type=Path, required=True, help="official unpacked LLVM 19.1.5 win64")
    ap.add_argument("--dxc-license", type=Path, required=True, help="DirectXShaderCompiler LICENSE.TXT")
    ap.add_argument("--sdk-cache", type=Path, help="optional runner cache of a matching, complete SDK install")
    args = ap.parse_args()
    if os.name != "nt" or platform.machine().lower() not in ("amd64", "x86_64"):
        raise SystemExit("This builder needs native Windows x64")
    if not os.environ.get("VCToolsRedistDir") or not shutil.which("cl.exe"):
        raise SystemExit("Start a Visual Studio x64 Developer Command Prompt first")
    if run("git", "status", "--porcelain", capture=True).strip():
        raise SystemExit("Build from a clean checkout at the intended release commit")
    for rel in [*(dep[0] for dep in DEPS), "gtav/generated", "gtav/assets", "build-dll", "build-xr",
                "build-launcher", "build/controller-check", f"dist/{PACKAGE}", f"dist/{PACKAGE}.zip"]:
        if (REPO / rel).exists():
            raise SystemExit(f"Use a fresh checkout: refusing to reuse {rel}")
    llvm = args.llvm.resolve(strict=True)
    dxc_license = args.dxc_license.resolve(strict=True)
    llvm_bin = llvm / "bin"
    os.environ["PATH"] = str(llvm_bin) + os.pathsep + os.environ["PATH"]
    if "clang version 19.1.5" not in run(llvm_bin / "clang.exe", "--version", capture=True):
        raise SystemExit("Expected the official LLVM 19.1.5 release bundle")
    checkout_dependencies(REPO)

    sdk = REPO / "rexglue-sdk"
    install = sdk / "out/install/win-amd64"
    rel = "RelWithDebInfo"
    clang = (f"-DCMAKE_C_COMPILER={llvm_bin / 'clang.exe'}",
             f"-DCMAKE_CXX_COMPILER={llvm_bin / 'clang++.exe'}",
             f"-DCMAKE_LINKER={llvm_bin / 'lld-link.exe'}")
    build_sdk(sdk, install, clang, args.sdk_cache)

    run("cmake", "-S", "thirdparty/XenosRecomp", "-B", "build-xr", "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
        "-DBUILD_SHARED_LIBS=OFF", "-DXENOS_RECOMP_DXIL=OFF")
    run("cmake", "--build", "build-xr", "--target", "XenosRecomp", "--parallel")
    run("cmake", "-S", "gtav", "--preset", "win-amd64-relwithdebinfo", "-B", "build-dll",
        "-DGTAV_GAME_DLL=ON", f"-Drexglue_DIR={install / 'lib/cmake/rexglue'}",
        f"-DREX_SDK_SRC={sdk / 'src'}", *clang)
    run("cmake", "--build", "build-dll", "--parallel")
    run("cmake", "-S", "loader", "-B", "build-launcher", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=cl", "-DGTAV_LAUNCHER_TESTS=ON", f"-DGTAV_SDK_THIRDPARTY={sdk / 'thirdparty'}")
    run("cmake", "--build", "build-launcher", "--parallel")
    run("ctest", "--test-dir", "build-launcher", "--output-on-failure")
    run("cmake", "-S", "tools/input_tests/sdl_probe", "-B", "build/controller-check", "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={rel}", "-DCMAKE_C_COMPILER=cl", f"-DCMAKE_PREFIX_PATH={install}")
    run("cmake", "--build", "build/controller-check", "--parallel")
    run("ctest", "--test-dir", "build/controller-check", "--output-on-failure")
    run("python", "tools/release/test_windows_release.py")

    licenses = REPO / "build/windows-licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(dxc_license, licenses / "dxc-LICENSE.TXT")
    shutil.copyfile(REPO / "LICENSE", licenses / "GPL-3.0.txt")
    run("python", "tools/release/make_release.py", f"dist/{PACKAGE}", "--llvm", llvm,
        "--expected", "tools/release/expected.txt", "--licenses", licenses, "--zip")
    smoke_package(REPO / "dist" / f"{PACKAGE}.zip")
    print(f"Windows candidate: dist/{PACKAGE}.zip (in-game validation still required)", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.TimeoutExpired) as exc:
        annotate(exc)
        raise
    except SystemExit as exc:
        if isinstance(exc.code, str):
            annotate(exc.code)
        raise
