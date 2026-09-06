#!/bin/sh
# Produce facturx-test/Resources/pdfa3.pdf: a genuinely PDF/A-3B conformant
# document, used by the conformance test.
#
# Ghostscript is a build-time tool here. It is never linked into the plugin,
# so its AGPL licence does not reach the distributed binary.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/facturx-test/Resources/sample.pdf"
out="$here/facturx-test/Resources/pdfa3.pdf"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

gsroot=$(gs -h | tr ',' '\n' | grep -o '[^ ]*/Resource/Init' | head -1 | sed 's|/Resource/Init$||')
icc="$gsroot/iccprofiles/default_rgb.icc"
[ -f "$icc" ] || { echo "sRGB ICC profile not found at $icc" >&2; exit 1; }

cat > "$work/PDFA_def.ps" <<PS
%!
[ /Title (facturx plugin test document) /DOCINFO pdfmark
[ /_objdef {icc_PDFA} /type /stream /OBJ pdfmark
[ {icc_PDFA} << /N 3 >> /PUT pdfmark
[ {icc_PDFA} ($icc) (r) file /PUT pdfmark
[ /_objdef {OutputIntent_PDFA} /type /dict /OBJ pdfmark
[ {OutputIntent_PDFA} <<
    /Type /OutputIntent
    /S /GTS_PDFA1
    /DestOutputProfile {icc_PDFA}
    /OutputConditionIdentifier (sRGB)
  >> /PUT pdfmark
[ {Catalog} << /OutputIntents [ {OutputIntent_PDFA} ] >> /PUT pdfmark
PS

gs -dPDFA=3 -dBATCH -dNOPAUSE -dNOOUTERSAVE -q \
   -dPDFACompatibilityPolicy=1 \
   --permit-file-read="$icc" \
   -sColorConversionStrategy=RGB \
   -sDEVICE=pdfwrite \
   -sOutputFile="$out" \
   "$work/PDFA_def.ps" "$src"

echo "wrote $out"
