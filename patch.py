import hashlib
from typing import TYPE_CHECKING, Any
import requests

if TYPE_CHECKING:
    Import: Any = None
    env: Any = {}

import glob
import gzip
import subprocess
from os import makedirs, remove, rename
from os.path import basename, dirname, exists, isfile, join

Import("env")  # type: ignore

FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
board_mcu = env.BoardConfig()
mcu = board_mcu.get("build.mcu", "")
lib_dir = join(FRAMEWORK_DIR, mcu, "lib")
patchflag_path = join(lib_dir, ".patched")


def _find_tool(name, package):
    """Return the full path of a tool from a PlatformIO package, or None."""
    try:
        pkg_dir = env.PioPlatform().get_package_dir(package)
    except Exception:
        return None
    if not pkg_dir:
        return None
    for candidate in (name + ".exe", name):
        path = join(pkg_dir, "bin", candidate)
        if isfile(path):
            return path
    return None


def _find_objcopy():
    if mcu.startswith("esp32c") or mcu in ("esp32p4", "esp32h2"):
        package = "toolchain-riscv32-esp"
        names = ["riscv32-esp-elf-objcopy"]
    else:
        package = "toolchain-xtensa-esp-elf"
        # Prefer the target-specific binary, fall back to the generic one.
        names = ["xtensa-%s-elf-objcopy" % mcu, "xtensa-esp-elf-objcopy"]
    for name in names:
        path = _find_tool(name, package)
        if path:
            return path
    return None


def _symbol_is_weak(library):
    """True when the deauth override symbol is already weak in the archive."""
    nm = _find_tool("xtensa-%s-elf-nm" % mcu, "toolchain-xtensa-esp-elf") or _find_tool(
        "riscv32-esp-elf-nm", "toolchain-riscv32-esp"
    )
    if not nm:
        return False
    try:
        out = subprocess.run(
            [nm, "-A", library], capture_output=True, text=True, timeout=120
        ).stdout
    except Exception:
        return False
    for line in out.splitlines():
        if "ieee80211_raw_frame_sanity_check" not in line:
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in ("W", "w", "V", "v"):
            return True
    return False


def _patch_net80211():
    original_file = join(lib_dir, "libnet80211.a")
    patched_file = join(lib_dir, "libnet80211.a.patched")
    old_file = original_file + ".old"

    # A previous broken run could leave only the .old copy behind.
    if not isfile(original_file) and isfile(old_file):
        rename(old_file, original_file)
        print("Patch: restored libnet80211.a left behind by an earlier run")

    if not isfile(original_file):
        print("Patch: libnet80211.a not found, skipping symbol patch")
        return

    # The flag is only written after a verified patch, but validate it anyway
    # so a manually restored library is re-patched instead of breaking the link.
    if isfile(patchflag_path) and _symbol_is_weak(original_file):
        return
    if isfile(patchflag_path):
        remove(patchflag_path)

    objcopy = _find_objcopy()
    if not objcopy:
        print("Patch: objcopy not found, keeping original libnet80211.a")
        return

    try:
        result = subprocess.run(
            [
                objcopy,
                "--weaken-symbol=ieee80211_raw_frame_sanity_check",
                original_file,
                patched_file,
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except Exception as exc:
        print("Patch: objcopy failed: %s" % exc)
        return

    if result.returncode != 0 or not isfile(patched_file):
        print(
            "Patch: objcopy produced nothing, keeping the original library: %s"
            % (result.stderr or result.stdout or "unknown error").strip()
        )
        if isfile(patched_file):
            remove(patched_file)
        return

    # Only swap the library in once objcopy actually produced a patched copy.
    if isfile(old_file):
        remove(old_file)
    rename(original_file, old_file)
    rename(patched_file, original_file)
    remove(old_file)

    with open(patchflag_path, "w") as fp:
        fp.write("")
    print("Patch: weakened ieee80211_raw_frame_sanity_check in libnet80211.a")


_patch_net80211()


def hash_file(file_path):
    """Generate SHA-256 hash for a single file."""
    hasher = hashlib.sha256()
    with open(file_path, "rb") as f:
        # Read the file in chunks to avoid memory issues
        for chunk in iter(lambda: f.read(4096), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def hash_files(file_paths):
    """Generate a combined hash for multiple files."""
    combined_hash = hashlib.sha256()

    for file_path in file_paths:
        file_hash = hash_file(file_path)
        combined_hash.update(file_hash.encode("utf-8"))  # Update with the file's hash

    return combined_hash.hexdigest()


def save_checksum_file(hash_value, output_file):
    """Save the hash value to a specified output file."""
    with open(output_file, "w") as f:
        f.write(hash_value)


def load_checksum_file(input_file):
    """Load the hash value from a specified input file."""
    with open(input_file, "r") as f:
        return f.readline().strip()


def minify_css(c):
    minify_req = requests.post(
        "https://www.toptal.com/developers/cssminifier/api/raw",
        {"input": c.read().decode('utf-8')},
    )
    return c if minify_req is False else minify_req.text.encode('utf-8')


def minify_js(js):
    minify_req = requests.post(
        'https://www.toptal.com/developers/javascript-minifier/api/raw',
        {'input': js.read().decode('utf-8')},
    )
    return js if minify_req is False else minify_req.text.encode('utf-8')


def minify_html(html):
    minify_req = requests.post(
        'https://www.toptal.com/developers/html-minifier/api/raw',
        {'input': html.read().decode('utf-8')},
    )
    return html if minify_req is False else minify_req.text.encode('utf-8')


# gzip web files
def prepare_www_files():
    HEADER_FILE = join(env.get("PROJECT_DIR"), "include", "webFiles.h")
    filetypes_to_gzip = ["html", "css", "js"]
    data_src_dir = join(env.get("PROJECT_DIR"), "embedded_resources/web_interface")
    checksum_file = join(data_src_dir, "checksum.sha256")
    checksum = ""

    if not exists(data_src_dir):
        print(f'Error: Source directory "{data_src_dir}" does not exist!')
        return

    if exists(checksum_file):
        checksum = load_checksum_file(checksum_file)

    files_to_gzip = []
    for extension in filetypes_to_gzip:
        files_to_gzip.extend(glob.glob(join(data_src_dir, "*." + extension)))

    files_checksum = hash_files(files_to_gzip)
    if files_checksum == checksum:
        print("[GZIP & EMBED INTO HEADER] - Nothing to process.")
        return

    print(f"[GZIP & EMBED INTO HEADER] - Processing {len(files_to_gzip)} files.")

    makedirs(dirname(HEADER_FILE), exist_ok=True)

    with open(HEADER_FILE, "w") as header:
        header.write(
            "#ifndef WEB_FILES_H\n#define WEB_FILES_H\n\n#include <Arduino.h>\n\n"
        )
        header.write(
            "// THIS FILE IS AUTOGENERATED DO NOT MODIFY IT. MODIFY FILES IN /embedded_resources/web_interface\n\n"
        )

        for file in files_to_gzip:
            gz_file = file + ".gz"
            with open(file, "rb") as src, gzip.open(gz_file, "wb") as dst:
                ext = basename(file).rsplit(".", 1)[-1].lower()
                if ext == 'html':
                    minified = minify_html(src)
                elif ext == 'css':
                    minified = minify_css(src)
                elif ext == 'js':
                    minified = minify_js(src)
                else:
                    raise ValueError(f"Unsupported file type: {ext}")

                # # Output minified file
                # min_file = file + ".min"
                # with open(min_file, "wb") as minf:
                #     minf.write(minified)

                dst.write(minified)

            with open(gz_file, "rb") as gz:
                compressed_data = gz.read()
                var_name = basename(file).replace(".", "_")

                header.write(f"const uint8_t {var_name}[] PROGMEM = {{\n")

                # Write hex values, inserting a newline every 15 bytes
                for i in range(0, len(compressed_data), 15):
                    hex_chunk = ", ".join(
                        f"0x{byte:02X}" for byte in compressed_data[i : i + 15]
                    )
                    header.write(f"  {hex_chunk},\n")

                header.write("};\n\n")
                header.write(
                    f"const uint32_t {var_name}_size = {len(compressed_data)};\n\n"
                )

            remove(gz_file)  # Clean up temporary gzip file

        header.write("#endif // WEB_FILES_H\n")

    save_checksum_file(files_checksum, checksum_file)

    print(f"[DONE] Gzipped files embedded into {HEADER_FILE}")


prepare_www_files()
