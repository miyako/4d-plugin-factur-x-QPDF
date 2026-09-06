//%attributes = {"invisible":true,"preemptive":"capable"}

  // Conformance fixture. Embeds an invoice into a genuine PDF/A-3B document
  // produced by make-pdfa3.sh and leaves the result next to the test project
  // so that CI can hand it to veraPDF.
  //
  // Skipped when the fixture has not been generated, so that a plain
  // checkout can still run the rest of the suite.

var $xml; $res : Object
var $resources; $root; $source; $target : Text

$resources:=Convert path system to POSIX(Get 4D folder(Current resources folder))
$root:=Substring($resources; 1; Length($resources)-Length("Resources/"))

$source:=$resources+"pdfa3.pdf"

If (Test path name(Convert path POSIX to system($source))=Is a document)
	
	$target:=$root+"conformance-out.pdf"
	If (Test path name(Convert path POSIX to system($target))=Is a document)
		DELETE DOCUMENT(Convert path POSIX to system($target))
	End if
	
	$xml:=New object
	$xml.path:=$resources+"factur-x.xml"
	$xml.documentType:="INVOICE"
	$xml.version:="1.0"
	$xml.conformanceLevel:="EN 16931"
	
	$res:=PDFA Embed FacturX($source; $xml; $target; New collection(New object(\
"path"; $resources+"notes.txt"; "name"; "notes.txt"; "type"; "text/plain")))
	
	ASSERT($res.errorCode=0; "conformance run failed: "+String($res.errorCode)+" "+String($res.errorMessage))
	ASSERT(Test path name(Convert path POSIX to system($target))=Is a document; "conformance output was not written")
	
End if
