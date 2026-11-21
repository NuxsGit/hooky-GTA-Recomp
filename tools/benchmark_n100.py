#!/usr/bin/env python3
"""Helper collecte sysinfo pour Lenovo N100 + lance le binaire si present."""
import argparse, json, os, platform, shutil, subprocess, sys, pathlib

def sysinfo():
    info={}
    info["platform"]=platform.platform()
    info["cpu"]=platform.processor() or "unknown"
    try:
        with open("/proc/cpuinfo") as f:
            for l in f:
                if l.startswith("model name"):
                    info["cpu"]=l.split(":",1)[1].strip()
                    break
    except: pass
    try:
        with open("/proc/meminfo") as f:
            for l in f:
                if l.startswith("MemTotal:"):
                    info["mem_mib"]=int(l.split()[1])//1024
                    break
    except: pass
    info["threads"]=os.cpu_count()
    # AVX2
    avx=False
    try:
        import subprocess as sp
        out=sp.check_output(["lscpu"], text=True)
        avx="avx2" in out.lower()
    except: pass
    info["avx2"]=avx
    # vulkaninfo summary
    if shutil.which("vulkaninfo"):
        try:
            out=subprocess.check_output(["vulkaninfo","--summary"], text=True, timeout=5)
            info["vulkan_summary"]=out[:4000]
        except Exception as e:
            info["vulkan_error"]=str(e)
    return info

def main():
    ap=argparse.ArgumentParser(description="Lenovo N100 benchmark helper")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--json", default="")
    ap.add_argument("--bin", default="build-n100/gtav_n100_benchmark")
    args=ap.parse_args()
    info=sysinfo()
    print("=== Sysinfo N100 ===")
    print(json.dumps(info, indent=2, ensure_ascii=False))
    # try run binary
    bins=[args.bin, "/tmp/n100/gtav_n100_benchmark", "gtav/tests/build/gtav_n100_benchmark", "./gtav_n100_benchmark"]
    found=None
    for b in bins:
        if pathlib.Path(b).exists():
            found=b
            break
    if found:
        print(f"\n=== Running {found} {'--quick' if args.quick else ''} ===")
        cmd=[found]
        if args.quick: cmd.append("--quick")
        if args.json: cmd.append("--json")
        try:
            out=subprocess.check_output(cmd, text=True, timeout=60)
            print(out)
            if args.json:
                # json mode already printed by binary, also dump sysinfo
                pathlib.Path(args.json).write_text(out)
                print(f"Written {args.json}")
        except Exception as e:
            print(f"run failed: {e}", file=sys.stderr)
            sys.exit(1)
    else:
        print("Binaire non trouve, compile: cmake -S gtav/tests -B /tmp/n100 -G Ninja && cmake --build /tmp/n100 --target gtav_n100_benchmark", file=sys.stderr)
        if args.json:
            pathlib.Path(args.json).write_text(json.dumps(info, indent=2))
if __name__=="__main__": main()
