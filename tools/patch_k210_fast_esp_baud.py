#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count == 0:
        if new in text:
            print(f"already patched: {label}")
            return text
        raise SystemExit(f"pattern not found for {label}")
    if count != 1:
        raise SystemExit(f"pattern for {label} found {count} times, refusing")
    return text.replace(old, new, 1)


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch K210 esp_flasher.c to switch ESP8266/ESP8285 stub to faster UART baud")
    ap.add_argument("k210_repo", nargs="?", default=r"D:\w_space\K210_AI_V7s_Plus")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    repo = Path(args.k210_repo)
    path = repo / "src" / "esp_flasher.c"
    if not path.exists():
        raise SystemExit(f"not found: {path}")

    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#include <esp_loader.h>\n#include <esp_loader_io.h>\n",
        "#include <esp_loader.h>\n#include <esp_loader_io.h>\n#include \"../third_party/esp_serial_flasher/private_include/protocol.h\"\n",
        "private protocol include",
    )

    text = replace_once(
        text,
        "#define ESP_FLASH_BAUD       115200u\n#define ESP_FLASH_BLOCK      1024u\n",
        f"#define ESP_FLASH_BAUD       115200u\n#define ESP_FLASH_FAST_BAUD  {args.baud}u\n#define ESP_FLASH_BLOCK      1024u\n",
        "fast baud define",
    )

    marker = "static bool esp_flash_open_loader(esp_loader_t *loader)\n{\n"
    helper = f'''static void esp_flash_try_fast_baud(esp_loader_t *loader)\n{{\n    if (ESP_FLASH_FAST_BAUD == ESP_FLASH_BAUD)\n        return;\n\n    uint32_t old_baud = s_port.baud ? s_port.baud : ESP_FLASH_BAUD;\n    LOGF("[esp-flash] baud switch try %lu -> %lu",\n         (unsigned long)old_baud, (unsigned long)ESP_FLASH_FAST_BAUD);\n\n    loader->_port->ops->start_timer(loader->_port, 1000);\n    esp_loader_error_t err = loader_change_baudrate_cmd(loader, ESP_FLASH_FAST_BAUD, old_baud);\n    if (err == ESP_LOADER_SUCCESS) {{\n        vTaskDelay(pdMS_TO_TICKS(25));\n        err = k210_change_baud(&s_port.port, ESP_FLASH_FAST_BAUD);\n    }}\n\n    if (err == ESP_LOADER_SUCCESS) {{\n        LOGF("[esp-flash] baud switched to %lu", (unsigned long)s_port.baud);\n    }} else {{\n        LOGF("[esp-flash] baud switch failed: %d, keep %lu", err, (unsigned long)s_port.baud);\n    }}\n}}\n\n'''
    if helper.strip() not in text:
        text = replace_once(text, marker, helper + marker, "fast baud helper")
    else:
        print("already patched: fast baud helper")

    text = replace_once(
        text,
        "    target_chip_t chip = esp_loader_get_target(loader);\n    LOGF(\"[esp-flash] connected target=%s\", target_name(chip));\n    LOGF(\"[esp-flash] session baud=%lu block=%lu\", (unsigned long)ESP_FLASH_BAUD,\n         (unsigned long)sizeof(s_flash_buf));\n",
        "    target_chip_t chip = esp_loader_get_target(loader);\n    LOGF(\"[esp-flash] connected target=%s\", target_name(chip));\n    esp_flash_try_fast_baud(loader);\n    LOGF(\"[esp-flash] session baud=%lu block=%lu\", (unsigned long)s_port.baud,\n         (unsigned long)sizeof(s_flash_buf));\n",
        "call fast baud helper",
    )

    path.write_text(text, encoding="utf-8")
    print(f"patched: {path}")
    print(f"ESP flash fast baud target: {args.baud}")
    print("Next: build and flash K210 main/test firmware that contains this patched esp_flasher.c")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
