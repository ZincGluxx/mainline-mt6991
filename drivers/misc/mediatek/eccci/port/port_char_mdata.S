	.section .rodata
	.global port_char_mdata_blob
port_char_mdata_blob:
	.incbin "/root/mainline/linux-mt6991/drivers/misc/mediatek/eccci/port/port_char_mdata.bin"
	.global port_char_mdata_blob_end
port_char_mdata_blob_end:
