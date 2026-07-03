from pathlib import Path

Import("env")


def merge_esp8285_image(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    out = build_dir / "firmware_1m_merged.bin"
    boot = build_dir / "firmware.bin"
    irom = build_dir / "firmware.bin.irom0text.bin"

    image = bytearray([0xFF]) * (1024 * 1024)
    parts = [(0x00000, boot)]
    if irom.exists():
        parts.append((0x20000, irom))

    for offset, path in parts:
        data = path.read_bytes()
        image[offset:offset + len(data)] = data

    out.write_bytes(image)
    print(f"Merged ESP8285 image: {out} ({len(image)} bytes)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_esp8285_image)
