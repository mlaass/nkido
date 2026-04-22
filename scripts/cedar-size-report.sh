#!/bin/bash
# Cedar binary size report generator.
# Builds Cedar across multiple configurations and emits a markdown report:
#   - Summary sizes (archive, stripped, text)
#   - Delta vs. baseline
#   - Feature matrix (what is enabled in each config)
#   - Per-object text breakdown
#
# Usage:
#   scripts/cedar-size-report.sh                  # print to stdout
#   scripts/cedar-size-report.sh -o report.md     # write to file
#   scripts/cedar-size-report.sh --quick          # skip debug build
#   scripts/cedar-size-report.sh --configs a,b,c  # custom subset
#
# ──────────────────────────────────────────────────────────────────────────
# To add a new feature: append to FEATURES_TOGGLE or FEATURES_MEMORY.
# To add a new config: append to CONFIGS, then any overrides to CONFIG_OVERRIDES.
# The cmake flags, feature matrix, and description rows are all derived.
# ──────────────────────────────────────────────────────────────────────────

set -euo pipefail

cd "$(dirname "$0")/.."

# ============================================================================
# Data model — single source of truth for features and configs
# ============================================================================

# Feature toggles: NAME|DEFAULT|DESCRIPTION
#   Compiled as -DCEDAR_ENABLE_<NAME>=<value> unless NAME == FLOAT_ONLY
#   (which is -DCEDAR_FLOAT_ONLY — it's a behavior flag, not an enable flag).
FEATURES_TOGGLE=(
    "AUDIO_DECODERS|ON|MP3/FLAC/OGG decoders (stb_vorbis, dr_flac, minimp3)"
    "SOUNDFONT|ON|SoundFont (SF2) support via TinySoundFont"
    "FFT|ON|FFT support (KissFFT) for FFT_PROBE opcode"
    "FILE_IO|ON|std::filesystem-based file loading"
    "MINBLEP|ON|MinBLEP anti-aliased oscillators"
    "FLOAT_ONLY|OFF|Use float instead of double for beat timing"
)

# Memory overrides: NAME|DEFAULT|DESCRIPTION
#   Compiled as -DCEDAR_<NAME>=<value> when non-default.
FEATURES_MEMORY=(
    "BLOCK_SIZE|128|Samples per audio block"
    "MAX_BUFFERS|256|Buffer pool size (registers)"
    "MAX_STATES|512|DSP state pool size"
    "MAX_VARS|4096|Variable slots"
    "MAX_PROGRAM_SIZE|4096|Program bytecode capacity"
    "ARENA_SIZE|33554432|AudioArena bytes (default 32 MB)"
)

# Configs: NAME|BUILD_TYPE|DESCRIPTION
CONFIGS=(
    "debug|Debug|Debug, all features, -O0 -g3"
    "release|Release|Release, all features, -O3"
    "minsize|MinSizeRel|MinSizeRel, all features, -Os"
    "minsize-stripped|MinSizeRel|MinSizeRel, no optional modules"
    "esp32|MinSizeRel|ESP32 profile: stripped + reduced memory + float-only"
)

# Per-config overrides: CONFIG|FEATURE|VALUE
#   Only list values that differ from the feature's default.
CONFIG_OVERRIDES=(
    # minsize-stripped turns off all optional modules
    "minsize-stripped|AUDIO_DECODERS|OFF"
    "minsize-stripped|SOUNDFONT|OFF"
    "minsize-stripped|FFT|OFF"
    "minsize-stripped|FILE_IO|OFF"
    "minsize-stripped|MINBLEP|OFF"

    # esp32 inherits the stripped settings plus reduced memory and float-only
    "esp32|AUDIO_DECODERS|OFF"
    "esp32|SOUNDFONT|OFF"
    "esp32|FFT|OFF"
    "esp32|FILE_IO|OFF"
    "esp32|MINBLEP|OFF"
    "esp32|FLOAT_ONLY|ON"
    "esp32|MAX_BUFFERS|64"
    "esp32|MAX_STATES|128"
    "esp32|MAX_VARS|512"
    "esp32|MAX_PROGRAM_SIZE|1024"
    "esp32|ARENA_SIZE|262144"
)

# ============================================================================
# Data accessors
# ============================================================================

# field <n> <record>   extract the Nth pipe-separated field
field() { echo "$2" | awk -F'|' -v i="$1" '{print $i}'; }

# feature_default <name>   look up a feature's default value (toggle or memory)
feature_default() {
    local name="$1"
    for f in "${FEATURES_TOGGLE[@]}" "${FEATURES_MEMORY[@]}"; do
        if [ "$(field 1 "$f")" = "$name" ]; then
            field 2 "$f"
            return 0
        fi
    done
    return 1
}

# feature_kind <name>   echo "toggle" or "memory"
feature_kind() {
    local name="$1"
    for f in "${FEATURES_TOGGLE[@]}"; do
        [ "$(field 1 "$f")" = "$name" ] && echo "toggle" && return
    done
    for f in "${FEATURES_MEMORY[@]}"; do
        [ "$(field 1 "$f")" = "$name" ] && echo "memory" && return
    done
    return 1
}

# get_value <config> <feature>   override if present, else default
get_value() {
    local cfg="$1" feat="$2"
    for ov in "${CONFIG_OVERRIDES[@]}"; do
        if [ "$(field 1 "$ov")" = "$cfg" ] && [ "$(field 2 "$ov")" = "$feat" ]; then
            field 3 "$ov"
            return 0
        fi
    done
    feature_default "$feat"
}

# config_field <config> <n>   pull a field from the CONFIGS table
config_field() {
    local cfg="$1" n="$2"
    for c in "${CONFIGS[@]}"; do
        if [ "$(field 1 "$c")" = "$cfg" ]; then
            field "$n" "$c"
            return 0
        fi
    done
    return 1
}

# cmake_flags_for <config>   derive full cmake flag string from data model
cmake_flags_for() {
    local cfg="$1"
    local build_type
    build_type=$(config_field "$cfg" 2) || return 1

    local flags="-DCMAKE_BUILD_TYPE=$build_type"
    flags+=" -DNKIDO_BUILD_TESTS=OFF -DNKIDO_BUILD_TOOLS=OFF -DNKIDO_BUILD_AKKADO=OFF"

    # Toggle features
    for f in "${FEATURES_TOGGLE[@]}"; do
        local name default value
        name=$(field 1 "$f")
        default=$(field 2 "$f")
        value=$(get_value "$cfg" "$name")
        [ "$value" = "$default" ] && continue
        if [ "$name" = "FLOAT_ONLY" ]; then
            flags+=" -DCEDAR_FLOAT_ONLY=$value"
        else
            flags+=" -DCEDAR_ENABLE_$name=$value"
        fi
    done

    # Memory overrides
    for f in "${FEATURES_MEMORY[@]}"; do
        local name default value
        name=$(field 1 "$f")
        default=$(field 2 "$f")
        value=$(get_value "$cfg" "$name")
        [ "$value" = "$default" ] && continue
        flags+=" -DCEDAR_$name=$value"
    done

    echo "$flags"
}

# ============================================================================
# Argument parsing
# ============================================================================

OUTPUT=""
CONFIGS_ARG=""
QUICK=0
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--output) OUTPUT="$2"; shift 2 ;;
        --quick)     QUICK=1; shift ;;
        --configs)   CONFIGS_ARG="$2"; shift 2 ;;
        -h|--help)
            awk '/^#!/{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

# Resolve which configs to build
if [ -n "$CONFIGS_ARG" ]; then
    IFS=',' read -ra SELECTED <<< "$CONFIGS_ARG"
else
    SELECTED=()
    for c in "${CONFIGS[@]}"; do
        name=$(field 1 "$c")
        if [ "$QUICK" -eq 1 ] && [ "$name" = "debug" ]; then
            continue
        fi
        SELECTED+=("$name")
    done
fi

# ============================================================================
# Build & measure
# ============================================================================

archive_bytes() { stat -c %s "$1"; }

stripped_bytes() {
    local tmp
    tmp=$(mktemp --suffix=.a)
    cp "$1" "$tmp"
    strip "$tmp" 2>/dev/null || true
    stat -c %s "$tmp"
    rm -f "$tmp"
}

text_total() {
    local objdir="$1" total=0 t
    while IFS= read -r -d '' obj; do
        t=$(size "$obj" 2>/dev/null | awk 'NR==2 {print $1}')
        total=$((total + ${t:-0}))
    done < <(find "$objdir" -name "*.o" -print0)
    echo "$total"
}

per_object_rows() {
    local objdir="$1"
    while IFS= read -r -d '' obj; do
        local name sz
        name=$(basename "$obj" .cpp.o)
        sz=$(size "$obj" 2>/dev/null | awk 'NR==2 {printf "%s\t%s\t%s", $1, $2, $3}')
        [ -n "$sz" ] && printf '%s\t%s\n' "$name" "$sz"
    done < <(find "$objdir" -name "*.o" -print0 | sort -z)
}

format_kb() { awk -v b="$1" 'BEGIN { printf "%.1f KB", b/1024 }'; }

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

    printf '%s|%s|%s|%s|%s\n' \
        "$name" \
        "$(archive_bytes "$archive")" \
        "$(stripped_bytes "$archive")" \
        "$(text_total "$objdir")" \
        "$objdir"
}

RESULTS_FILE=$(mktemp)
trap 'rm -f "$RESULTS_FILE"' EXIT
for cfg in "${SELECTED[@]}"; do
    build_and_measure "$cfg" >> "$RESULTS_FILE"
done

# ============================================================================
# Markdown emission
# ============================================================================

emit_report() {
    local timestamp host_arch compiler git_hash
    timestamp=$(date -u +"%Y-%m-%d %H:%M UTC")
    host_arch=$(uname -m)
    compiler=$(${CXX:-c++} --version 2>/dev/null | head -1 || echo "unknown")
    git_hash=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

    echo "# Cedar Binary Size Report"
    echo
    echo "_Generated: ${timestamp} | Host: ${host_arch} | Commit: ${git_hash}_"
    echo "_Compiler: ${compiler}_"
    echo

    # ── Size summary ──────────────────────────────────────────────────────
    echo "## Size Summary"
    echo
    echo "| Config | Description | Archive | Stripped | Text (sum) |"
    echo "|--------|-------------|--------:|---------:|-----------:|"
    while IFS='|' read -r name a s t _; do
        local desc
        desc=$(config_field "$name" 3)
        printf "| \`%s\` | %s | %s | %s | %s |\n" \
            "$name" "$desc" "$(format_kb "$a")" "$(format_kb "$s")" "$(format_kb "$t")"
    done < "$RESULTS_FILE"
    echo

    # ── Delta ─────────────────────────────────────────────────────────────
    local baseline_name baseline_stripped
    baseline_name=$(head -1 "$RESULTS_FILE" | cut -d'|' -f1)
    baseline_stripped=$(head -1 "$RESULTS_FILE" | cut -d'|' -f3)
    echo "## Size Delta vs. \`$baseline_name\`"
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
        printf "| \`%s\` | %s | %+d | %s |\n" \
            "$name" "$(format_kb "$s")" "$delta" "$pct"
    done < "$RESULTS_FILE"
    echo

    # ── Feature matrix ────────────────────────────────────────────────────
    # Columns = selected configs, Rows = features. Defaults shown for context.
    echo "## Feature Matrix"
    echo
    local -a built_configs=()
    while IFS='|' read -r name _ _ _ _; do
        built_configs+=("$name")
    done < "$RESULTS_FILE"

    # Header
    local header="| Feature | Default |"
    local sep="|---------|---------|"
    for c in "${built_configs[@]}"; do
        header+=" \`$c\` |"
        sep+="------|"
    done
    echo "### Toggles"
    echo
    echo "$header"
    echo "$sep"
    for f in "${FEATURES_TOGGLE[@]}"; do
        local name default value cell
        name=$(field 1 "$f")
        default=$(field 2 "$f")
        local row="| \`$name\` | $default |"
        for c in "${built_configs[@]}"; do
            value=$(get_value "$c" "$name")
            if [ "$value" = "ON" ]; then
                cell="✓"
            elif [ "$value" = "OFF" ]; then
                cell="✗"
            else
                cell="$value"
            fi
            # Highlight differences from default
            if [ "$value" != "$default" ]; then
                cell="**$cell**"
            fi
            row+=" $cell |"
        done
        echo "$row"
    done
    echo

    # Memory overrides — show values; bold if differs from default
    echo "### Memory"
    echo
    echo "$header"
    echo "$sep"
    for f in "${FEATURES_MEMORY[@]}"; do
        local name default value
        name=$(field 1 "$f")
        default=$(field 2 "$f")
        local row="| \`$name\` | $default |"
        for c in "${built_configs[@]}"; do
            value=$(get_value "$c" "$name")
            if [ "$value" = "$default" ]; then
                row+=" $value |"
            else
                row+=" **$value** |"
            fi
        done
        echo "$row"
    done
    echo
    echo "_Bold = overridden from default. ✓/✗ = ON/OFF toggle._"
    echo

    # ── Per-object breakdown ──────────────────────────────────────────────
    echo "## Per-object Text Sections"
    echo
    while IFS='|' read -r name _ _ _ objdir; do
        echo "### \`$name\`"
        echo
        echo "| Object | Text | Data | BSS |"
        echo "|--------|-----:|-----:|----:|"
        per_object_rows "$objdir" | sort -k2 -n -r -t$'\t' | \
            while IFS=$'\t' read -r obj text data bss; do
                printf "| \`%s\` | %s | %s | %s |\n" \
                    "$obj" "$(format_kb "$text")" "$(format_kb "$data")" "$(format_kb "$bss")"
            done
        echo
    done < "$RESULTS_FILE"

    # ── Feature reference ────────────────────────────────────────────────
    echo "## Feature Reference"
    echo
    echo "### Toggles"
    echo
    echo "| Toggle | Default | Purpose |"
    echo "|--------|---------|---------|"
    for f in "${FEATURES_TOGGLE[@]}"; do
        printf "| \`CEDAR_ENABLE_%s\` | %s | %s |\n" \
            "$(field 1 "$f")" "$(field 2 "$f")" "$(field 3 "$f")"
    done
    echo
    echo "_Note: \`FLOAT_ONLY\` is compiled as \`-DCEDAR_FLOAT_ONLY\` (behavior flag, not enable flag)._"
    echo
    echo "### Memory Overrides"
    echo
    echo "| Variable | Default | Purpose |"
    echo "|----------|--------:|---------|"
    for f in "${FEATURES_MEMORY[@]}"; do
        printf "| \`CEDAR_%s\` | %s | %s |\n" \
            "$(field 1 "$f")" "$(field 2 "$f")" "$(field 3 "$f")"
    done
}

if [ -n "$OUTPUT" ]; then
    emit_report > "$OUTPUT"
    echo "Report written to $OUTPUT" >&2
else
    emit_report
fi
