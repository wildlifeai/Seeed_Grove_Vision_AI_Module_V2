#!/usr/bin/env python3
"""
jpeg_decode_analyse.py
----------------------
Fully decodes one or more JPEG files (baseline DCT only) and reports:
  - Per-component pixel statistics (min / max / mean)
  - Approximate RGB from YCbCr means
  - Y-channel brightness histogram
  - Per-row anomaly scan (flags rows that deviate from the dominant range)
  - Zoomed ASCII pixel map of any anomalous region found

No third-party libraries required.

Usage:
  python3 jpeg_decode_analyse.py <folder>
  python3 jpeg_decode_analyse.py <file.jpg> [file2.jpg ...]
  python3 jpeg_decode_analyse.py img/ extra.jpg
"""

import argparse
import glob
import math
import os
import struct
import sys

# ---------------------------------------------------------------------------
# JPEG parsing helpers
# ---------------------------------------------------------------------------

JPEG_EXTENSIONS = {'.jpg', '.jpeg', '.JPG', '.JPEG'}

ZIGZAG = [
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
]


def build_huffman(counts, huffval):
    table = {}
    code = 0
    idx = 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[(code, length)] = huffval[idx]
            idx += 1
            code += 1
        code <<= 1
    return table


def parse_dht(data):
    """Return dict (tc, th) -> huffman_table from all DHT segments in data."""
    tables = {}
    pos = 0
    while True:
        ff = data.find(b'\xff\xc4', pos)
        if ff == -1:
            break
        seg_len = struct.unpack('>H', data[ff + 2:ff + 4])[0]
        payload = data[ff + 4:ff + 2 + seg_len]
        offset = 0
        while offset < len(payload):
            tc_th = payload[offset]; offset += 1
            tc = (tc_th >> 4) & 0xF
            th = tc_th & 0xF
            counts = list(payload[offset:offset + 16]); offset += 16
            total = sum(counts)
            huffval = list(payload[offset:offset + total]); offset += total
            tables[(tc, th)] = build_huffman(counts, huffval)
        pos = ff + 2 + seg_len
    return tables


def parse_dqt(data):
    """Return dict tq -> [64 quant values] from all DQT segments in data."""
    tables = {}
    pos = 0
    while True:
        ff = data.find(b'\xff\xdb', pos)
        if ff == -1:
            break
        seg_len = struct.unpack('>H', data[ff + 2:ff + 4])[0]
        payload = data[ff + 4:ff + 2 + seg_len]
        offset = 0
        while offset < len(payload):
            pq_tq = payload[offset]; offset += 1
            tq = pq_tq & 0xF
            pq = (pq_tq >> 4) & 0xF
            if pq == 0:
                tables[tq] = list(payload[offset:offset + 64]); offset += 64
            else:
                tables[tq] = list(struct.unpack('>64H', payload[offset:offset + 128])); offset += 128
        pos = ff + 2 + seg_len
    return tables


def parse_sof0(data):
    """Return (width, height, components) where components is a list of dicts."""
    ff = data.find(b'\xff\xc0')
    if ff == -1:
        raise ValueError('SOF0 marker not found — progressive or non-baseline JPEG not supported')
    seg_len = struct.unpack('>H', data[ff + 2:ff + 4])[0]
    sof = data[ff + 4:ff + 2 + seg_len]
    prec, height, width, ncomp = struct.unpack('>BHHB', sof[:6])
    components = []
    for i in range(ncomp):
        cid, sf, qtid = struct.unpack('BBB', sof[6 + i * 3:9 + i * 3])
        components.append({'id': cid, 'h': (sf >> 4) & 0xF, 'v': sf & 0xF, 'qt': qtid})
    return width, height, components


def parse_sos(data):
    """Return list of scan component dicts {id, dc, ac}."""
    ff = data.find(b'\xff\xda')
    if ff == -1:
        raise ValueError('SOS marker not found')
    seg_len = struct.unpack('>H', data[ff + 2:ff + 4])[0]
    sos = data[ff + 4:ff + 2 + seg_len]
    scan_comps = []
    for i in range(sos[0]):
        cid = sos[1 + i * 2]
        tbl = sos[2 + i * 2]
        scan_comps.append({'id': cid, 'dc': (tbl >> 4) & 0xF, 'ac': tbl & 0xF})
    ecd_start = ff + 2 + seg_len
    return scan_comps, ecd_start


def unstuff_ecd(data, ecd_start, ecd_end):
    """Remove FF 00 byte stuffing from entropy-coded data."""
    raw = data[ecd_start:ecd_end]
    out = bytearray()
    i = 0
    while i < len(raw):
        out.append(raw[i])
        if raw[i] == 0xFF and i + 1 < len(raw) and raw[i + 1] == 0x00:
            i += 2
        else:
            i += 1
    return bytes(out)


def find_ecd_end(data, ecd_start):
    """Find the position of the EOI or next non-stuffed marker after SOS."""
    pos = ecd_start
    n = len(data)
    while pos < n - 1:
        ff = data.find(b'\xff', pos)
        if ff == -1 or ff >= n - 1:
            return n
        nb = data[ff + 1]
        if nb == 0x00 or (0xD0 <= nb <= 0xD7):
            pos = ff + 2
        else:
            return ff
    return n


# ---------------------------------------------------------------------------
# Bit-stream decoder
# ---------------------------------------------------------------------------

class BitReader:
    def __init__(self, buf):
        self.buf = buf
        self.pos = 0
        self.bits = 0
        self.nbits = 0

    def read_bit(self):
        if self.nbits == 0:
            if self.pos >= len(self.buf):
                return 0
            self.bits = self.buf[self.pos]
            self.pos += 1
            self.nbits = 8
        self.nbits -= 1
        return (self.bits >> self.nbits) & 1

    def decode_huffman(self, table):
        code = 0
        for length in range(1, 17):
            code = (code << 1) | self.read_bit()
            if (code, length) in table:
                return table[(code, length)]
        return None

    def receive(self, ssss):
        if ssss == 0:
            return 0
        val = 0
        for _ in range(ssss):
            val = (val << 1) | self.read_bit()
        if val < (1 << (ssss - 1)):
            val -= (1 << ssss) - 1
        return val


# ---------------------------------------------------------------------------
# IDCT
# ---------------------------------------------------------------------------

def idct_block(coeffs):
    """Full 8x8 inverse DCT. coeffs is a list of 64 floats in natural order."""
    out = [0.0] * 64
    for y in range(8):
        for x in range(8):
            s = 0.0
            for v in range(8):
                cv = (1.0 / math.sqrt(2)) if v == 0 else 1.0
                for u in range(8):
                    cu = (1.0 / math.sqrt(2)) if u == 0 else 1.0
                    s += (cu * cv * coeffs[v * 8 + u]
                          * math.cos((2 * x + 1) * u * math.pi / 16)
                          * math.cos((2 * y + 1) * v * math.pi / 16))
            out[y * 8 + x] = s / 4.0
    return out


# ---------------------------------------------------------------------------
# Full decoder: returns per-component pixel planes
# ---------------------------------------------------------------------------

def decode_jpeg(path):
    with open(path, 'rb') as f:
        data = f.read()

    dht = parse_dht(data)
    dqt = parse_dqt(data)
    width, height, components = parse_sof0(data)
    scan_comps, ecd_start = parse_sos(data)
    ecd_end = find_ecd_end(data, ecd_start)
    ecd = unstuff_ecd(data, ecd_start, ecd_end)

    comp_map = {sc['id']: sc for sc in scan_comps}
    max_h = max(c['h'] for c in components)
    max_v = max(c['v'] for c in components)
    mcu_cols = (width  + max_h * 8 - 1) // (max_h * 8)
    mcu_rows = (height + max_v * 8 - 1) // (max_v * 8)

    # Allocate one plane per component
    planes = {}
    for comp in components:
        plane_w = mcu_cols * comp['h'] * 8
        plane_h = mcu_rows * comp['v'] * 8
        planes[comp['id']] = [[128] * plane_w for _ in range(plane_h)]

    dc_pred = {c['id']: 0 for c in components}
    br = BitReader(ecd)

    for mcu_row in range(mcu_rows):
        for mcu_col in range(mcu_cols):
            for comp in components:
                sc = comp_map[comp['id']]
                q_table = dqt[comp['qt']]
                dc_tbl = dht[(0, sc['dc'])]
                ac_tbl = dht[(1, sc['ac'])]
                for block_v in range(comp['v']):
                    for block_h in range(comp['h']):
                        # Decode zigzag coefficients
                        coeffs_zz = [0] * 64
                        ssss = br.decode_huffman(dc_tbl)
                        dc_pred[comp['id']] += br.receive(ssss) if ssss else 0
                        coeffs_zz[0] = dc_pred[comp['id']]
                        k = 1
                        while k < 64:
                            rs = br.decode_huffman(ac_tbl)
                            if rs == 0x00:
                                break
                            if rs == 0xF0:
                                k += 16
                                continue
                            run = (rs >> 4) & 0xF
                            size = rs & 0xF
                            k += run
                            coeffs_zz[k] = br.receive(size)
                            k += 1

                        # Dequantize into natural order
                        coeffs = [coeffs_zz[ZIGZAG.index(i)] * q_table[i] for i in range(64)]

                        # IDCT → pixel values
                        pixels = idct_block(coeffs)
                        base_row = mcu_row * comp['v'] * 8 + block_v * 8
                        base_col = mcu_col * comp['h'] * 8 + block_h * 8
                        plane = planes[comp['id']]
                        for py in range(8):
                            for px in range(8):
                                v = max(0, min(255, round(pixels[py * 8 + px] + 128)))
                                plane[base_row + py][base_col + px] = v

    # Crop planes to actual image dimensions
    for cid in planes:
        planes[cid] = [row[:width] for row in planes[cid][:height]]

    return width, height, planes


# ---------------------------------------------------------------------------
# Analysis functions
# ---------------------------------------------------------------------------

CNAMES = {1: 'Y  (luma)', 2: 'Cb (blue-diff)', 3: 'Cr (red-diff)'}


def component_stats(planes, width, height):
    stats = {}
    for cid, plane in planes.items():
        vals = [v for row in plane for v in row]
        mn = min(vals)
        mx = max(vals)
        mean = sum(vals) / len(vals)
        stats[cid] = (mn, mx, mean)
    return stats


def approx_rgb(stats):
    Y  = stats[1][2]
    Cb = stats.get(2, (128, 128, 128))[2]
    Cr = stats.get(3, (128, 128, 128))[2]
    R = max(0, min(255, round(Y + 1.402 * (Cr - 128))))
    G = max(0, min(255, round(Y - 0.344 * (Cb - 128) - 0.714 * (Cr - 128))))
    B = max(0, min(255, round(Y + 1.772 * (Cb - 128))))
    return R, G, B


def y_histogram(planes, width, height, buckets=16):
    y_plane = planes[1]
    hist = [0] * 256
    for row in y_plane:
        for v in row:
            hist[v] += 1
    total = sum(hist)
    step = 256 // buckets
    print('  Y brightness histogram (0=black, 255=white):')
    print('  %-12s  %7s  %5s  %s' % ('Range', 'Pixels', '%', 'Bar'))
    print('  ' + '-' * 55)
    for i in range(buckets):
        lo = i * step
        hi = lo + step - 1
        count = sum(hist[lo:hi + 1])
        pct = 100.0 * count / total if total else 0
        bar = '#' * min(40, int(pct * 0.8))
        print('  %3d - %3d     %7d  %5.1f%%  %s' % (lo, hi, count, pct, bar))

    # Percentiles
    cum = 0
    p50 = p95 = p99 = None
    for v in range(256):
        cum += hist[v]
        if p50 is None and cum >= total * 0.50: p50 = v
        if p95 is None and cum >= total * 0.95: p95 = v
        if p99 is None and cum >= total * 0.99: p99 = v
    print()
    print('  Percentiles — p50: %d   p95: %d   p99: %d' % (p50, p95, p99))


def row_anomaly_scan(planes, width, height):
    """Find rows whose mean Y is a statistical outlier relative to all other rows.

    Uses the interquartile range of per-row means to define a normal band.
    A row is anomalous if its mean falls more than 3*IQR outside the IQR, or
    if it contains individual pixels more than 4 standard deviations above the
    image mean (catches bright OSD overlays in otherwise dark images).
    Returns list of (row_index, min, max, mean, bright_outlier_count).
    """
    y_plane = planes[1]
    row_means = [sum(row) / len(row) for row in y_plane]

    sorted_means = sorted(row_means)
    n = len(sorted_means)
    q1 = sorted_means[n // 4]
    q3 = sorted_means[3 * n // 4]
    median = sorted_means[n // 2]
    iqr = q3 - q1 if q3 > q1 else 1.0
    lo_fence = q1 - 3 * iqr
    hi_fence = q3 + 3 * iqr

    # Per-pixel bright threshold: mean + 4*std of all pixel values
    all_vals = [v for row in y_plane for v in row]
    img_mean = sum(all_vals) / len(all_vals)
    img_std  = math.sqrt(sum((v - img_mean) ** 2 for v in all_vals) / len(all_vals))
    bright_thresh = min(255, img_mean + 4 * img_std)

    anomalies = []
    for r, row in enumerate(y_plane):
        mn = min(row); mx = max(row)
        mean = row_means[r]
        bright_px = sum(1 for v in row if v > bright_thresh)
        if mean < lo_fence or mean > hi_fence or bright_px > width * 0.005:
            anomalies.append((r, mn, mx, mean, bright_px))

    return anomalies, median, bright_thresh


def zoom_region(planes, row_start, row_end, col_start, col_end, lo_thresh, hi_thresh):
    """Print an ASCII pixel map of a sub-region of the Y plane."""
    y_plane = planes[1]
    width = len(y_plane[0])
    col_end = min(col_end, width)

    print('  Pixel map (rows %d-%d, cols %d-%d):' % (row_start, row_end - 1, col_start, col_end - 1))
    print('  Key: ## = bright (>%d)   -- = mid (%d-%d)   .. = dim   (space) = very dark (<10)' % (
        hi_thresh + 10, lo_thresh - 10, hi_thresh + 10))
    print()
    header = '  col: ' + ''.join(str((col_start + i) // 10 % 10) if (col_start + i) % 10 == 0 else ' '
                                  for i in range(col_end - col_start))
    print(header)
    header2 = '       ' + ''.join(str((col_start + i) % 10) for i in range(col_end - col_start))
    print(header2)
    print('  ' + '-' * (col_end - col_start + 5))
    for r in range(row_start, row_end):
        if r >= len(y_plane):
            break
        row = y_plane[r][col_start:col_end]
        line = ''
        for v in row:
            if v > hi_thresh + 10:   line += '#'
            elif v > hi_thresh:      line += '-'
            elif v > lo_thresh - 10: line += '.'
            else:                    line += ' '
        mn = min(row); mx = max(row)
        print('  row %3d [%3d-%3d]: %s' % (r, mn, mx, line))


def print_exact_values(planes, row_start, row_end, col_start, col_end):
    """Print exact Y values for a small region."""
    y_plane = planes[1]
    width = len(y_plane[0])
    col_end = min(col_end, width)
    print('  Exact Y values, rows %d-%d, cols %d-%d:' % (row_start, row_end - 1, col_start, col_end - 1))
    print('       ' + ''.join('%4d' % c for c in range(col_start, col_end)))
    for r in range(row_start, row_end):
        if r >= len(y_plane):
            break
        vals = y_plane[r][col_start:col_end]
        print('  %3d: %s' % (r, ''.join('%4d' % v for v in vals)))


# ---------------------------------------------------------------------------
# Top-level report for one file
# ---------------------------------------------------------------------------

def analyse_file(path):
    print()
    print('=' * 72)
    print('File: %s  (%d bytes / %.1f KB)' % (
        path, os.path.getsize(path), os.path.getsize(path) / 1024))
    print('=' * 72)

    try:
        width, height, planes = decode_jpeg(path)
    except ValueError as e:
        print('  Cannot decode: %s' % e)
        return None

    print('  Image size: %d x %d px,  %d component(s)' % (width, height, len(planes)))
    print()

    # --- Component stats ---
    stats = component_stats(planes, width, height)
    print('  Component pixel statistics:')
    print('  %-16s  %5s  %5s  %7s' % ('Component', 'Min', 'Max', 'Mean'))
    print('  ' + '-' * 40)
    for cid in sorted(stats):
        mn, mx, mean = stats[cid]
        print('  %-16s  %5d  %5d  %7.1f' % (CNAMES.get(cid, 'Comp %d' % cid), mn, mx, mean))

    if 1 in stats and 2 in stats and 3 in stats:
        R, G, B = approx_rgb(stats)
        print()
        print('  Approximate mean RGB: R=%d  G=%d  B=%d' % (R, G, B))
        brightness_pct = round((0.299 * R + 0.587 * G + 0.114 * B) / 255 * 100)
        print('  Perceived brightness: ~%d%%  (0%% = black, 100%% = white)' % brightness_pct)

    print()

    # --- Histogram ---
    y_histogram(planes, width, height)
    print()

    # --- Row anomaly scan ---
    anomalies, median_y, bright_thresh = row_anomaly_scan(planes, width, height)
    print('  Row anomaly scan (median Y = %d, bright threshold = %.0f):' % (median_y, bright_thresh))

    # If more than 15% of rows are flagged the image has natural content variation;
    # reporting every row would be noise rather than signal.
    if not anomalies:
        print('  No anomalous rows found — image appears uniform.')
    elif len(anomalies) > height * 0.15:
        print('  %d / %d rows flagged — image has normal content variation, no isolated anomaly.' % (
            len(anomalies), height))
    else:
        print('  %-6s  %5s  %5s  %7s  %s' % ('Row', 'Min', 'Max', 'Mean', 'Bright px'))
        print('  ' + '-' * 45)
        for r, mn, mx, mean, bright_px in anomalies:
            print('  %-6d  %5d  %5d  %7.1f  %d' % (r, mn, mx, mean, bright_px))

        # Zoom into the anomalous region
        anom_rows = [a[0] for a in anomalies]
        zoom_row_start = max(0, min(anom_rows) - 2)
        zoom_row_end   = min(height, max(anom_rows) + 3)

        # Find the column extent of the bright pixels in the anomalous rows
        y_plane = planes[1]
        bright_cols = []
        for r in anom_rows:
            for c, v in enumerate(y_plane[r]):
                if v > bright_thresh:
                    bright_cols.append(c)

        if bright_cols:
            zoom_col_start = max(0, min(bright_cols) - 4)
            zoom_col_end   = min(width, max(bright_cols) + 20)
        else:
            zoom_col_start = 0
            zoom_col_end   = min(80, width)

        lo_thresh = int(median_y - 5)
        hi_thresh = int(median_y + 5)
        print()
        zoom_region(planes, zoom_row_start, zoom_row_end,
                    zoom_col_start, zoom_col_end, lo_thresh, hi_thresh)
        print()
        print_exact_values(planes, max(0, zoom_row_end - 8), zoom_row_end,
                           zoom_col_start, zoom_col_start + 32)

    print()
    return {'path': path, 'width': width, 'height': height,
            'stats': stats, 'anomaly_rows': len(anomalies)}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def collect_paths(targets):
    paths = []
    for arg in targets:
        if os.path.isdir(arg):
            found = sorted(
                os.path.join(arg, f) for f in os.listdir(arg)
                if os.path.splitext(f)[1] in JPEG_EXTENSIONS
                and os.path.isfile(os.path.join(arg, f))
            )
            if not found:
                print('Warning: no JPEG files found in %s' % arg)
            paths.extend(found)
        else:
            expanded = glob.glob(arg)
            paths.extend(sorted(expanded) if expanded else [arg])
    return paths


def main():
    parser = argparse.ArgumentParser(
        prog='jpeg_decode_analyse.py',
        description='Fully decode JPEG files and report pixel-level statistics, '
                    'brightness histogram, and row anomaly scan.',
        epilog=(
            'examples:\n'
            '  %(prog)s img/               analyse all JPEGs in a folder\n'
            '  %(prog)s photo.jpg          analyse a single file\n'
            '  %(prog)s img/ extra.jpg     mix of folder and file'
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
        result = analyse_file(path)
        if result:
            results.append(result)

    if len(results) > 1:
        print()
        print('=' * 72)
        print('SUMMARY ACROSS ALL FILES')
        print('=' * 72)
        fmt = '  %-22s  %9s  %5s  %5s  %7s  %s'
        print(fmt % ('File', 'Size (KB)', 'Y min', 'Y max', 'Y mean', 'Anomalous rows'))
        print('  ' + '-' * 65)
        for r in results:
            mn, mx, mean = r['stats'][1]
            kb = os.path.getsize(r['path']) / 1024
            print(fmt % (
                os.path.basename(r['path']),
                '%.1f' % kb,
                mn, mx, '%.1f' % mean,
                r['anomaly_rows'] if r['anomaly_rows'] else '-',
            ))


if __name__ == '__main__':
    main()
