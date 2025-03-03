#!/usr/bin/env bash
# Lint a regress test schedule.
#
# Exit code 1: For argument or filesystem issues, return exit code 1.
#
# Exit code 2: If a schedule has test lines that don't match pattern "test:
# <test_name>", output those lines and return exit code 2.
#
# Exit code 3: If ported tests of the schedule don't match the ordering of
# serial_schedule, output those lines and return exit code 3.

# Switch to script dir.
cd "${BASH_SOURCE%/*}" || exit 1

# Check args.
if [ $# -ne 1 ]; then
  echo "incorrect number of arguments: $#" >&2
  exit 1
fi
schedule=$1
if [ ! -f "$schedule" ]; then
  echo "schedule does not exist: $schedule" >&2
  exit 1
fi

# Check schedule style: tests should match pattern "test: <test_name>".
TESTS=$(diff \
          <(grep '^test: ' "$schedule") \
          <(grep -E '^test: \w+$' "$schedule") \
        | grep '^<' \
        | sed 's/^< //')
if [ -n "$TESTS" ]; then
  echo "$TESTS"
  exit 2
fi

# Check schedule test ordering:
# For ported tests (those beginning with "yb_pg_"), they should be ordered the
# same way as in serial_schedule.  Ignore some tests:
# - yb_pg_hint_plan*: these are ported from thirdparty-extension
# - yb_pg_numeric_big: this is in GNUmakefile instead of serial_schedule
# - yb_pg_orafce*: these are ported from thirdparty-extension
# - yb_pg_stat: this is a YB test, not ported: prefix "yb_" + name "pg_stat"
TESTS=$(diff \
          <(grep '^test: yb_pg_' "$schedule" | sed 's/: yb_pg_/: /') \
          serial_schedule \
        | grep '^<' \
        | sed 's/< test: /test: yb_pg_/' \
        | grep -Ev '^test: yb_pg_(hint_plan|numeric_big$|orafce|stat$)')
if [ -n "$TESTS" ]; then
  echo "$TESTS"
  exit 3
fi
