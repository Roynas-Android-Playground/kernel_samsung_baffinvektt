#include <linux/export.h>
#include <linux/init.h>

#define GLOBAL(name)	\
	.globl ASM_SYMBOL(name);	\
	ASM_SYMBOL(name):

	__INITRODATA

GLOBAL(system_certificate_list)
	.incbin "kernel/x509_certificate_list"
GLOBAL(system_certificate_list_end)
