void _start(void) {
	volatile char *ptr;
	ptr=(volatile char *)0x02500000L;
	while(1)
		*ptr='A';
}
