#!/bin/bash
# Generate 200x228 store screenshots across weather scenes / times of day.
# Rewrites src/c/demo.h per scenario, builds, installs into the emery
# emulator, captures, then restores demo.h and rebuilds the clean pbw.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=store_assets
mkdir -p "$OUT"

capture() { # name time temp code hours htemps hcodes tmin tmax tcode steps hr batt
  local name=$1
  cat > src/c/demo.h <<EOF
#pragma once
#define DEMO_MODE 1
#define DEMO_TIME "$2"
#define DEMO_TEMP_NOW $3
#define DEMO_CODE_NOW $4
#define DEMO_HOURS {$5}
#define DEMO_HOUR_TEMPS {$6}
#define DEMO_HOUR_CODES {$7}
#define DEMO_TMRW_MIN $8
#define DEMO_TMRW_MAX $9
#define DEMO_TMRW_CODE ${10}
#define DEMO_STEPS ${11}
#define DEMO_HR ${12}
#define DEMO_BATT ${13}
EOF
  pebble build > /dev/null 2>&1
  pebble install --emulator emery > /dev/null 2>&1
  sleep 3
  pebble screenshot --no-open --emulator emery "$OUT/$name.png" > /dev/null 2>&1
  echo "captured $OUT/$name.png"
}

#       name       time    temp code hours                    htemps          hcodes         tmin tmax tcode steps hr batt
capture 1_clear   "07:31"  25   0   '"08","09","10","11"'    '25,26,27,28'   '0,1,1,0'      24  34  0    8427 72 100
capture 2_cloudy  "10:08"  19   3   '"11","12","13","14"'    '19,20,21,21'   '3,2,3,2'      14  21  2    3541 64 85
capture 3_rain    "14:45"  13   63  '"15","16","17","18"'    '13,12,12,11'   '61,63,63,61'  11  16  61   6802 78 70
capture 4_storm   "18:20"  17   95  '"19","20","21","22"'    '17,16,15,15'   '95,95,80,61'  15  22  80   9163 88 55
capture 5_snow    "21:37"  -3   71  '"22","23","00","01"'    '-3,-4,-5,-5'   '71,73,73,71'  -7  -1  71   4207 61 40
capture 6_fog     "06:12"  4    45  '"07","08","09","10"'    '4,5,7,9'       '45,45,48,3'   3   12  45   312  58 90

git checkout -- src/c/demo.h
pebble build > /dev/null 2>&1
echo "demo.h restored, clean pbw rebuilt"
