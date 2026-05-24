#!/usr/bin/env bash

# Convert an AsciiDoc file into multiple HTML files,
# one for each Rouge syntax-highlighting style.

set -euo pipefail

INPUT_FILE="${1:-test.adoc}"

if [[ ! -f "$INPUT_FILE" ]]; then
  echo "Error: File '$INPUT_FILE' not found."
  exit 1
fi

BASENAME="$(basename "$INPUT_FILE" .adoc)"

# List of light Rouge styles
STYLES=(
github
igorpro
magritte
pastie
)

for style in "${STYLES[@]}"; do
  OUTPUT_FILE="${BASENAME}_${style}.html"

  echo "Generating $OUTPUT_FILE using Rouge style: $style"

  asciidoctor \
    -r ../docs/src/extensions/rouge_hal.rb \
    -r ../docs/src/extensions/rouge_ngc.rb \
    -a source-highlighter=rouge \
    -a rouge-style="$style" \
    "$INPUT_FILE" \
    -o "$OUTPUT_FILE"
done

echo "Done."