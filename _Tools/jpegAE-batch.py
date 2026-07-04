# jpegAE-batch.py
# ---------------
# Scans a folder for JPEG files and extracts auto-exposure register values
# from the MakerNote EXIF field.
#
# The MakerNote is expected to contain comma-separated ASCII values in this order:
#   Integration time, Analog gain, Digital gain, AE Mean, AEConverged
# Any additional comma-separated values beyond the first five are written as
# Extra_1, Extra_2, ... columns.

import os
import csv
import struct
import argparse
from datetime import datetime

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


def parse_makernote(text):
    """Split a MakerNote string into AE fields dict; return (dict, n_extras)."""
    parts = [s.strip() for s in text.split(',')]
    fields = {}
    for i, header in enumerate(AE_HEADERS):
        fields[header] = parts[i] if i < len(parts) else ''
    extras = parts[len(AE_HEADERS):]
    for i, val in enumerate(extras, start=1):
        fields[f"Extra_{i}"] = val
    return fields, len(extras)


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


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Batch auto-exposure data extractor for WW500 JPEG files.\n"
            "\n"
            "Reads the MakerNote EXIF field from each JPEG and parses it as\n"
            "comma-separated auto-exposure register values. The first 5 fields\n"
            "map to named AE registers; any additional values appear as Extra_N.\n"
            "\n"
            "MakerNote field order:\n"
            "  1. Integration time\n"
            "  2. Analog gain\n"
            "  3. Digital gain\n"
            "  4. AE Mean\n"
            "  5. AEConverged\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python jpegAE-batch.py --input_folder . --output_csv ae.csv\n"
            "  python jpegAE-batch.py --input_folder D:\\images --output_csv ae.csv --output_txt ae.txt"
        ),
    )
    parser.add_argument('--input_folder', required=True,
                        help='Folder to scan for JPEG files (recursive)')
    parser.add_argument('--output_csv', required=True,
                        help='Output CSV file path')
    parser.add_argument('--output_txt', required=False,
                        help='Optional TXT log file path')
    args = parser.parse_args()

    rows = []
    log_entries = []
    max_extras = 0

    for root, _, files in os.walk(args.input_folder):
        for name in sorted(files):
            if not name.lower().endswith(('.jpg', '.jpeg')):
                continue
            path = os.path.join(root, name)

            try:
                filetime = datetime.fromtimestamp(os.path.getmtime(path)).strftime('%Y-%m-%d %H:%M')
            except Exception:
                filetime = ''

            makernote = extract_makernote(path)
            ae_fields, n_extras = parse_makernote(makernote)
            if n_extras > max_extras:
                max_extras = n_extras

            row = {"FileName": name, "FileTime": filetime}
            row.update(ae_fields)
            rows.append(row)

            if args.output_txt:
                log_entries.append(f"File: {name}  ({filetime})")
                for k, v in ae_fields.items():
                    log_entries.append(f"  {k}: {v}")
                log_entries.append("")

    extra_headers = [f"Extra_{i}" for i in range(1, max_extras + 1)]
    fieldnames = ["FileName", "FileTime"] + AE_HEADERS + extra_headers

    with open(args.output_csv, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    if args.output_txt:
        with open(args.output_txt, 'w', encoding='utf-8') as f:
            for entry in log_entries:
                f.write(entry + "\n")

    print(f"Processed {len(rows)} file(s) -> {args.output_csv}")
    if args.output_txt:
        print(f"Log written -> {args.output_txt}")


if __name__ == "__main__":
    main()
