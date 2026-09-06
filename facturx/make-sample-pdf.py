#!/usr/bin/env python3
"""
Generate facturx-test/Resources/sample.pdf.

A small, hand-built PDF 1.7 document carrying an XMP packet that declares
pdfaid:part 3. It exercises the plugin's PDF handling deterministically and
is intentionally kept ASCII-only so the 4D test methods can inspect the
output with plain text searches.

It is NOT a veraPDF-valid PDF/A-3 (no embedded font, no OutputIntent). The
CI conformance job builds a real PDF/A-3 with Ghostscript instead.
"""

import os
import sys

XMP = """<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/">
   <pdfaid:part>3</pdfaid:part>
   <pdfaid:conformance>B</pdfaid:conformance>
  </rdf:Description>
  <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/">
   <dc:title>
    <rdf:Alt>
     <rdf:li xml:lang="x-default">facturx plugin test document</rdf:li>
    </rdf:Alt>
   </dc:title>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>
"""

CONTENT = "BT /F1 18 Tf 72 750 Td (facturx plugin test document) Tj ET\n"


def build():
    objects = []

    objects.append(
        "<< /Type /Catalog /Pages 2 0 R /Metadata 5 0 R >>")
    objects.append(
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objects.append(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
        "/Resources << /Font << /F1 6 0 R >> >> /Contents 4 0 R >>")
    objects.append(
        "<< /Length %d >>\nstream\n%sendstream" % (len(CONTENT), CONTENT))
    objects.append(
        "<< /Type /Metadata /Subtype /XML /Length %d >>\nstream\n%sendstream"
        % (len(XMP), XMP))
    objects.append(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

    out = "%PDF-1.7\n"
    offsets = []
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += "%d 0 obj\n%s\nendobj\n" % (i, body)

    xref_offset = len(out)
    out += "xref\n0 %d\n" % (len(objects) + 1)
    out += "0000000000 65535 f \n"
    for off in offsets:
        out += "%010d 00000 n \n" % off
    out += (
        "trailer\n<< /Size %d /Root 1 0 R "
        "/ID [<0123456789ABCDEF0123456789ABCDEF> "
        "<0123456789ABCDEF0123456789ABCDEF>] >>\n"
        "startxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref_offset))
    return out.encode("ascii")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, "facturx-test", "Resources", "sample.pdf")
    with open(target, "wb") as f:
        f.write(build())
    print("wrote %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
