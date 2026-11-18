/* test_up_tls: the certificate bundle the board's image embeds, as the
 * symbols esp_crt_bundle.c reads it from (BUNDLE is its path) */
	.section .rodata
	.global _binary_x509_crt_bundle_start
	.global _binary_x509_crt_bundle_end
_binary_x509_crt_bundle_start:
	.incbin BUNDLE
_binary_x509_crt_bundle_end:
	.section .note.GNU-stack,"",@progbits
