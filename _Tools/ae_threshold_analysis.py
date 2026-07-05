#!/usr/bin/env python3
"""
ae_threshold_analysis.py
========================
Analyses Auto-Exposure register data extracted from WW500 JPEG MakerNote EXIF
to determine optimal darkness thresholds for automatic flash control.

Reads CSVs produced by jpegAE-batch.py, combines them, and produces:
  1. Time-series plots of each AE register
  2. Correlation scatter plots
  3. Distribution histograms with proposed thresholds
  4. A markdown summary report

Usage:
  python ae_threshold_analysis.py --csvs ae_data_000.csv ae_data_001.csv ae_data_002.csv --outdir <output_dir>
"""

import os
import csv
import argparse
import statistics
from pathlib import Path

# We'll try matplotlib; if not available, fall back to text-only analysis
try:
    import matplotlib
    matplotlib.use('Agg')  # Non-interactive backend
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("WARNING: matplotlib not available. Charts will not be generated.")


def load_csvs(csv_paths):
    """Load and combine multiple CSV files, adding a 'Set' column."""
    rows = []
    for i, path in enumerate(csv_paths):
        with open(path, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                row['Set'] = i
                row['SetName'] = f'IMAGES.{i:03d}'
                rows.append(row)
    return rows


def parse_ae_values(rows):
    """Parse string AE values to numeric types. Returns list of dicts."""
    parsed = []
    for idx, row in enumerate(rows):
        try:
            integration = int(row.get('Integration time', '').strip())
            analog_gain = int(row.get('Analog gain', '').strip())
            digital_gain = int(row.get('Digital gain', '').strip())
            ae_mean = int(row.get('AE Mean', '').strip())
            ae_converged_str = row.get('AEConverged', '').strip()
            ae_converged = 1 if ae_converged_str == 'Y' else 0
        except (ValueError, AttributeError):
            continue

        # File size as proxy for image complexity/brightness
        file_name = row.get('FileName', '')
        file_time = row.get('FileTime', '')

        parsed.append({
            'index': idx,
            'file': file_name,
            'time': file_time,
            'set': row.get('Set', 0),
            'set_name': row.get('SetName', ''),
            'integration': integration,
            'analog_gain': analog_gain,
            'digital_gain': digital_gain,
            'ae_mean': ae_mean,
            'ae_converged': ae_converged,
        })
    return parsed


def classify_brightness(data):
    """
    Classify each image as 'dark' or 'light' using a multi-factor heuristic.

    Dark indicators:
      - AE Mean very low (sensor sees little light)
      - Integration time high (long exposure = compensating for darkness)
      - Analog gain high (amplification maxed)
      - Digital gain high
      - AE not converged (sensor can't find a good exposure = very dark)
    """
    for d in data:
        dark_score = 0

        # AE Mean below ~30 is very dark
        if d['ae_mean'] < 30:
            dark_score += 3
        elif d['ae_mean'] < 50:
            dark_score += 1

        # High integration time
        if d['integration'] > 300:
            dark_score += 2
        elif d['integration'] > 200:
            dark_score += 1

        # Analog gain > 2 means sensor is boosting
        if d['analog_gain'] >= 3:
            dark_score += 2
        elif d['analog_gain'] >= 1:
            dark_score += 1

        # Digital gain > 100 means heavy post-processing boost
        if d['digital_gain'] > 128:
            dark_score += 2
        elif d['digital_gain'] > 80:
            dark_score += 1

        # Not converged means the AE loop couldn't settle
        if d['ae_converged'] == 0:
            dark_score += 2

        d['dark_score'] = dark_score
        d['is_dark'] = dark_score >= 4  # Threshold for classification

    return data


def compute_statistics(data):
    """Compute per-category statistics."""
    categories = {'dark': [], 'light': []}
    for d in data:
        key = 'dark' if d['is_dark'] else 'light'
        categories[key].append(d)

    stats = {}
    fields = ['integration', 'analog_gain', 'digital_gain', 'ae_mean']

    for cat_name, cat_data in categories.items():
        stats[cat_name] = {'count': len(cat_data)}
        for field in fields:
            values = [d[field] for d in cat_data]
            if values:
                stats[cat_name][field] = {
                    'min': min(values),
                    'max': max(values),
                    'mean': round(statistics.mean(values), 1),
                    'median': round(statistics.median(values), 1),
                    'stdev': round(statistics.stdev(values), 1) if len(values) > 1 else 0,
                }
            else:
                stats[cat_name][field] = {'min': 0, 'max': 0, 'mean': 0, 'median': 0, 'stdev': 0}

    return stats, categories


def compute_correlations(data):
    """Compute pairwise Pearson correlation between AE fields."""
    fields = ['integration', 'analog_gain', 'digital_gain', 'ae_mean']
    correlations = {}

    for i, f1 in enumerate(fields):
        for f2 in fields[i+1:]:
            v1 = [d[f1] for d in data]
            v2 = [d[f2] for d in data]
            n = len(v1)
            if n < 2:
                correlations[f'{f1} vs {f2}'] = 0
                continue
            mean1 = sum(v1) / n
            mean2 = sum(v2) / n
            cov = sum((a - mean1) * (b - mean2) for a, b in zip(v1, v2)) / (n - 1)
            std1 = (sum((a - mean1) ** 2 for a in v1) / (n - 1)) ** 0.5
            std2 = (sum((b - mean2) ** 2 for b in v2) / (n - 1)) ** 0.5
            if std1 > 0 and std2 > 0:
                correlations[f'{f1} vs {f2}'] = round(cov / (std1 * std2), 3)
            else:
                correlations[f'{f1} vs {f2}'] = 0

    return correlations


def find_optimal_thresholds(data):
    """
    For each AE field, find the threshold that best separates dark from light images.
    Uses a simple sweep approach to find the value that minimises misclassification.
    """
    fields_and_directions = [
        ('ae_mean', 'below'),       # Dark = low AE Mean
        ('integration', 'above'),   # Dark = high integration time
        ('analog_gain', 'above'),   # Dark = high gain
        ('digital_gain', 'above'),  # Dark = high gain
    ]

    thresholds = {}

    for field, direction in fields_and_directions:
        values = sorted(set(d[field] for d in data))
        best_threshold = values[0]
        best_accuracy = 0

        for thresh in values:
            correct = 0
            for d in data:
                if direction == 'below':
                    predicted_dark = d[field] < thresh
                else:
                    predicted_dark = d[field] > thresh
                if predicted_dark == d['is_dark']:
                    correct += 1
            accuracy = correct / len(data)
            if accuracy > best_accuracy:
                best_accuracy = accuracy
                best_threshold = thresh

        thresholds[field] = {
            'threshold': best_threshold,
            'direction': direction,
            'accuracy': round(best_accuracy * 100, 1),
        }

    return thresholds


def generate_charts(data, categories, thresholds, outdir):
    """Generate analysis charts."""
    if not HAS_MATPLOTLIB:
        return []

    chart_files = []
    os.makedirs(outdir, exist_ok=True)

    dark_data = categories['dark']
    light_data = categories['light']

    # Colour scheme
    DARK_COLOR = '#1a1a2e'
    LIGHT_COLOR = '#f4a261'
    THRESHOLD_COLOR = '#e63946'
    BG_COLOR = '#f8f9fa'

    # ---- Chart 1: Time-series of all AE registers ----
    fig, axes = plt.subplots(4, 1, figsize=(16, 12), sharex=True)
    fig.patch.set_facecolor(BG_COLOR)

    indices = [d['index'] for d in data]
    fields = [
        ('ae_mean', 'AE Mean (brightness)', '0-255'),
        ('integration', 'Integration Time (lines)', '0-65535'),
        ('analog_gain', 'Analog Gain', '0-7'),
        ('digital_gain', 'Digital Gain', '0-255'),
    ]

    for ax, (field, title, range_str) in zip(axes, fields):
        ax.set_facecolor(BG_COLOR)
        values = [d[field] for d in data]
        colors = [DARK_COLOR if d['is_dark'] else LIGHT_COLOR for d in data]
        ax.bar(indices, values, color=colors, width=1.0, edgecolor='none')
        ax.set_ylabel(f'{title}\n({range_str})', fontsize=9)
        ax.grid(axis='y', alpha=0.3)

        # Add threshold line
        if field in thresholds:
            ax.axhline(y=thresholds[field]['threshold'],
                       color=THRESHOLD_COLOR, linestyle='--', linewidth=1.5,
                       label=f"Threshold: {thresholds[field]['threshold']}")
            ax.legend(loc='upper right', fontsize=8)

    axes[-1].set_xlabel('Image Index (chronological order)', fontsize=10)
    fig.suptitle('AE Register Values Over Time - All 303 Images\n(Dark=navy, Light=orange)',
                 fontsize=13, fontweight='bold')
    plt.tight_layout()
    path = os.path.join(outdir, 'ae_timeseries.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    chart_files.append(('ae_timeseries.png', 'Time-series of AE registers'))

    # ---- Chart 2: Scatter - Integration Time vs AE Mean ----
    fig, ax = plt.subplots(figsize=(10, 7))
    fig.patch.set_facecolor(BG_COLOR)
    ax.set_facecolor(BG_COLOR)

    for d in light_data:
        ax.scatter(d['integration'], d['ae_mean'], c=LIGHT_COLOR, s=30, alpha=0.7, edgecolors='#333', linewidths=0.3)
    for d in dark_data:
        ax.scatter(d['integration'], d['ae_mean'], c=DARK_COLOR, s=30, alpha=0.7, edgecolors='#333', linewidths=0.3)

    # Add threshold lines
    if 'ae_mean' in thresholds:
        ax.axhline(y=thresholds['ae_mean']['threshold'],
                   color=THRESHOLD_COLOR, linestyle='--', linewidth=1.5,
                   label=f"AE Mean threshold: {thresholds['ae_mean']['threshold']}")
    if 'integration' in thresholds:
        ax.axvline(x=thresholds['integration']['threshold'],
                   color='#457b9d', linestyle='--', linewidth=1.5,
                   label=f"Integration threshold: {thresholds['integration']['threshold']}")

    ax.set_xlabel('Integration Time (lines)', fontsize=11)
    ax.set_ylabel('AE Mean', fontsize=11)
    ax.set_title('Integration Time vs AE Mean\n(Dark=navy, Light=orange)', fontsize=13, fontweight='bold')
    ax.legend(fontsize=9)
    ax.grid(alpha=0.3)
    plt.tight_layout()
    path = os.path.join(outdir, 'ae_scatter_integration_vs_mean.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    chart_files.append(('ae_scatter_integration_vs_mean.png', 'Integration Time vs AE Mean scatter'))

    # ---- Chart 3: Scatter - Analog Gain vs Digital Gain ----
    fig, ax = plt.subplots(figsize=(10, 7))
    fig.patch.set_facecolor(BG_COLOR)
    ax.set_facecolor(BG_COLOR)

    for d in light_data:
        ax.scatter(d['analog_gain'], d['digital_gain'], c=LIGHT_COLOR, s=30, alpha=0.7, edgecolors='#333', linewidths=0.3)
    for d in dark_data:
        ax.scatter(d['analog_gain'], d['digital_gain'], c=DARK_COLOR, s=30, alpha=0.7, edgecolors='#333', linewidths=0.3)

    ax.set_xlabel('Analog Gain (0-7)', fontsize=11)
    ax.set_ylabel('Digital Gain (0-255)', fontsize=11)
    ax.set_title('Analog Gain vs Digital Gain\n(Dark=navy, Light=orange)', fontsize=13, fontweight='bold')
    ax.grid(alpha=0.3)
    plt.tight_layout()
    path = os.path.join(outdir, 'ae_scatter_gains.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    chart_files.append(('ae_scatter_gains.png', 'Analog vs Digital gain scatter'))

    # ---- Chart 4: Distribution histograms ----
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.patch.set_facecolor(BG_COLOR)

    field_configs = [
        ('ae_mean', 'AE Mean', axes[0, 0]),
        ('integration', 'Integration Time', axes[0, 1]),
        ('analog_gain', 'Analog Gain', axes[1, 0]),
        ('digital_gain', 'Digital Gain', axes[1, 1]),
    ]

    for field, title, ax in field_configs:
        ax.set_facecolor(BG_COLOR)
        dark_vals = [d[field] for d in dark_data]
        light_vals = [d[field] for d in light_data]

        bins = 30
        ax.hist(light_vals, bins=bins, alpha=0.7, color=LIGHT_COLOR, label=f'Light (n={len(light_vals)})', edgecolor='#333', linewidth=0.5)
        ax.hist(dark_vals, bins=bins, alpha=0.7, color=DARK_COLOR, label=f'Dark (n={len(dark_vals)})', edgecolor='#333', linewidth=0.5)

        if field in thresholds:
            ax.axvline(x=thresholds[field]['threshold'],
                       color=THRESHOLD_COLOR, linestyle='--', linewidth=2,
                       label=f"Threshold: {thresholds[field]['threshold']}")

        ax.set_xlabel(title, fontsize=10)
        ax.set_ylabel('Count', fontsize=10)
        ax.legend(fontsize=8)
        ax.grid(axis='y', alpha=0.3)

    fig.suptitle('AE Register Distributions - Dark vs Light',
                 fontsize=13, fontweight='bold')
    plt.tight_layout()
    path = os.path.join(outdir, 'ae_distributions.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    chart_files.append(('ae_distributions.png', 'Distribution histograms'))

    # ---- Chart 5: Composite "darkness score" distribution ----
    fig, ax = plt.subplots(figsize=(10, 6))
    fig.patch.set_facecolor(BG_COLOR)
    ax.set_facecolor(BG_COLOR)

    scores = [d['dark_score'] for d in data]
    for score_val in sorted(set(scores)):
        count_dark = sum(1 for d in data if d['dark_score'] == score_val and d['is_dark'])
        count_light = sum(1 for d in data if d['dark_score'] == score_val and not d['is_dark'])
        ax.bar(score_val - 0.15, count_light, width=0.3, color=LIGHT_COLOR, edgecolor='#333', linewidth=0.5)
        ax.bar(score_val + 0.15, count_dark, width=0.3, color=DARK_COLOR, edgecolor='#333', linewidth=0.5)

    ax.axvline(x=3.5, color=THRESHOLD_COLOR, linestyle='--', linewidth=2,
               label='Classification boundary (score >= 4 = dark)')
    ax.set_xlabel('Darkness Score (composite)', fontsize=11)
    ax.set_ylabel('Number of Images', fontsize=11)
    ax.set_title('Composite Darkness Score Distribution', fontsize=13, fontweight='bold')
    ax.legend(fontsize=9)
    ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.grid(axis='y', alpha=0.3)
    plt.tight_layout()
    path = os.path.join(outdir, 'ae_darkness_score.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    chart_files.append(('ae_darkness_score.png', 'Composite darkness score distribution'))

    return chart_files


def generate_report(data, stats, categories, correlations, thresholds, chart_files, outdir):
    """Generate a markdown summary report."""

    dark_count = stats['dark']['count']
    light_count = stats['light']['count']
    total = dark_count + light_count

    lines = []
    lines.append('# AE Register Analysis - Light Sensor Threshold Report')
    lines.append('')
    lines.append(f'**Total images analysed:** {total}')
    lines.append(f'**Classified as dark:** {dark_count} ({round(dark_count/total*100,1)}%)')
    lines.append(f'**Classified as light:** {light_count} ({round(light_count/total*100,1)}%)')
    lines.append('')

    # Summary table
    lines.append('## Per-Category Statistics')
    lines.append('')
    lines.append('| Register | Category | Min | Max | Mean | Median | Std Dev |')
    lines.append('|----------|----------|-----|-----|------|--------|---------|')
    for field in ['ae_mean', 'integration', 'analog_gain', 'digital_gain']:
        field_display = field.replace('_', ' ').title()
        for cat in ['light', 'dark']:
            s = stats[cat][field]
            lines.append(f"| {field_display} | {cat.upper()} | {s['min']} | {s['max']} | {s['mean']} | {s['median']} | {s['stdev']} |")

    lines.append('')

    # Correlations
    lines.append('## Correlations Between AE Registers')
    lines.append('')
    lines.append('| Pair | Pearson r |')
    lines.append('|------|-----------|')
    for pair, r in sorted(correlations.items(), key=lambda x: abs(x[1]), reverse=True):
        lines.append(f'| {pair} | {r} |')
    lines.append('')

    # Optimal thresholds
    lines.append('## Optimal Single-Register Thresholds')
    lines.append('')
    lines.append('These thresholds were found by sweeping each register value to find the split')
    lines.append('that best separates dark from light images (maximising classification accuracy).')
    lines.append('')
    lines.append('| Register | Direction | Threshold | Accuracy |')
    lines.append('|----------|-----------|-----------|----------|')
    for field in ['ae_mean', 'integration', 'analog_gain', 'digital_gain']:
        t = thresholds[field]
        field_display = field.replace('_', ' ').title()
        direction_text = f"Dark = {t['direction']} {t['threshold']}"
        lines.append(f"| {field_display} | {direction_text} | **{t['threshold']}** | {t['accuracy']}% |")
    lines.append('')

    # Recommendation
    best_field = max(thresholds.keys(), key=lambda k: thresholds[k]['accuracy'])
    best = thresholds[best_field]
    best_display = best_field.replace('_', ' ').title()

    lines.append('## Recommendation')
    lines.append('')
    lines.append(f'> [!IMPORTANT]')
    lines.append(f'> **Best single register: `{best_display}`** - achieves **{best["accuracy"]}%** accuracy')
    lines.append(f'> with threshold **{best["direction"]} {best["threshold"]}**.')
    lines.append('')

    # Detailed reasoning
    lines.append('### Why each register works (or does not)')
    lines.append('')
    lines.append('**AE Mean (register 0x205D):**')
    lines.append(f'- This is the average scene brightness measured by the AE loop (0-255 scale).')
    lines.append(f'- Light images: mean={stats["light"]["ae_mean"]["mean"]}, dark images: mean={stats["dark"]["ae_mean"]["mean"]}')
    lines.append(f'- Accuracy as sole threshold: {thresholds["ae_mean"]["accuracy"]}%')
    lines.append(f'- **Most direct measure of scene brightness.** Should be the primary indicator.')
    lines.append('')
    lines.append('**Integration Time (registers 0x0202-0x0203):**')
    lines.append(f'- Exposure time in sensor lines. Higher = darker (sensor needs more time).')
    lines.append(f'- Light images: mean={stats["light"]["integration"]["mean"]}, dark images: mean={stats["dark"]["integration"]["mean"]}')
    lines.append(f'- Accuracy as sole threshold: {thresholds["integration"]["accuracy"]}%')
    lines.append(f'- Good secondary indicator - saturates at max when very dark.')
    lines.append('')
    lines.append('**Analog Gain (register 0x0205):**')
    lines.append(f'- Only 3 bits (0-7), so coarse granularity.')
    lines.append(f'- Light images: mean={stats["light"]["analog_gain"]["mean"]}, dark images: mean={stats["dark"]["analog_gain"]["mean"]}')
    lines.append(f'- Accuracy as sole threshold: {thresholds["analog_gain"]["accuracy"]}%')
    lines.append(f'- Useful as confirming signal but too coarse to use alone.')
    lines.append('')
    lines.append('**Digital Gain (registers 0x020E-0x020F):**')
    lines.append(f'- Post-ADC amplification.')
    lines.append(f'- Light images: mean={stats["light"]["digital_gain"]["mean"]}, dark images: mean={stats["dark"]["digital_gain"]["mean"]}')
    lines.append(f'- Accuracy as sole threshold: {thresholds["digital_gain"]["accuracy"]}%')
    lines.append('')

    # Transition zone analysis
    lines.append('### Transition Zone (Dusk/Dawn)')
    lines.append('')
    transition = [d for d in data if 3 <= d['dark_score'] <= 5]
    if transition:
        lines.append(f'Found **{len(transition)} images** in the transition zone (dark score 3-5):')
        lines.append('')
        lines.append('| File | AE Mean | Integration | Analog Gain | Digital Gain | Converged | Score |')
        lines.append('|------|---------|-------------|-------------|--------------|-----------|-------|')
        for d in transition[:20]:
            lines.append(f"| {d['file']} | {d['ae_mean']} | {d['integration']} | {d['analog_gain']} | {d['digital_gain']} | {'Y' if d['ae_converged'] else 'N'} | {d['dark_score']} |")
    else:
        lines.append('No images found in the transition zone.')
    lines.append('')

    # Charts
    if chart_files:
        lines.append('## Charts')
        lines.append('')
        for fname, description in chart_files:
            abs_path = os.path.join(outdir, fname).replace('\\', '/')
            lines.append(f'### {description}')
            lines.append(f'![{description}]({abs_path})')
            lines.append('')

    # Proposed OP values
    lines.append('## Proposed Operational Parameter Values')
    lines.append('')
    lines.append('Based on this analysis, the recommended new Operational Parameter for AE darkness threshold:')
    lines.append('')
    lines.append(f'- **Primary register:** AE Mean (most direct brightness measure)')
    lines.append(f'- **Proposed threshold value:** {thresholds["ae_mean"]["threshold"]}')
    lines.append(f'- **Interpretation:** AE Mean **below** this value -> scene is dark -> flash needed')
    lines.append(f'- **Classification accuracy on training data:** {thresholds["ae_mean"]["accuracy"]}%')
    lines.append('')
    lines.append('> [!TIP]')
    lines.append('> Consider also checking integration time as a secondary safeguard:')
    lines.append(f'> if integration time exceeds {thresholds["integration"]["threshold"]}, ')
    lines.append(f'> the sensor is clearly compensating for low light even if AE Mean has not')
    lines.append(f'> dropped below threshold yet (e.g. during dusk transition).')

    report_path = os.path.join(outdir, 'ae_analysis_report.md')
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))

    return report_path


def main():
    parser = argparse.ArgumentParser(description='Analyse AE register data for flash threshold determination')
    parser.add_argument('--csvs', nargs='+', required=True, help='CSV files from jpegAE-batch.py')
    parser.add_argument('--outdir', required=True, help='Output directory for charts and report')
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    print(f'Loading {len(args.csvs)} CSV file(s)...')
    rows = load_csvs(args.csvs)
    print(f'  Total rows: {len(rows)}')

    print('Parsing AE values...')
    data = parse_ae_values(rows)
    print(f'  Valid data points: {len(data)}')
    if not data:
        print("Error: No valid AE data points found. Exiting.")
        return

    print('Classifying brightness...')
    data = classify_brightness(data)
    dark_count = sum(1 for d in data if d['is_dark'])
    light_count = len(data) - dark_count
    print(f'  Dark: {dark_count}, Light: {light_count}')

    print('Computing statistics...')
    stats, categories = compute_statistics(data)

    print('Computing correlations...')
    correlations = compute_correlations(data)

    print('Finding optimal thresholds...')
    thresholds = find_optimal_thresholds(data)
    for field, t in thresholds.items():
        print(f'  {field}: {t["direction"]} {t["threshold"]} (accuracy: {t["accuracy"]}%)')

    print('Generating charts...')
    chart_files = generate_charts(data, categories, thresholds, args.outdir)
    print(f'  Generated {len(chart_files)} chart(s)')

    print('Generating report...')
    report_path = generate_report(data, stats, categories, correlations, thresholds, chart_files, args.outdir)
    print(f'  Report: {report_path}')

    # Also write the combined data to a single CSV for reference
    combined_csv = os.path.join(args.outdir, 'ae_combined_data.csv')
    with open(combined_csv, 'w', newline='', encoding='utf-8') as f:
        fieldnames = ['index', 'file', 'time', 'set_name', 'integration', 'analog_gain',
                       'digital_gain', 'ae_mean', 'ae_converged', 'dark_score', 'is_dark']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for d in data:
            row = {k: d[k] for k in fieldnames}
            writer.writerow(row)
    print(f'  Combined CSV: {combined_csv}')

    print('\nDone!')


if __name__ == '__main__':
    main()
