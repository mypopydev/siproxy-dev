#ifndef RS232_H
#define RS232_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

int rs232_open(const char *uart, 
	      int speed, 
	      int flow_ctrl, 
	      int databits, 
	      int stopbits, 
	      int parity);
int rs232_read(int, unsigned char *, int);
int rs232_send_byte(int, unsigned char);
int rs232_send_buf(int, unsigned char *, int);
void rs232_close(int);
void rs232_cputs(int, const char *);
int rs232_IsDCDEnabled(int);
int rs232_IsCTSEnabled(int);
int rs232_IsDSREnabled(int);
void rs232_enableDTR(int);
void rs232_disableDTR(int);
void rs232_enableRTS(int);
void rs232_disableRTS(int);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif


