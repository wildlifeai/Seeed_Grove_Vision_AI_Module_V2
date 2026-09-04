#!/usr/bin/env python3
"""Convert NZ local time to UTC - for testing FLASH_MODE_TIME_OF_DAY
(OP_PARAMETER_FLASH_TOD_START / OP_PARAMETER_FLASH_TOD_DURATION, both in
minutes after midnight UTC - see flash_led_modes_proposal.md).

Uses the 'Pacific/Auckland' timezone so NZST/NZDT (daylight saving) is handled
automatically - no manual +12/+13 offset to get wrong. If that timezone data
isn't available (Windows Python has no IANA database built in - the 'tzdata'
package provides it, 'pip install tzdata'), falls back to a fixed UTC+12
(NZST, no daylight saving) so the script still works immediately - a clear
warning is printed either way, since the fallback is wrong for about half the
year (NZDT, UTC+13, runs late Sept - early Apr).

With no arguments, converts the current time ('now'). Optionally pass a
24-hour HH:MM local time to convert instead (today's date, NZ time) - handy
for working out what OP_PARAMETER_FLASH_TOD_START to set for a given local
start time.

Usage:
    python nz_to_utc.py
    python nz_to_utc.py 18:30
"""

import sys
from datetime import datetime, timedelta, timezone

try:
    from zoneinfo import ZoneInfo
    NZ = ZoneInfo("Pacific/Auckland")
    UTC = ZoneInfo("UTC")
except Exception as e:
    print(f"Could not load 'Pacific/Auckland' timezone data ({e}).", file=sys.stderr)
    print("Falling back to a FIXED UTC+12 (NZST) - wrong during NZ daylight saving", file=sys.stderr)
    print("(late Sept - early Apr, NZDT is UTC+13). For correct handling year-round:", file=sys.stderr)
    print("    pip install tzdata", file=sys.stderr)
    print(file=sys.stderr)
    NZ = timezone(timedelta(hours=12), name="NZST (fixed - no DST)")
    UTC = timezone.utc


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0

    now_nz = datetime.now(UTC).astimezone(NZ)

    if len(sys.argv) > 1:
        try:
            hh, mm = sys.argv[1].split(":")
            local_dt = now_nz.replace(hour=int(hh), minute=int(mm), second=0, microsecond=0)
        except ValueError:
            print(f"Usage: {sys.argv[0]} [HH:MM]", file=sys.stderr)
            return 2
    else:
        local_dt = now_nz

    utc_dt = local_dt.astimezone(UTC)
    minutes_after_midnight_utc = utc_dt.hour * 60 + utc_dt.minute

    print(f"NZ local: {local_dt.strftime('%Y-%m-%d %H:%M:%S %Z (UTC%z)')}")
    print(f"UTC:      {utc_dt.strftime('%Y-%m-%d %H:%M:%S %Z')}")
    print(f"Minutes after midnight UTC: {minutes_after_midnight_utc}  "
          f"(OP_PARAMETER_FLASH_TOD_START value for this local time)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
