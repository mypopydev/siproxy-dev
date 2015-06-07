#include "rs232.h"

int	error;

int rs232_open(const char *uart, 
	       int speed,
	       int flow_ctrl,
	       int databits,
	       int stopbits,
	       int parity)
{
	int fd = -1;
	struct termios options;
	int   i;
	int   speed_arr[] = { B115200, B19200, B9600, B4800, B2400, B1200, B300};
	int   name_arr[] = {115200,  19200,  9600,  4800,  2400,  1200,  300};
	fd = open(uart, O_RDWR|O_NOCTTY|O_NDELAY);
	if (-1 == fd)  {
		perror("Can't open serial port\n");
		return(-1);
	}
	
	if (fcntl(fd, F_SETFL, 0) < 0)  {
		perror("fcntl failed!\n");
		return(-1);
	}
  
	if (tcgetattr(fd, &options) !=  0) {
		perror("get arrtr error.\n");
		return(-1);
	}

	for (i = 0; i < sizeof(speed_arr)/sizeof(int); i++) {
		if (speed == name_arr[i]) {
			cfsetispeed(&options, speed_arr[i]);
			cfsetospeed(&options, speed_arr[i]);
		}
	}

	options.c_cflag |= CLOCAL;
	options.c_cflag |= CREAD; 

	switch (flow_ctrl) {
	case 0:
		options.c_cflag &= ~CRTSCTS;
		break;

	case 1:
		options.c_cflag |= CRTSCTS;
		break;
	case 2:
		options.c_cflag |= IXON | IXOFF | IXANY;
		break;
	}

	options.c_cflag &= ~CSIZE;
	switch (databits) {
	case 5:
		options.c_cflag |= CS5;
		break;
	case 6:
		options.c_cflag |= CS6;
		break;
	case 7:   
		options.c_cflag |= CS7;
		break;
	case 8:
		options.c_cflag |= CS8;
		break;
	default:
		fprintf(stderr,"Unsupported data size\n");
		return (-1);
	}

	switch (parity) { 
	case 'n':
	case 'N':
		options.c_cflag &= ~PARENB;
		options.c_iflag &= ~INPCK;     
		break;
	case 'o':
	case 'O':
		options.c_cflag |= (PARODD | PARENB);
		options.c_iflag |= INPCK;   
		break;
	case 'e':
	case 'E':
		options.c_cflag |= PARENB;
		options.c_cflag &= ~PARODD;
		options.c_iflag |= INPCK;      
		break;
	case 's':
	case 'S':
		options.c_cflag &= ~PARENB;
		options.c_cflag &= ~CSTOPB;
		break;
	default:
		fprintf(stderr,"Unsupported parity\n");
		return (-1);
	}

	switch (stopbits) {
	case 1:
		options.c_cflag &= ~CSTOPB;
		break;
	case 2:
		options.c_cflag |= CSTOPB;
		break;
	default:
		fprintf(stderr,"Unsupported stop bits\n");
		return (-1);
	}

	options.c_oflag &= ~OPOST;
	
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	
	options.c_cc[VTIME] = 5;
	options.c_cc[VMIN]  = 1;

	tcflush(fd, TCIFLUSH);
	   
	if (tcsetattr(fd, TCSANOW, &options) != 0) {
		perror("serial port set error!\n");
		return (-1);
	}

	return (fd);
}

int rs232_read(int fd, unsigned char *buf, int size)
{
	int n;

	n = read(fd, buf, size);

	return(n);
}

int rs232_send_byte(int fd, unsigned char byte)
{
	int n;

	n = write(fd, &byte, 1);
	if (n<0)  return(-1);

	return(0);
}

int rs232_send_buf(int fd, unsigned char *buf, int size)
{
	return(write(fd, buf, size));
}

void rs232_close(int fd)
{
	int status;

	if (ioctl(fd, TIOCMGET, &status) == -1) {
		perror("unable to get portstatus");
	}

	status &= ~TIOCM_DTR;    /* turn off DTR */
	status &= ~TIOCM_RTS;    /* turn off RTS */

	if (ioctl(fd, TIOCMSET, &status) == -1) {
		perror("unable to set portstatus");
	}

	close(fd);
}

/*
  Constant  Description
  TIOCM_LE        DSR (data set ready/line enable)
  TIOCM_DTR       DTR (data terminal ready)
  TIOCM_RTS       RTS (request to send)
  TIOCM_ST        Secondary TXD (transmit)
  TIOCM_SR        Secondary RXD (receive)
  TIOCM_CTS       CTS (clear to send)
  TIOCM_CAR       DCD (data carrier detect)
  TIOCM_CD        see TIOCM_CAR
  TIOCM_RNG       RNG (ring)
  TIOCM_RI        see TIOCM_RNG
  TIOCM_DSR       DSR (data set ready)

*/

int rs232_IsDCDEnabled(int fd)
{
	int status;

	ioctl(fd, TIOCMGET, &status);

	if (status&TIOCM_CAR) return(1);
	else return(0);
}

int rs232_IsCTSEnabled(int fd)
{
	int status;

	ioctl(fd, TIOCMGET, &status);

	if (status&TIOCM_CTS) return(1);
	else return(0);
}

int rs232_IsDSREnabled(int fd)
{
	int status;

	ioctl(fd, TIOCMGET, &status);

	if (status&TIOCM_DSR) return(1);
	else return(0);
}

void rs232_enableDTR(int fd)
{
	int status;

	if (ioctl(fd, TIOCMGET, &status) == -1) {
		perror("unable to get portstatus");
	}

	status |= TIOCM_DTR;    /* turn on DTR */

	if (ioctl(fd, TIOCMSET, &status) == -1) {
		perror("unable to set portstatus");
	}
}

void rs232_disableDTR(int fd)
{
	int status;

	if (ioctl(fd, TIOCMGET, &status) == -1) {
		perror("unable to get portstatus");
	}

	status &= ~TIOCM_DTR;    /* turn off DTR */

	if (ioctl(fd, TIOCMSET, &status) == -1) {
		perror("unable to set portstatus");
	}
}

void rs232_enableRTS(int fd)
{
	int status;

	if (ioctl(fd, TIOCMGET, &status) == -1) {
		perror("unable to get portstatus");
	}

	status |= TIOCM_RTS;    /* turn on RTS */

	if (ioctl(fd, TIOCMSET, &status) == -1) {
		perror("unable to set portstatus");
	}
}

void rs232_disableRTS(int fd)
{
	int status;

	if (ioctl(fd, TIOCMGET, &status) == -1) {
		perror("unable to get portstatus");
	}

	status &= ~TIOCM_RTS;    /* turn off RTS */

	if (ioctl(fd, TIOCMSET, &status) == -1) {
		perror("unable to set portstatus");
	}
}

void rs232_cputs(int fd, const char *text)  /* sends a string to serial port */
{
	while(*text != 0)   rs232_send_byte(fd, *(text++));
}
