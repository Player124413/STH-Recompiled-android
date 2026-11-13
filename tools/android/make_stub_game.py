#!/usr/bin/env python3
"""Writes a stand-in for the recompiler's output, so the port can be built and tested without the game.

`xbe_recomp` writes generated/<image>/ from the player's own disc: the game's code, and the headers the SDK's runtime
reads (image_symbols.h with the addresses config/game.toml names). Nothing of that can be committed. This writes a
folder of the same shape with no game code in it: the symbol addresses from the config, an empty function table, and
an entry point of 0, which no XBE has. A build on top of it links and installs, starts, and stops at "game not found"
-- enough to prove the whole Android build (SDK runtime, renderer, ImGui, Java side) works before the game is added.

usage: make_stub_game.py [--config config/game.toml] [--out generated/game]
"""
import argparse
import os
import re
import sys

SECTION = re.compile(r"^\[+([^\]]+)\]+\s*$")  # [symbols] and [[hook]] alike
ENTRY = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*(?:#.*)?$")
HEX = re.compile(r"^0x([0-9A-Fa-f]+)$")


def clean(value):
    value = value.strip()
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    return value


def parse(path):
    """the config's [xbe], [symbols], [data], [layout] and [abi] as the recompiler reads them"""
    out = {"xbe": {}, "symbols": [], "data": [], "layout": [], "abi": []}
    section = ""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = SECTION.match(line.strip())
            if m:
                section = m.group(1)
                continue
            m = ENTRY.match(line.strip())
            if not m or section not in out:
                continue
            name, value = m.group(1), clean(m.group(2))
            if section == "xbe":
                out["xbe"][name] = value
                continue
            if section == "abi":
                # name = [eax, stack, ...]
                locs = [x.strip() for x in value.strip("[]").split(",") if x.strip()]
                out["abi"].append((name, locs))
                continue
            try:
                value = int(value, 0)  # 0: the image has no such symbol
            except ValueError:
                continue  # not an address (a string or a list): nothing for this header

            (out["layout"] if section == "layout" else out[section]).append((name, value))
    return out


def image_symbols(cfg):
    """the same shape xbe_recomp writes (sdk/tools/recomp/codegen_emit.cpp, Run)"""
    text = ["#pragma once", "// written by tools/android/make_stub_game.py: no game code, only the config's symbols"]
    xbe = os.path.basename(cfg["xbe"].get("path", "default.xbe")) or "default.xbe"
    text.append('#define IMAGE_XBE "%s"' % xbe)
    absent = 0
    for name, value in cfg["symbols"]:
        if not value:  # the image has none: a placeholder the host's override still links against
            value = 0xFFFF0000 + absent
            absent += 1
        text.append("#define SYM_%s %08X" % (name, value))
        text.append("#define HAS_%s %d" % (name, 1 if value < 0xFFFF0000 else 0))
    for name, value in cfg["data"]:
        text.append("#define SYM_%s %08X" % (name, value))
        text.append("#define HAS_%s 1" % name)
    for name, value in cfg["layout"]:
        text.append("#define XDK_%s 0x%X" % (name, value))
    if cfg["abi"]:
        rows = ["#define IMAGE_ABI(X) \\"] + ["\tX(%s, %s)" % (n, ", ".join(l)) + (" \\" if i < len(cfg["abi"]) - 1 else "")
                                              for i, (n, l) in enumerate(cfg["abi"])]
        text.extend(rows)
    text.append("")
    return "\n".join(text)


def func_mapping(cfg):
    """no functions at all: the tables the dispatcher reads, empty"""
    entry = cfg["xbe"].get("entry", "0")
    entry = int(entry, 16) if entry.lower().startswith("0x") else 0
    return "\n".join([
        "#include \"cpu/x86_ops.h\"",
        "",
        "// written by tools/android/make_stub_game.py: the game's code is not in this build",
        "const uint32_t X86CodeBase = 0x10000u;",
        "const uint32_t X86CodeEnd = 0x10000u;",
        "const uint32_t X86ImageBase = 0x10000u;",
        "const uint32_t X86ImageEnd = 0x10000u;",
        "const uint32_t X86EntryPoint = 0x%Xu;  // the config's entry: no image carries it, so nothing runs" % entry,
        "const uint32_t X86KernelThunk = 0x0u;",
        "",
        "const X86FuncMapping X86FuncMappings[] = {",
        "\t{0, nullptr},",
        "};",
        "",
        "const uint32_t X86Continuations[] = {",
        "\t0,",
        "};",
        "",
    ])


def main():
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=os.path.join(here, "config", "game.toml"))
    ap.add_argument("--out", default=os.path.join(here, "generated", "game"))
    args = ap.parse_args()
    if not os.path.exists(args.config):
        sys.exit("%s: no such config" % args.config)
    cfg = parse(args.config)
    if not cfg["symbols"]:
        sys.exit("%s: no [symbols] section" % args.config)
    os.makedirs(args.out, exist_ok=True)
    files = {
        "image_symbols.h": image_symbols(cfg),
        "x86_func_mapping.cpp": func_mapping(cfg),
        "x86_recomp_shared.h": "#pragma once\n// written by tools/android/make_stub_game.py: there is no recompiled code\n",
        "hook_exports.h": "#pragma once\n// written by tools/android/make_stub_game.py: there are no hooks to export\n",
    }
    for name, text in files.items():
        with open(os.path.join(args.out, name), "w", encoding="utf-8") as f:
            f.write(text)
    # the build reads this to know it has no game code (CMakeLists.txt)
    with open(os.path.join(args.out, "stub.txt"), "w", encoding="utf-8") as f:
        f.write("no game code: generated by tools/android/make_stub_game.py\n")
    print("stub game written to %s: %d symbols, %d data, %d argument maps" %
          (args.out, len(cfg["symbols"]), len(cfg["data"]), len(cfg["abi"])))


if __name__ == "__main__":
    main()
