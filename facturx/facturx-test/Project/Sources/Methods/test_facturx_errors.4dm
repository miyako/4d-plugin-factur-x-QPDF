//%attributes = {"invisible":true,"preemptive":"capable"}

  // Negative path coverage. Every rejection must come back as a status
  // object with a stable code -- never as a 4D error and never as a crash.

var $xml; $res; $options : Object
var $resources; $tmp; $in; $out; $xmlPath; $notes; $bad : Text

$resources:=Convert path system to POSIX(Get 4D folder(Current resources folder))
$tmp:=Convert path system to POSIX(Temporary folder)

$in:=$resources+"sample.pdf"
$xmlPath:=$resources+"factur-x.xml"
$notes:=$resources+"notes.txt"
$bad:=$resources+"bad-encoding.xml"
$out:=$tmp+"facturx-errors.pdf"

If (Test path name(Convert path POSIX to system($out))=Is a document)
	DELETE DOCUMENT(Convert path POSIX to system($out))
End if

$options:=New object("overwrite"; True)

  // A well-formed xmlInfo, copied and mutated per case.
$xml:=New object
$xml.path:=$xmlPath
$xml.documentType:="INVOICE"
$xml.version:="1.0"
$xml.conformanceLevel:="EN 16931"

  // --- input validation -------------------------------------------------

  // 101 empty input path
$res:=PDFA Embed FacturX(""; $xml; $out; Null; $options)
ASSERT($res.success=False)
ASSERT($res.errorCode=101; "expected 101, got "+String($res.errorCode))

  // 102 input PDF does not exist
$res:=PDFA Embed FacturX($resources+"no-such-file.pdf"; $xml; $out; Null; $options)
ASSERT($res.errorCode=102; "expected 102, got "+String($res.errorCode))

  // 103 xmlInfo is not an object
$res:=PDFA Embed FacturX($in; Null; $out; Null; $options)
ASSERT($res.errorCode=103; "expected 103, got "+String($res.errorCode))

  // 104 xmlInfo.path missing
$res:=PDFA Embed FacturX($in; New object("version"; "1.0"); $out; Null; $options)
ASSERT($res.errorCode=104; "expected 104, got "+String($res.errorCode))

  // Sanity check on the template, which also leaves $out in place for the
  // output-policy cases further down.
$res:=PDFA Embed FacturX($in; $xml; $out; Null; $options)
ASSERT($res.errorCode=0; "template xmlInfo should succeed, got "+String($res.errorCode))

  // 105 xmlInfo.path unreadable
$res:=PDFA Embed FacturX($in; New object("path"; $resources+"missing.xml"; "version"; "1.0"; \
"conformanceLevel"; "EN 16931"); $out; Null; $options)
ASSERT($res.errorCode=105; "expected 105, got "+String($res.errorCode))

  // 107 xmlInfo.path is not valid UTF-8
$res:=PDFA Embed FacturX($in; New object("path"; $bad; "version"; "1.0"; \
"conformanceLevel"; "EN 16931"); $out; Null; $options)
ASSERT($res.errorCode=107; "expected 107, got "+String($res.errorCode))

  // 108 xmlInfo.name contains a path separator
$res:=PDFA Embed FacturX($in; New object("path"; $xmlPath; "name"; "sub/factur-x.xml"; \
"version"; "1.0"; "conformanceLevel"; "EN 16931"); $out; Null; $options)
ASSERT($res.errorCode=108; "expected 108, got "+String($res.errorCode))

  // 109 xmlInfo.relationship outside {Alternative, Source}
$res:=PDFA Embed FacturX($in; New object("path"; $xmlPath; "relationship"; "Supplement"; \
"version"; "1.0"; "conformanceLevel"; "EN 16931"); $out; Null; $options)
ASSERT($res.errorCode=109; "expected 109, got "+String($res.errorCode))

  // 111 version missing
$res:=PDFA Embed FacturX($in; New object("path"; $xmlPath; "conformanceLevel"; "EN 16931"); \
$out; Null; $options)
ASSERT($res.errorCode=111; "expected 111, got "+String($res.errorCode))

  // 112 conformanceLevel missing
$res:=PDFA Embed FacturX($in; New object("path"; $xmlPath; "version"; "1.0"); $out; Null; $options)
ASSERT($res.errorCode=112; "expected 112, got "+String($res.errorCode))

  // 113 empty output path
$res:=PDFA Embed FacturX($in; $xml; ""; Null; $options)
ASSERT($res.errorCode=113; "expected 113, got "+String($res.errorCode))

  // 122 wrong property type
$res:=PDFA Embed FacturX($in; New object("path"; $xmlPath; "version"; 1; \
"conformanceLevel"; "EN 16931"); $out; Null; $options)
ASSERT($res.errorCode=122; "expected 122, got "+String($res.errorCode))

  // --- attachments ------------------------------------------------------

  // 115 collection element is not an object
$res:=PDFA Embed FacturX($in; $xml; $out; New collection("not an object"); $options)
ASSERT($res.errorCode=115; "expected 115, got "+String($res.errorCode))

  // 116 attachment file unreadable
$res:=PDFA Embed FacturX($in; $xml; $out; New collection(New object("path"; \
$resources+"missing.txt"; "name"; "missing.txt"; "type"; "text/plain")); $options)
ASSERT($res.errorCode=116; "expected 116, got "+String($res.errorCode))

  // 117 attachment name contains a path separator
$res:=PDFA Embed FacturX($in; $xml; $out; New collection(New object("path"; $notes; "name"; \
"a/notes.txt"; "type"; "text/plain")); $options)
ASSERT($res.errorCode=117; "expected 117, got "+String($res.errorCode))

  // 118 attachment name duplicates the invoice XML name
$res:=PDFA Embed FacturX($in; $xml; $out; New collection(New object("path"; $notes; "name"; \
"factur-x.xml"; "type"; "text/plain")); $options)
ASSERT($res.errorCode=118; "expected 118, got "+String($res.errorCode))

  // 118 attachment names duplicate each other
$res:=PDFA Embed FacturX($in; $xml; $out; New collection(\
New object("path"; $notes; "name"; "notes.txt"; "type"; "text/plain"); \
New object("path"; $notes; "name"; "NOTES.TXT"; "type"; "text/plain")); $options)
ASSERT($res.errorCode=118; "expected 118, got "+String($res.errorCode))

  // 119 malformed MIME type
$res:=PDFA Embed FacturX($in; $xml; $out; New collection(New object("path"; $notes; "name"; \
"notes.txt"; "type"; "text plain")); $options)
ASSERT($res.errorCode=119; "expected 119, got "+String($res.errorCode))

  // 120 attachment claims a relationship reserved for the invoice
$res:=PDFA Embed FacturX($in; $xml; $out; New collection(New object("path"; $notes; "name"; \
"notes.txt"; "type"; "text/plain"; "relationship"; "Alternative")); $options)
ASSERT($res.errorCode=120; "expected 120, got "+String($res.errorCode))

  // --- output policy ----------------------------------------------------

  // 301 output exists and overwrite was not requested
ASSERT(Test path name(Convert path POSIX to system($out))=Is a document; "fixture output missing")
$res:=PDFA Embed FacturX($in; $xml; $out; Null; Null)
ASSERT($res.errorCode=301; "expected 301, got "+String($res.errorCode))

  // 305 in-place edit without overwrite
$res:=PDFA Embed FacturX($out; $xml; $out; Null; Null)
ASSERT($res.errorCode=305; "expected 305, got "+String($res.errorCode))

  // --- re-run protection ------------------------------------------------

  // 203 the source already carries an invoice
$res:=PDFA Embed FacturX($out; $xml; $tmp+"facturx-again.pdf"; Null; $options)
ASSERT($res.errorCode=203; "expected 203, got "+String($res.errorCode))

DELETE DOCUMENT(Convert path POSIX to system($out))
If (Test path name(Convert path POSIX to system($tmp+"facturx-again.pdf"))=Is a document)
	DELETE DOCUMENT(Convert path POSIX to system($tmp+"facturx-again.pdf"))
End if
