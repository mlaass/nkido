#!/bin/bash
# Cedar binary size report generator.
# Builds Cedar across multiple configurations and emits a markdown report
# comparing archive sizes, stripped sizes, and per-object text sections.
#
# Usage:
#   scripts/cedar-size-report.sh                  # print to stdout
#   scripts/cedar-size-report.sh -o report.md     # write to file
#   scripts/cedar-size-report.sh --quick          # only size-focused configs
#   scripts/cedar-size-report.sh --configs a,b,c  # custom list

set -euo pipefail

cd "$(dirname "$0")/.."

# --- Args ---
OUTPUT=""
CONFIGS=""
QUICK=0
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--output) OUTPUT="$2"; shift 2 ;;
        --quick)     QUICK=1; shift ;;
        --configs)   CONFIGS="$2"; shift 2 ;;
        -h|--help)
            head -12 "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

# Default config list: order matters for table rows
if [ -z "$CONFIGS" ]; then
    if [ "$QUICK" -eq 1 ]; then
        CONFIGS="release,minsize,minsize-stripped,esp32"
    else
        CONFIGS="debug,release,minsize,minsize-stripped,esp32"
    fi
fi

# Map config name -> cmake flags (no LTO for apples-to-apples size)
cmake_flags_for() {
    case "$1" in
        debug)
            echo "-DCMAKE_BUILD_TYPE=Debug -DENKIDO_BUILD_TESTS=OFF -DENKIDO_BUILD_TOOLS=OFF -DENKIDO_BUILD_AKKADO=OFF"
            ;;
        release)
            echo "-DCMAKE_BUILD_TYPE=Release -DENKIDO_BUILD_TESTS=OFF -DENKIDO_BUILD_TOOLS=OFF -DENKIDO_BUILD_AKKADO=OFF"
            ;;
        minsize)
            echo "-DCMAKE_BUILD_TYPE=MinSizeRel -DENKIDO_BUILD_TESTS=OFF -DENKIDO_BUILD_TOOLS=OFF -DENKIDO_BUILD_AKKADO=OFF"
            ;;
        minsize-stripped)
            echo "-DCMAKE_BUILD_TYPE=MinSizeRel -DENKIDO_BUILD_TESTS=OFF -DENKIDO_BUILD_TOOLS=OFF -DENKIDO_BUILD_AKKADO=OFF -DCEDAR_ENABLE_AUDIO_DECODERS=OFF -DCEDAR_ENABLE_SOUNDFONT=OFF -DCEDAR_ENABLE_FFT=OFF -DCEDAR_ENABLE_FILE_IO=OFF -DCEDAR_ENABLE_MINBLEP=OFF"
            ;;
        esp32)
            echo "-DCMAKE_BUILD_TYPE=MinSizeRel -DENKIDO_BUILD_TESTS=OFF -DENKIDO_BUILD_TOOLS=OFF -DENKIDO_BUILD_AKKADO=OFF -DCEDAR_ENABLE_AUDIO_DECODERS=OFF -DCEDAR_ENABLE_SOUNDFONT=OFF -DCEDAR_ENABLE_FFT=OFF -DCEDAR_ENABLE_FILE_IO=OFF -DCEDAR_ENABLE_MINBLEP=OFF -DCEDAR_MAX_BUFFERS=64 -DCEDAR_MAX_STATES=128 -DCEDAR_MAX_VARS=512 -DCEDAR_MAX_PROGRAM_SIZE=1024 -DCEDAR_ARENA_SIZE=262144 -DCEDAR_FLOAT_ONLY=ON"
            ;;
        *)
            return 1
            ;;
    esac
}

# Human-friendly description for each config
describe_config() {
    case "$1" in
        debug)            echo "Debug, all features, -O0 -g3" ;;
        release)          echo "Release, all features, -O3" ;;
        minsize)          echo "MinSizeRel, all features, -Os" ;;
        minsize-stripped) echo "MinSizeRel, no decoders/SF/FFT/MinBLEP/FileIO" ;;
        esp32)            echo "ESP32 profile: stripped + reduced memory + float-only" ;;
        *)                echo "-" ;;
    esac
}

# Format bytes as KB with one decimal (e.g. 152.3 KB)
format_kb() {
    awk -v b="$1" 'BEGIN { printf "%.1f KB", b/1024 }'
}

# Query archive size in bytes
archive_bytes() { stat -c %s "$1"; }

# Strip archive to a temp file, return stripped bytes
stripped_bytes() {
    local archive="$1"
    local tmp
    tmp=$(mktemp --suffix=.a)
    cp "$archive" "$tmp"
    strip "$tmp" 2>/dev/null || true
    stat -c %s "$tmp"
    rm -f "$tmp"
}

# Sum text section across all cedar object files
text_total() {
    local objdir="$1"
    local total=0
    while IFS= read -r -d '' obj; do
        local t
        t=$(size "$obj" 2>/dev/null | awk 'NR==2 {print $1}')
        total=$((total + ${t:-0}))
    done < <(find "$objdir" -name "*.o" -print0)
    echo "$total"
}

# Extract (basename, text, data, bss) for each .o under objdir
per_object_rows() {
    local objdir="$1"
    while IFS= read -r -d '' obj; do
        local name
        name=$(basename "$obj" .cpp.o)
        local sz
        sz=$(size "$obj" 2>/dev/null | awk 'NR==2 {printf "%s\t%s\t%s", $1, $2, $3}')
        if [ -n "$sz" ]; then
            printf '%s\t%s\n' "$name" "$sz"
        fi
    done < <(find "$objdir" -name "*.o" -print0 | sort -z)
}

# --- Run a single config, populate associative-array-ish output ---
# Writes one line to stdout: "config|archive|stripped|text|objdir"
build_and_measure() {
    local name="$1"
    local flags
    flags=$(cmake_flags_for "$name") || { echo "unknown config: $name" >&2; return 1; }
    local build_dir="build-size/$name"

    echo "[build] $name ..." >&2
    rm -rf "$build_dir"
    # shellcheck disable=SC2086
    cmake -B "$build_dir" $flags >/dev/null 2>&1
    cmake --build "$build_dir" --target cedar -j"$(nproc)" >/dev/null 2>&1

    local archive="$build_dir/cedar/libcedar.a"
    local objdir="$build_dir/cedar/CMakeFiles/cedar.dir/src"

    local a s t
    a=$(archive_bytes "$archive")
    s=$(stripped_bytes "$archive")
    t=$(text_total "$objdir")

    printf '%s|%s|%s|%s|%s\n' "$name" "$a" "$s" "$t" "$objdir"
}

# --- Build everything, collect results ---
RESULTS_FILE=$(mktemp)
IFS=',' read -ra CONFIG_ARR <<< "$CONFIGS"
for cfg in "${CONFIG_ARR[@]}"; do
    build_and_measure "$cfg" >> "$RESULTS_FILE"
done

# --- Emit markdown report ---
emit_report() {
    local timestamp
    timestamp=$(date -u +"%Y-%m-%d %H:%M UTC")
    local host_arch
    host_arch=$(uname -m)
    local compiler
    compiler=$(${CXX:-c++} --version 2>/dev/null | head -1 || echo "unknown")
    local git_hash
    git_hash=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

    echo "# Cedar Binary Size Report"
    echo
    echo "_Generated: ${timestamp} | Host: ${host_arch} | Commit: ${git_hash}_"
    echo "_Compiler: ${compiler}_"
    echo
    echo "## Summary"
    echo
    echo "| Config | Description | Archive | Stripped | Text (sum) |"
    echo "|--------|-------------|--------:|---------:|-----------:|"
    while IFS='|' read -r name a s t _; do
        local desc
        desc=$(describe_config "$name")
        printf "| %s | %s | %s | %s | %s |\n" \
            "\`$name\`" "$desc" "$(format_kb "$a")" "$(format_kb "$s")" "$(format_kb "$t")"
    done < "$RESULTS_FILE"
    echo

    # Baseline row for delta
    local baseline_stripped
    baseline_stripped=$(head -1 "$RESULTS_FILE" | cut -d'|' -f3)

    echo "## Size Delta vs. baseline (\`$(head -1 "$RESULTS_FILE" | cut -d'|' -f1)\`)"
    echo
    echo "| Config | Stripped | Δ bytes | Δ % |"
    echo "|--------|---------:|--------:|----:|"
    while IFS='|' read -r name _ s _ _; do
        local delta pct
        delta=$((s - baseline_stripped))
        if [ "$baseline_stripped" -eq 0 ]; then
            pct="-"
        else
            pct=$(awk -v d="$delta" -v b="$baseline_stripped" 'BEGIN { printf "%+.1f%%", (d*100.0)/b }')
        fi
        printf "| %s | %s | %+d | %s |\n" \
            "\`$name\`" "$(format_kb "$s")" "$delta" "$pct"
    done < "$RESULTS_FILE"
    echo

    echo "## Per-object Text Sections"
    echo
    while IFS='|' read -r name _ _ _ objdir; do
        echo "### \`$name\`"
        echo
        echo "| Object | Text | Data | BSS |"
        echo "|--------|-----:|-----:|----:|"
        per_object_rows "$objdir" | sort -k2 -n -r -t$'\t' | \
            while IFS=$'\t' read -r obj text data bss; do
                printf "| %s | %s | %s | %s |\n" \
                    "\`$obj\`" "$(format_kb "$text")" "$(format_kb "$data")" "$(format_kb "$bss")"
            done
        echo
    done < "$RESULTS_FILE"

    echo "## Feature Toggle Reference"
    echo
    echo "| Toggle | Default | Purpose |"
    echo "|--------|---------|---------|"
    echo "| \`CEDAR_ENABLE_AUDIO_DECODERS\` | ON | MP3/FLAC/OGG decoders (stb_vorbis, dr_flac, minimp3) |"
    echo "| \`CEDAR_ENABLE_SOUNDFONT\` | ON | SoundFont (SF2) support via TinySoundFont |"
    echo "| \`CEDAR_ENABLE_FFT\` | ON | FFT support (KissFFT) for FFT_PROBE opcode |"
    echo "| \`CEDAR_ENABLE_FILE_IO\` | ON | std::filesystem-based file loading |"
    echo "| \`CEDAR_ENABLE_MINBLEP\` | ON | MinBLEP anti-aliased oscillators |"
    echo "| \`CEDAR_FLOAT_ONLY\` | OFF | Use float instead of double for beat timing |"
    echo
    echo "## Memory Override Reference"
    echo
    echo "| Variable | Default |"
    echo "|----------|--------:|"
    echo "| \`CEDAR_BLOCK_SIZE\` | 128 |"
    echo "| \`CEDAR_MAX_BUFFERS\` | 256 |"
    echo "| \`CEDAR_MAX_STATES\` | 512 |"
    echo "| \`CEDAR_MAX_VARS\` | 4096 |"
    echo "| \`CEDAR_MAX_PROGRAM_SIZE\` | 4096 |"
    echo "| \`CEDAR_ARENA_SIZE\` | 33554432 (32 MB) |"
}

if [ -n "$OUTPUT" ]; then
    emit_report > "$OUTPUT"
    echo "Report written to $OUTPUT" >&2
else
    emit_report
fi

rm -f "$RESULTS_FILE"
