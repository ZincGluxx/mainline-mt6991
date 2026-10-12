	.section .rodata
	.global port_char_zdata_blob
port_char_zdata_blob:
	.incbin "/root/mainline/linux-mt6991/drivers/misc/mediatek/eccci/port/port_char_zdata.bin"
	.global port_char_zdata_blob_end
port_char_zdata_blob_end:
