//%attributes = {"invisible":true,"preemptive":"capable"}

  // In-place editing and atomicity: the output file must never be left
  // truncated or half-written when the operation fails.

var $xml; $res; $options : Object
var $resources; $tmp; $in; $work; $xmlPath : Text
var $before; $after : Text
var $blob : Blob
var $target : 4D.File

$resources:=Convert path system to POSIX(Get 4D folder(Current resources folder))
$tmp:=Convert path system to POSIX(Temporary folder)

$in:=$resources+"sample.pdf"
$xmlPath:=$resources+"factur-x.xml"
$work:=$tmp+"facturx-inplace.pdf"

$target:=File($work; fk posix path)
If ($target.exists)
	$target.delete()
End if
File($in; fk posix path).copyTo(Folder($tmp; fk posix path); "facturx-inplace.pdf"; fk overwrite)
ASSERT($target.exists; "could not stage a working copy of the fixture")

$xml:=New object
$xml.path:=$xmlPath
$xml.version:="1.0"
$xml.conformanceLevel:="EN 16931"

  // A failure must leave the working copy exactly as it was.
DOCUMENT TO BLOB(Convert path POSIX to system($work); $blob)
$before:=Convert to text($blob; "ISO-8859-1")

$options:=New object("overwrite"; True)
$res:=PDFA Embed FacturX($work; New object("path"; $resources+"missing.xml"; "version"; "1.0"; \
"conformanceLevel"; "EN 16931"); $work; Null; $options)
ASSERT($res.errorCode=105; "expected 105, got "+String($res.errorCode))

DOCUMENT TO BLOB(Convert path POSIX to system($work); $blob)
$after:=Convert to text($blob; "ISO-8859-1")
ASSERT($before=$after; "a failed run modified the target document")

  // Now the real in-place edit.
$res:=PDFA Embed FacturX($work; $xml; $work; Null; $options)
ASSERT($res.errorCode=0; "in-place edit failed: "+String($res.errorCode)+" "+String($res.errorMessage))
ASSERT($res.success=True)

DOCUMENT TO BLOB(Convert path POSIX to system($work); $blob)
$after:=Convert to text($blob; "ISO-8859-1")
ASSERT($before#$after; "in-place edit did not change the document")
ASSERT(Position("(factur-x.xml)"; $after)>0; "in-place edit did not embed the invoice")
ASSERT(Position("/AFRelationship /Alternative"; $after)>0; "in-place edit did not register /AF")

  // No temporary file may be left behind next to the output.
ASSERT(Position(".facturx-"; $after)=0)

DELETE DOCUMENT(Convert path POSIX to system($work))
