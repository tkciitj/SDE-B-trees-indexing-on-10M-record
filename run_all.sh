#!/usr/bin/env bash
# Usage: ./run_all.sh path/to/dataset.csv   (builds, self-tests, runs every configuration)
set -e
CSV=${1:?usage: ./run_all.sh dataset.csv}
make
./bpt inspect "$CSV"
./bpt test
rm -f results.csv
./bpt mem  "$CSV"
./bpt disk "$CSV" --cold
for pages in 1024 4096 16384 65536; do ./bpt buf "$CSV" --cache-pages $pages --cold; done
echo "All runs appended to results.csv"
