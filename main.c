#if !defined(_POSIX_SOURCE)
#	define _POSIX_SOURCE
#endif
#if !defined(_BSD_SOURCE)
#	define _BSD_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

#include "telnet.h"
#include "rs232.h"
#include "match.h"

/* Read characters from 'fd' until a newline is encountered. If a newline
  character is not encountered in the first (n - 1) bytes, then the excess
  characters are discarded. The returned string placed in 'buf' is
  null-terminated and includes the newline character if it was read in the
  first (n - 1) bytes. The function return value is the number of bytes
  placed in buffer (which includes the newline character if encountered,
  but excludes the terminating null byte). */
static ssize_t
read_line(int fd, void *buffer, size_t n)
{
	ssize_t num_read;                    /* # of bytes fetched by last read() */
	size_t tot_read;                     /* total bytes read so far */
	char *buf;
	char ch;
	
	if (n <= 0 || buffer == NULL) {
		errno = EINVAL;
		return -1;
	}
	
	buf = buffer;                       /* no pointer arithmetic on "void *" */
	
	tot_read = 0;
	for (;;) {
		num_read = read(fd, &ch, 1);
		
		if (num_read == -1) {
			if (errno == EINTR)         /* interrupted --> restart read() */
				continue;
			else
				return -1;              /* some other error */
			
		} else if (num_read == 0) {     /* EOF */
			if (tot_read == 0)          /* no bytes read; return 0 */
				return 0;
			else                        /* some bytes read; add '\0' */
				break;
			
		} else {                        /* 'numRead' must be 1 if we get here */
			if (tot_read < n - 1) {     /* siscard > (n - 1) bytes */
				tot_read++;
				*buf++ = ch;
			}
			
			if (ch == '\n')
				break;
		}
	}
	
	*buf = '\0';
	return tot_read;
}

/* send comand "cmd" to modem and wait the "respond" from modem */
static int modem_cmd_wait(int uart, char *cmd, char *respond)
{
	unsigned char buf[4096] = {0};
	int n, i = 0;
	
	rs232_cputs(uart, cmd);
	printf("\n\nT:>>sent : %s", cmd);
	sleep(1);
	
	while (1) {
		n = read_line(uart, buf, 4095);
		
		if(n > 0) {
			buf[n] = 0;   /* always put a "null" at the end of a string! */
			
			for (i=0; i < n; i++) {
				if (buf[i] < 32) {  /* replace unreadable control-codes by dots */
					buf[i] = '.';
				}
			}
			printf("T:<<recv : %s\n", (char *)buf);
			
		}
		
		if ( n > 0 && match(respond, (char *)buf))
			break;
	}

	return 0;
}

/* send comand "cmd" to modem */
static int modem_cmd(int uart, char *cmd)
{
	printf("\n\nT:>>sent : %s", cmd);
	rs232_cputs(uart, cmd);

	return 0;
}

static telnet_t *telnet;
static int do_echo;

enum STATE {
	STATE_NULL = 0,
	STATE_INCOMING,    /* SIP_A <- SIP_B */
	STATE_CALLING,     /* SIP_A -> SIP_B */
	STATE_EARLY,       //
	STATE_CONNECTING,  //
	STATE_CONFIRMED,   //
	STATE_DISCONNCTD,  //
	STATE_TERMINATED,  //
	STATE_UNKONW,
};

/* state name define */
static const char *inv_state_names[] =
{
	"NULL",
	"INCOMING",    // SIP_A <- SIP_B or modem_A <- modem_O
	"CALLING",     // SIP_A -> SIP_B or modem_A -> modem_O
	"EARLY",       //
	"CONNECTING",  //
	"CONFIRMED",   //
	"DISCONNCTD",  //
	"TERMINATED",  //
	"UNKNOW"
	
};

enum EVENT {
	EVT_NONE = 0,

	/* modem event */
	EVT_MODEM_CALLING,     /* SIP_A -> SIP_B */
	EVT_MODEM_INCOMING,    /* SIP_A <- SIP_B */
	EVT_MODEM_EARLY,       //
	EVT_MODEM_CONNECTING,  //
	EVT_MODEM_CONFIRMED,   //
	EVT_MODEM_BUSY,        //
	EVT_MODEM_DISCONNCTD,  //
	EVT_MODEM_TERMINATED,  //

	/* sip event */
	EVT_SIP_CALLING,     /* SIP_A -> SIP_B */
	EVT_SIP_INCOMING,    /* SIP_A <- SIP_B */
	EVT_SIP_EARLY,       //
	EVT_SIP_CONNECTING,  //
	EVT_SIP_CONFIRMED,   //
	EVT_SIP_DISCONNCTD,  //
	EVT_SIP_TERMINATED,  //

	EVT_UNKNOW,
};

/* the call init from */
enum CALL_INIT {
	INIT_NONE,
	INIT_MODEM,
	INIT_SIP,
};

struct siproxy {
	int state;
	
	int init_dir;

	char uart[255];
	char sip_peer[255];

	int uart_fd;
	int sock_fd;
};

static const telnet_telopt_t telopts[] = {
	{ TELNET_TELOPT_ECHO,		TELNET_WONT, TELNET_DO   },
	{ TELNET_TELOPT_TTYPE,		TELNET_WILL, TELNET_DONT },
	{ TELNET_TELOPT_COMPRESS2,	TELNET_WONT, TELNET_DO   },
	{ TELNET_TELOPT_MSSP,		TELNET_WONT, TELNET_DO   },
	{ -1, 0, 0 }
};

static void _cleanup(void)
{
	//tcsetattr(STDOUT_FILENO, TCSADRAIN, &orig_tios);
}

static void modem_event(unsigned char *buf, int n)
{
	int i;

	
	if (n > 0) {
		buf[n] = 0;   /* always put a "null" at the end of a string! */
		
		for (i=0; i < n; i++) {
			if (buf[i] < 32) {  /* replace unreadable control-codes by dots */
				buf[i] = '.';
			}
		}
		printf("T:<<recv : %s\n", (char *)buf);
		
	}
	
	//if ( n > 0 && match(respond, (char *)buf))
	//	break;
	/* XXX: get modem event */
	
	fflush(stdout);
}

static void _send(int sock, const char *buffer, size_t size)
{
	int rs;

	/* send data */
	while (size > 0) {
		if ((rs = send(sock, buffer, size, 0)) == -1) {
			fprintf(stderr, "send() failed: %s\n", strerror(errno));
			exit(-1);
		} else if (rs == 0) {
			fprintf(stderr, "send() unexpectedly returned 0\n");
			exit(-1);
		}

		/* update pointer and size to see if we've got more to send */
		buffer += rs;
		size -= rs;
	}
}

static void sip_event(telnet_t *telnet, telnet_event_t *ev,
		void *user_data)
{
	int sock = *(int*)user_data;

	switch (ev->type) {
	/* data received */
	case TELNET_EV_DATA:
		printf("S:%.*s\n", (int)ev->data.size, ev->data.buffer);
		/* XXX: sip event */
		fflush(stdout);
		break;
	/* data must be sent */
	case TELNET_EV_SEND:
		_send(sock, ev->data.buffer, ev->data.size);
		break;
	/* request to enable remote feature (or receipt) */
	case TELNET_EV_WILL:
		/* we'll agree to turn off our echo if server wants us to stop */
		if (ev->neg.telopt == TELNET_TELOPT_ECHO)
			do_echo = 0;
		break;
	/* notification of disabling remote feature (or receipt) */
	case TELNET_EV_WONT:
		if (ev->neg.telopt == TELNET_TELOPT_ECHO)
			do_echo = 1;
		break;
	/* request to enable local feature (or receipt) */
	case TELNET_EV_DO:
		break;
	/* demand to disable local feature (or receipt) */
	case TELNET_EV_DONT:
		break;
	/* respond to TTYPE commands */
	case TELNET_EV_TTYPE:
		/* respond with our terminal type, if requested */
		if (ev->ttype.cmd == TELNET_TTYPE_SEND) {
			telnet_ttype_is(telnet, getenv("TERM"));
		}
		break;
	/* respond to particular subnegotiations */
	case TELNET_EV_SUBNEGOTIATION:
		break;
	/* error */
	case TELNET_EV_ERROR:
		fprintf(stderr, "ERROR: %s\n", ev->error.msg);
		exit(-1);
	default:
		/* ignore */
		break;
	}
}

int main(int argc, char **argv)
{
	char buffer[4096] = {0};
	int rs;
	int sock;
	int uart;
	struct sockaddr_in addr;
	struct pollfd pfd[2];
	struct addrinfo *ai;
	struct addrinfo hints;
	unsigned char buf[4096] = {0};

	/* check usage */
	if (argc != 5) {
		fprintf(stderr, "Usage:\n ./siproxy <host> <port> <sip> <uart>\n");
		return -1;
	}

	/* look up server host */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((rs = getaddrinfo(argv[1], argv[2], &hints, &ai)) != 0) {
		fprintf(stderr, "getaddrinfo() failed for %s: %s\n", argv[1],
				gai_strerror(rs));
		return -1;
	}
	
	/* create server socket */
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		return -1;
	}

	/* bind server socket */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		fprintf(stderr, "bind() failed: %s\n", strerror(errno));
		return -1;
	}

	/* connect */
	if (connect(sock, ai->ai_addr, ai->ai_addrlen) == -1) {
		fprintf(stderr, "connect() failed: %s\n", strerror(errno));
		return -1;
	}

	/* free address lookup info */
	freeaddrinfo(ai);

	atexit(_cleanup);

	/* set input echoing on by default */
	do_echo = 1;

	/* initialize serial port for modem control */
	int bdrate = 115200;
	if ((uart = rs232_open(argv[4], bdrate, 0, 8, 1, 'N')) == -1) {
		fprintf(stderr, "rs232 open failed: %s\n", strerror(errno));
		return -1;
	}

	/* initialize modem */
	modem_cmd_wait(uart, "AT\r\n", "OK");
	modem_cmd_wait(uart, "AT\r\n", "OK");
	modem_cmd_wait(uart, "AT\r\n", "OK");

	modem_cmd_wait(uart, "ATE1\r\n", "OK");
	modem_cmd_wait(uart, "AT+COLP=1\r\n", "OK");
	modem_cmd_wait(uart, "AT+CLIP=1\r\n", "OK");

	/* initialize telnet */
	telnet = telnet_init(telopts, sip_event, 0, &sock);
	
	/* initialize poll descriptors */
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = uart;
	pfd[0].events = POLLIN;
	pfd[1].fd = sock;
	pfd[1].events = POLLIN;

	/* loop while both connections are open */
	while (poll(pfd, 2, -1) != -1) {
		/* read event from modem */
		if (pfd[0].revents & POLLIN) {
			if ((rs = read_line(uart, buf, sizeof(buf))) > 0) {
				modem_event(buf, rs);
			} else if (rs == 0) {
			  	fprintf(stderr, "recv(modem) failed: %s, rs is zero\n",
						strerror(errno));
				break;
			} else {
				fprintf(stderr, "recv(modem) failed: %s\n",
						strerror(errno));
				exit(-1);
			}
		}

		/* read event from sip client */
		if (pfd[1].revents & POLLIN) {
			if ((rs = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
				telnet_recv(telnet, buffer, rs);
			} else if (rs == 0) {
				fprintf(stderr, "recv(client) failed: %s, rs is zero\n",
						strerror(errno));
				break;
			} else {
				fprintf(stderr, "recv(client) failed: %s\n",
						strerror(errno));
				exit(-1);
			}
		}

	}

	/* clean up */
	telnet_free(telnet);
	close(sock);
	rs232_close(uart);

	return 0;
}

