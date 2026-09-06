//%attributes = {"invisible":true}

If (Application info.headless)
	
	test_facturx_embed
	test_facturx_errors
	test_facturx_inplace
	test_facturx_conformance
	
	LOG EVENT(Into system standard outputs; "PASS"; Information message)
	
End if
