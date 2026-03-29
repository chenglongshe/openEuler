#!/bin/bash
# Script to count code lines in each subdirectory under drivers/ using scc
# Output: CSV file with directory name and total code lines

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
DRIVERS_DIR="$ROOT_DIR/drivers"
OUTPUT_FILE="$ROOT_DIR/drivers_code_stats.csv"

if ! command -v scc &> /dev/null; then
    echo "Error: scc is not installed. Please install it first."
    echo "  go install github.com/boyter/scc/v3@latest"
    exit 1
fi

if [ ! -d "$DRIVERS_DIR" ]; then
    echo "Error: drivers directory not found at $DRIVERS_DIR"
    exit 1
fi

TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"' EXIT

for dir in "$DRIVERS_DIR"/*/; do
    dirname=$(basename "$dir")
    output=$(scc "$dir" 2>/dev/null)
    # Dynamically find the Code column index from the header line
    code_col=$(echo "$output" | grep "^Language" | sed 's/,//g' | awk '{for(i=1;i<=NF;i++) if($i=="Code") print i}')
    if [ -z "$code_col" ]; then
        echo "Warning: skipping $dirname (unable to parse scc output)" >&2
        continue
    fi
    code=$(echo "$output" | grep "^Total" | sed 's/,//g' | awk -v col="$code_col" '{print $col}')
    if [ -n "$code" ]; then
        echo "${dirname},${code}" >> "$TMPFILE"
    else
        echo "Warning: skipping $dirname (no Total line in scc output)" >&2
    fi
done

# Write header and sorted results (descending by code lines) to output CSV
echo "directory,code" > "$OUTPUT_FILE"
sort -t',' -k2 -n -r "$TMPFILE" >> "$OUTPUT_FILE"

echo "Results saved to $OUTPUT_FILE"
