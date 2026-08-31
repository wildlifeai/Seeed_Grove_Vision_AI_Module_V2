# jpegAE_annotate.py
# ------------------
# Burns a succinct one-line summary of the HM0360 AE registers into the
# bottom of each JPEG in a folder, reading the values from the same
# MakerNote EXIF field jpegAE-batch.py extracts for its CSV.
#
#   HM0360 AE regs:                      ->   AE  integ=376  aGain=1  dGain=107  mean=102  conv=N
#     Integration time = 376 lines
#     Analog gain = 1
#     Digital gain = 107
#     AE Mean = 102
#     AEConverged?: N
#
# Originals are never modified: each annotated image is written under a new
# name (default suffix '_AE') into a subfolder (default '<input_folder>\AN',
# override with --output_folder). A second run over the same folder skips
# files already carrying that suffix, so it cannot re-annotate its own
# output. Each output file's Windows timestamps (created/modified/accessed)
# are set to match the source file's, so it sorts alongside its original.

import os
import sys
import struct
import argparse

# Deferred to annotate_image() rather than imported here, so --help and
# argument-parsing errors work even without Pillow installed - only actually
# annotating a file requires it.

AE_HEADERS = [
    "Integration time",
    "Analog gain",
    "Digital gain",
    "AE Mean",
    "AEConverged",
]

TAG_MAKERNOTE = 0x927C
POINTER_TAGS  = {0x8769, 0x8825}   # ExifIFDPointer, GPSInfoIFDPointer

TYPE_SIZES = {
    1: 1, 2: 1, 3: 2, 4: 4,
    5: 8, 7: 1, 9: 4, 10: 8,
}


# --- MakerNote extraction (same parser as jpegAE-batch.py) -----------------
# Duplicated rather than imported: jpegAE-batch.py's hyphenated filename
# is not a valid Python module name.

def parse_makernote(text):
    """Split a MakerNote string into AE fields dict."""
    parts = [s.strip() for s in text.split(',')]
    fields = {}
    for i, header in enumerate(AE_HEADERS):
        fields[header] = parts[i] if i < len(parts) else ''
    return fields


def parse_ifd(fp, base_offset, ifd_offset, endian, collected, check_next_ifd=True):
    try:
        fp.seek(base_offset + ifd_offset)
        raw = fp.read(2)
        if len(raw) < 2:
            return
        num_entries = struct.unpack(endian + 'H', raw)[0]
    except Exception:
        return

    for _ in range(num_entries):
        entry = fp.read(12)
        if len(entry) < 12:
            return
        tag, type_id, count, value_offset = struct.unpack(endian + 'HHII', entry)
        next_entry_pos = fp.tell()
        type_size = TYPE_SIZES.get(type_id, 1)
        total_size = type_size * count

        if total_size <= 4:
            raw_bytes = struct.pack(endian + 'I', value_offset)
            value = raw_bytes[:total_size]
        else:
            cur = fp.tell()
            try:
                fp.seek(base_offset + value_offset)
                value = fp.read(total_size)
            except Exception:
                value = b''
            fp.seek(cur)

        if tag == TAG_MAKERNOTE:
            collected['makernote'] = value.decode('ascii', errors='replace').strip('\x00').strip()

        if tag in POINTER_TAGS:
            parse_ifd(fp, base_offset, value_offset, endian, collected, check_next_ifd=False)
            fp.seek(next_entry_pos)

    if check_next_ifd:
        next_ifd_bytes = fp.read(4)
        if len(next_ifd_bytes) == 4:
            next_offset = struct.unpack(endian + 'I', next_ifd_bytes)[0]
            if next_offset != 0:
                parse_ifd(fp, base_offset, next_offset, endian, collected)


def extract_makernote(filepath):
    collected = {}
    try:
        with open(filepath, 'rb') as fp:
            while True:
                byte = fp.read(1)
                if not byte:
                    break
                if byte != b'\xFF':
                    continue
                marker = fp.read(1)
                if marker in [b'\xD8', b'\xD9']:
                    continue
                length_bytes = fp.read(2)
                if len(length_bytes) < 2:
                    break
                length = struct.unpack('>H', length_bytes)[0]
                segment_start = fp.tell()
                segment_data = fp.read(length - 2)
                if marker == b'\xE1' and segment_data.startswith(b'Exif\x00\x00'):
                    endian_flag = segment_data[6:8]
                    endian = '<' if endian_flag == b'II' else '>'
                    if len(segment_data) < 14:
                        break
                    magic = struct.unpack(endian + 'H', segment_data[8:10])[0]
                    if magic != 0x2A:
                        break
                    first_ifd_offset = struct.unpack(endian + 'I', segment_data[10:14])[0]
                    tiff_offset = segment_start + 6
                    parse_ifd(fp, tiff_offset, first_ifd_offset, endian, collected)
                    break
    except Exception:
        pass
    return collected.get('makernote', '')


# --- Annotation --------------------------------------------------------

def format_ae_line(fields):
    """Turn the 5 named AE fields into one succinct line, or a placeholder
    if the MakerNote was empty/unparseable."""
    if not any(fields.values()):
        return "AE: no MakerNote data"
    return (
        f"AE  integ={fields['Integration time']}  "
        f"aGain={fields['Analog gain']}  "
        f"dGain={fields['Digital gain']}  "
        f"mean={fields['AE Mean']}  "
        f"conv={fields['AEConverged']}"
    )


def annotate_image(src_path, out_path, text, quality, margin):
    from PIL import Image, ImageDraw, ImageFont

    img = Image.open(src_path)
    orig_exif = img.info.get("exif")

    if img.mode not in ("RGB", "L"):
        img = img.convert("RGB")

    font_size = max(14, img.height // 24)
    try:
        font = ImageFont.load_default(size=font_size)
    except TypeError:
        # Pillow < 9.2 has no 'size' arg on load_default() - falls back to
        # its small fixed-size bitmap font.
        font = ImageFont.load_default()

    draw = ImageDraw.Draw(img)
    bbox = draw.textbbox((0, 0), text, font=font)
    text_h = bbox[3] - bbox[1]
    x = margin
    y = img.height - text_h - margin

    # Black outline + white fill so the line reads on both bright daylight
    # colour frames and dark IR/night frames without needing a background bar.
    for dx, dy in ((-1, -1), (-1, 1), (1, -1), (1, 1), (-1, 0), (1, 0), (0, -1), (0, 1)):
        draw.text((x + dx, y + dy), text, font=font, fill="black")
    draw.text((x, y), text, font=font, fill="white")

    save_kwargs = {"quality": quality}
    if orig_exif:
        # Keep the original EXIF (including the MakerNote) on the annotated
        # copy too - the burned-in text is a convenience, not a replacement.
        save_kwargs["exif"] = orig_exif
    img.save(out_path, "JPEG", **save_kwargs)


def copy_timestamps(src_path, dst_path):
    """Make the output file's Windows Explorer timestamps match the source's,
    so annotated copies sort/display the same as the originals they came from."""
    st = os.stat(src_path)
    os.utime(dst_path, (st.st_atime, st.st_mtime))  # 'Date accessed'/'Date modified' - portable

    if sys.platform == "win32":
        # 'Date created' has no portable stdlib API - set it via a raw
        # SetFileTime() call (no extra dependency beyond ctypes/kernel32,
        # both part of a standard Windows Python install). st_ctime IS the
        # creation time on Windows (unlike Unix, where it means something else).
        import ctypes
        from ctypes import wintypes

        FILE_WRITE_ATTRIBUTES = 0x100
        OPEN_EXISTING = 3
        FILE_ATTRIBUTE_NORMAL = 0x80
        WINDOWS_EPOCH_OFFSET_100NS = 116444736000000000  # 1601-01-01 -> 1970-01-01

        handle = ctypes.windll.kernel32.CreateFileW(
            dst_path, FILE_WRITE_ATTRIBUTES, 0, None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None
        )
        if handle and handle != -1:
            try:
                filetime = int(st.st_ctime * 10000000) + WINDOWS_EPOCH_OFFSET_100NS
                ft = wintypes.FILETIME(filetime & 0xFFFFFFFF, filetime >> 32)
                ctypes.windll.kernel32.SetFileTime(handle, ctypes.byref(ft), None, None)
            finally:
                ctypes.windll.kernel32.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Burn a succinct AE-register summary (from the MakerNote EXIF field) into\n"
            "the bottom of each JPEG in a folder. Originals are left untouched; each\n"
            "annotated image is written under a new name (default: '<name>_AE.jpg') into\n"
            "'<input_folder>\\AN' (default) or --output_folder."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python jpegAE_annotate.py --input_folder D:\\images\n"
            "  python jpegAE_annotate.py --input_folder . --output_folder annotated --recursive"
        ),
    )
    parser.add_argument('--input_folder', required=True, help='Folder of JPEGs to annotate')
    parser.add_argument('--output_folder', help="Write annotated files here (default: '<input_folder>\\AN')")
    parser.add_argument('--suffix', default='_AE', help="Inserted before the extension (default '_AE')")
    parser.add_argument('--quality', type=int, default=92, help='JPEG save quality (default 92)')
    parser.add_argument('--margin', type=int, default=8, help='Pixels from the left/bottom edge (default 8)')
    parser.add_argument('--recursive', action='store_true', help='Also descend into subfolders')
    parser.add_argument('--overwrite', action='store_true', help='Re-annotate even if the output file already exists')
    args = parser.parse_args()

    try:
        import PIL  # noqa: F401
    except ImportError:
        parser.error("Pillow is required to annotate images - install it with 'pip install pillow'.")

    output_folder = args.output_folder or os.path.join(args.input_folder, 'AN')
    os.makedirs(output_folder, exist_ok=True)
    output_folder_abs = os.path.abspath(output_folder)

    def source_files():
        if args.recursive:
            for root, dirs, files in os.walk(args.input_folder):
                # Never descend into the output folder itself (it lives inside
                # input_folder by default) - that would re-process its own output.
                dirs[:] = [d for d in dirs if os.path.abspath(os.path.join(root, d)) != output_folder_abs]
                for name in sorted(files):
                    yield root, name
        else:
            for name in sorted(os.listdir(args.input_folder)):
                if os.path.isfile(os.path.join(args.input_folder, name)):
                    yield args.input_folder, name

    processed = 0
    skipped = 0
    failed = 0

    for root, name in source_files():
        if not name.lower().endswith(('.jpg', '.jpeg')):
            continue

        base, ext = os.path.splitext(name)
        if base.endswith(args.suffix):
            continue  # a previous run's output - never re-annotate it

        src_path = os.path.join(root, name)

        rel_dir = os.path.relpath(root, args.input_folder)
        dest_dir = output_folder if rel_dir == '.' else os.path.join(output_folder, rel_dir)
        os.makedirs(dest_dir, exist_ok=True)

        out_path = os.path.join(dest_dir, f"{base}{args.suffix}{ext}")

        if os.path.exists(out_path) and not args.overwrite:
            skipped += 1
            continue

        makernote = extract_makernote(src_path)
        fields = parse_makernote(makernote)
        text = format_ae_line(fields)

        try:
            annotate_image(src_path, out_path, text, args.quality, args.margin)
            copy_timestamps(src_path, out_path)
            processed += 1
        except Exception as e:
            print(f"FAILED: {name}: {e}")
            failed += 1

    print(f"Annotated {processed} file(s), skipped {skipped} (already done), {failed} failed.")
    print(f"Output: {output_folder}")


if __name__ == "__main__":
    main()
