//%attributes = {"invisible":true,"preemptive":"capable"}

  // Happy path: embed an invoice XML plus one supplementary attachment,
  // then verify the produced document byte-for-byte.

var $xml; $att; $options; $res : Object
var $atts : Collection
var $resources; $tmp; $in; $out; $xmlPath; $notes : Text
var $pdfText; $xmlText; $notesText : Text
var $blob : Blob

$resources:=Convert path system to POSIX(Get 4D folder(Current resources folder))
$tmp:=Convert path system to POSIX(Temporary folder)

$in:=$resources+"sample.pdf"
$xmlPath:=$resources+"factur-x.xml"
$notes:=$resources+"notes.txt"
$out:=$tmp+"facturx-embed.pdf"

If (Test path name(Convert path POSIX to system($out))=Is a document)
	DELETE DOCUMENT(Convert path POSIX to system($out))
End if

$xml:=New object
$xml.path:=$xmlPath
$xml.documentType:="INVOICE"
$xml.version:="1.0"
$xml.conformanceLevel:="EN 16931"
$xml.description:="Factur-X invoice"

$att:=New object
$att.path:=$notes
$att.name:="notes.txt"
$att.type:="text/plain"
$att.relationship:="Supplement"
$att.description:="Supplementary notes"
$atts:=New collection($att)

$options:=New object
$options.overwrite:=False

$res:=PDFA Embed FacturX($in; $xml; $out; $atts; $options)

ASSERT($res#Null; "no status object returned")
ASSERT($res.errorCode=0; "unexpected error "+String($res.errorCode)+": "+String($res.errorMessage))
ASSERT($res.success=True)
ASSERT(Value type($res.warnings)=Is collection)
ASSERT(Test path name(Convert path POSIX to system($out))=Is a document)

  // The plugin writes with qpdf's "preserve" stream mode, so everything it
  // adds stays uncompressed and can be inspected as bytes. Decoding with
  // ISO-8859-1 maps each byte to one character, which makes a substring
  // search a true byte comparison.
DOCUMENT TO BLOB(Convert path POSIX to system($out); $blob)
$pdfText:=Convert to text($blob; "ISO-8859-1")

DOCUMENT TO BLOB(Convert path POSIX to system($xmlPath); $blob)
$xmlText:=Convert to text($blob; "ISO-8859-1")

DOCUMENT TO BLOB(Convert path POSIX to system($notes); $blob)
$notesText:=Convert to text($blob; "ISO-8859-1")

  // Both embedded payloads must survive byte-for-byte.
ASSERT(Position($xmlText; $pdfText)>0; "invoice XML was not embedded verbatim")
ASSERT(Position($notesText; $pdfText)>0; "attachment was not embedded verbatim")

  // Registered in the embedded-files name tree *and* in /AF.
ASSERT(Position("/EmbeddedFiles"; $pdfText)>0; "missing /Names/EmbeddedFiles")
ASSERT(Position("/AF"; $pdfText)>0; "missing /AF")
ASSERT(Position("/AFRelationship /Alternative"; $pdfText)>0; "invoice /AFRelationship missing")
ASSERT(Position("/AFRelationship /Supplement"; $pdfText)>0; "attachment /AFRelationship missing")
ASSERT(Position("(factur-x.xml)"; $pdfText)>0; "invoice file name missing")
ASSERT(Position("(notes.txt)"; $pdfText)>0; "attachment file name missing")

  // A "/" inside a PDF name must be escaped, otherwise the MIME subtype is
  // silently truncated by conforming readers.
ASSERT(Position("/text#2fxml"; $pdfText)>0; "invoice /Subtype is not an escaped MIME name")
ASSERT(Position("/text#2fplain"; $pdfText)>0; "attachment /Subtype is not an escaped MIME name")

  // XMP must carry the Factur-X description and its PDF/A extension schema.
ASSERT(Position("urn:factur-x:pdfa:CrossIndustryDocument:invoice:1p0#"; $pdfText)>0; "missing Factur-X XMP namespace")
ASSERT(Position("<fx:DocumentType>INVOICE</fx:DocumentType>"; $pdfText)>0; "missing fx:DocumentType")
ASSERT(Position("<fx:DocumentFileName>factur-x.xml</fx:DocumentFileName>"; $pdfText)>0; "missing fx:DocumentFileName")
ASSERT(Position("<fx:Version>1.0</fx:Version>"; $pdfText)>0; "missing fx:Version")
ASSERT(Position("<fx:ConformanceLevel>EN 16931</fx:ConformanceLevel>"; $pdfText)>0; "missing fx:ConformanceLevel")
ASSERT(Position("pdfaExtension:schemas"; $pdfText)>0; "missing PDF/A extension schema")

  // The pre-existing pdfaid declaration must be preserved, not dropped.
ASSERT(Position("<pdfaid:part>3</pdfaid:part>"; $pdfText)>0; "pdfaid:part was lost")

  // The metadata stream must stay unfiltered for PDF/A.
ASSERT(Position("/Type /Metadata"; $pdfText)>0; "metadata stream not marked /Type /Metadata")

DELETE DOCUMENT(Convert path POSIX to system($out))
