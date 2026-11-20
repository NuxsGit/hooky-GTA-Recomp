"""Build the small embedded asset bundle; no image tools needed at build time."""
import sys
from pathlib import Path

FILES = {
    "kStoryArt": "harbor-story.png",
    "kSandboxArt": "sandbox-yard.png",
    "kPrivateArt": "sandbox-night.png",
    "kBodyTtf": "Barlow-Medium.ttf",
    "kDisplayTtf": "BarlowCondensed-Bold.ttf",
}


def main():
    out = Path(sys.argv[1])
    assets = Path(__file__).with_name("assets")
    with out.open("w", encoding="ascii", newline="\n") as file:
        file.write("// Generated from original art and the bundled OFL fonts.\n")
        for symbol, name in FILES.items():
            data = (assets / name).read_bytes()
            file.write(f"constexpr unsigned char {symbol}[] = {{\n")
            for i in range(0, len(data), 24):
                file.write("    " + ",".join(f"0x{byte:02x}" for byte in data[i:i + 24]) + ",\n")
            file.write("};\n")


if __name__ == "__main__":
    main()
