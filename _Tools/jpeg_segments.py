#!/usr/bin/env python3
"""
jpeg_segments.py
----------------
Parses JPEG files and prints a breakdown of every segment block with its
size, plus a summary comparison that explains why black images are smaller
than visibly-exposed ones.

Usage:
  python3 jpeg_segments.py <folder>              # all JPEGs in folder
  python3 jpeg_segments.py <file> [file2 ...]   # specific files
  python3 jpeg_segments.py img/ extra.jpg        # mix of folders and files
"""

import struct
import math
import sys
import os
import glob
import argparse

# ---------------------------------------------------------------------------
# Marker tables
# ---------------------------------------------------------------------------

MARKER_NAMES = {
    0xD8: 'SOI',   # Start Of Image          (no length)
    0xD9: 'EOI',   # End Of Image            (no length)
    0xDA: 'SOS',   # Start Of Scan           (followed by entropy-coded data)
    0xDB: 'DQT',   # Define Quantization Table
    0xC0: 'SOF0',  # Start Of Frame (baseline DCT)
    0xC2: 'SOF2',  # Start Of Frame (progressive DCT)
    0xC4: 'DHT',   # Define Huffman Table
    0xDD: 'DRI',   # Define Restart Interval
    0xFE: 'COM',   # Comment
}

def marker_name(b):
    if b in MARKER_NAMES:
        return MARKER_NAMES[b]
    if 0xE0 <= b <= 0xEF:
        return 'APP%d' % (b & 0x0F)
    if 0xD0 <= b <= 0xD7:
        return 'RST%d' % (b & 0x07)
    return '0x%02X' % b

# Markers that carry NO length field
NO_LENGTH = {0xD8, 0xD9} | set(range(0xD0, 0xD8))

# ---------------------------------------------------------------------------
# Entropy / statistics helper
# ---------------------------------------------------------------------------

def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    h = 0.0
    for c in counts:
        if c:
            p = c / n
            h -= p * math.log2(p)
    return h

# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def parse_jpeg(path):
    """
    Returns a list of segment dicts:
      type, name, offset, header_size, data_size, total_size, extra
    plus a special 'ECD' entry for entropy-coded data after SOS.
    """
    with open(path, 'rb') as f:
        data = f.read()

    segments = []
    pos = 0
    n = len(data)

    while pos < n:
        # Find next 0xFF
        ff = data.find(b'\xFF', pos)
        if ff == -1:
            break

        if ff > pos:
            # Gap / trailing bytes not inside a known segment
            gap = ff - pos
            segments.append({
                'type': 'GAP', 'name': 'GAP', 'offset': pos,
                'header_size': 0, 'data_size': gap, 'total_size': gap,
                'extra': '',
            })
            pos = ff
            continue

        if ff + 1 >= n:
            break

        marker_byte = data[ff + 1]

        if marker_byte == 0x00:
            # Byte-stuffing inside ECD – skip
            pos = ff + 1
            continue

        if marker_byte == 0xFF:
            # Padding – skip one byte
            pos = ff + 1
            continue

        name = marker_name(marker_byte)

        if marker_byte in NO_LENGTH:
            segments.append({
                'type': 'MARKER', 'name': name, 'offset': ff,
                'header_size': 2, 'data_size': 0, 'total_size': 2,
                'extra': '',
            })
            pos = ff + 2

            if marker_byte == 0xD9:
                # EOI – anything after is trailing
                trailing = n - pos
                if trailing > 0:
                    segments.append({
                        'type': 'TRAILING', 'name': 'TRAILING', 'offset': pos,
                        'header_size': 0, 'data_size': trailing,
                        'total_size': trailing, 'extra': '',
                    })
                break
            continue

        # Regular segment: 2-byte length (includes the 2 length bytes)
        if ff + 3 >= n:
            break
        seg_len = struct.unpack('>H', data[ff + 2: ff + 4])[0]
        payload = data[ff + 4: ff + 2 + seg_len]   # excludes marker, includes length bytes' data

        extra = ''
        if marker_byte == 0xDB:
            tables = (seg_len - 2) // 65
            extra = '%d quantization table(s)' % tables
        elif marker_byte == 0xC0 and len(payload) >= 5:
            prec, h, w, comp = struct.unpack('>BHHB', payload[:6])
            extra = '%dx%d px, %d component(s), %d-bit' % (w, h, comp, prec)
        elif marker_byte == 0xC4:
            extra = 'Huffman table'
        elif marker_byte == 0xDA and len(payload) >= 1:
            comp = payload[0]
            extra = '%d component(s)' % comp

        segments.append({
            'type': 'SEGMENT', 'name': name, 'offset': ff,
            'header_size': 4,          # marker (2) + length (2)
            'data_size': seg_len - 2,  # payload only
            'total_size': 2 + seg_len, # marker + length + payload
            'extra': extra,
        })
        pos = ff + 2 + seg_len

        # After SOS header, scan entropy-coded data up to next real marker
        if marker_byte == 0xDA:
            ecd_start = pos
            ff00_count = 0
            while pos < n - 1:
                next_ff = data.find(b'\xFF', pos)
                if next_ff == -1 or next_ff >= n - 1:
                    pos = n
                    break
                next_byte = data[next_ff + 1]
                if next_byte == 0x00:
                    ff00_count += 1
                    pos = next_ff + 2
                elif 0xD0 <= next_byte <= 0xD7:
                    # RST marker inside ECD – keep scanning
                    pos = next_ff + 2
                else:
                    pos = next_ff
                    break

            ecd_data = data[ecd_start:pos]
            ecd_entropy = entropy(ecd_data)
            segments.append({
                'type': 'ECD', 'name': 'ECD',
                'offset': ecd_start,
                'header_size': 0,
                'data_size': len(ecd_data),
                'total_size': len(ecd_data),
                'extra': 'entropy=%.2f  ff00_stuffing=%d' % (ecd_entropy, ff00_count),
                '_entropy': ecd_entropy,
                '_ff00': ff00_count,
            })

    return segments, len(data)

# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

COL_WIDTHS = (8, 6, 10, 12, 10, 10, 36)
HEADER = ('Segment', 'Offset', 'Hdr bytes', 'Data bytes', 'Total', '% of file', 'Notes')

def print_file_report(path, segments, file_size):
    print()
    print('=' * 80)
    print('File: %s   (%d bytes / %.1f KB)' % (path, file_size, file_size / 1024))
    print('=' * 80)

    fmt = '%-8s  %8s  %9s  %10s  %8s  %8s  %s'
    print(fmt % HEADER)
    print('-' * 80)

    for seg in segments:
        pct = 100.0 * seg['total_size'] / file_size if file_size else 0
        print(fmt % (
            seg['name'],
            '0x%05X' % seg['offset'],
            seg['header_size'] if seg['header_size'] else '-',
            seg['data_size'],
            seg['total_size'],
            '%.1f%%' % pct,
            seg['extra'],
        ))

    # Totals
    total_seg = sum(s['total_size'] for s in segments)
    print('-' * 80)
    print('%-8s  %8s  %9s  %10s  %8d  %8s' % (
        'TOTAL', '', '', '', total_seg, '100%'))

    # ECD summary
    ecd = next((s for s in segments if s['type'] == 'ECD'), None)
    if ecd:
        print()
        print('  Image data (ECD): %d bytes,  entropy=%.2f,  ff00_stuffing=%d' % (
            ecd['data_size'], ecd.get('_entropy', 0), ecd.get('_ff00', 0)))
        if ecd.get('_entropy', 0) < 4.0:
            print('  *** Low entropy – image data is highly uniform (e.g. solid black)')

# ---------------------------------------------------------------------------
# Cross-file comparison
# ---------------------------------------------------------------------------

def print_comparison(results):
    if len(results) < 2:
        return

    print()
    print('=' * 80)
    print('COMPARISON SUMMARY')
    print('=' * 80)

    fmt = '%-20s  %8s  %10s  %10s  %7s  %s'
    print(fmt % ('File', 'Size(KB)', 'ECD bytes', 'ECD entropy', 'ff00', 'Segment totals match file?'))
    print('-' * 80)

    for path, segments, file_size in results:
        ecd = next((s for s in segments if s['type'] == 'ECD'), None)
        ecd_bytes   = ecd['data_size']    if ecd else 0
        ecd_ent     = ecd.get('_entropy', 0) if ecd else 0.0
        ff00        = ecd.get('_ff00', 0) if ecd else 0
        total_segs  = sum(s['total_size'] for s in segments)
        match       = 'yes' if total_segs == file_size else 'NO (%d gap)' % (file_size - total_segs)
        print(fmt % (
            os.path.basename(path),
            '%.1f' % (file_size / 1024),
            ecd_bytes,
            '%.2f' % ecd_ent,
            ff00,
            match,
        ))

    print()
    print('Key:')
    print('  ECD bytes   = entropy-coded image data (the actual pixel data, JPEG-compressed)')
    print('  ECD entropy = information density (7-8 = normal photo, <4 = very uniform/black)')
    print('  ff00        = byte-stuffing count (0 in a black image = no 0xFF bytes in data)')

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

JPEG_EXTENSIONS = {'.jpg', '.jpeg', '.JPG', '.JPEG'}

def collect_paths(args):
    """Expand folders to JPEG files; pass individual files through."""
    paths = []
    for arg in args:
        if os.path.isdir(arg):
            found = sorted(
                p for p in (os.path.join(arg, f) for f in os.listdir(arg))
                if os.path.isfile(p) and os.path.splitext(p)[1] in JPEG_EXTENSIONS
            )
            if not found:
                print('Warning: no JPEG files found in folder %s' % arg)
            paths.extend(found)
        else:
            expanded = glob.glob(arg)
            paths.extend(sorted(expanded) if expanded else [arg])
    return paths

def main():
    parser = argparse.ArgumentParser(
        prog='jpeg_segments.py',
        description='Parse JPEG files and print a breakdown of segment blocks with sizes.',
        epilog=(
            'examples:\n'
            '  %(prog)s img/               analyse all JPEGs in a folder\n'
            '  %(prog)s img/*.JPG          use a glob pattern\n'
            '  %(prog)s a.jpg b.jpg img/   mix of files and folders'
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        'targets', nargs='+', metavar='FILE_OR_DIR',
        help='JPEG file(s) or folder(s) to analyse',
    )
    args = parser.parse_args()

    paths = collect_paths(args.targets)
    if not paths:
        print('Error: no JPEG files found.')
        sys.exit(1)

    results = []
    for path in paths:
        try:
            segments, file_size = parse_jpeg(path)
            print_file_report(path, segments, file_size)
            results.append((path, segments, file_size))
        except Exception as e:
            print('ERROR reading %s: %s' % (path, e))

    if len(results) > 1:
        print_comparison(results)

if __name__ == '__main__':
    main()
