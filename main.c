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
#include <pthread.h>

#include "telnet.h"
#include "rs232.h"
#include "match.h"
#include "queue.h"
#include "mqtt.h"

#define NELEMS(array) (sizeof(array) / sizeof(array[0]))

struct queue events_queue; /* events queue */

struct evt {
	int event;
	char val[1024];
};

/* 
 * read characters from 'fd' until a newline is encountered. If a newline
 * character is not encountered in the first (n - 1) bytes, then the excess
 * characters are discarded. The returned string placed in 'buf' is
 * null-terminated and includes the newline character if it was read in the
 * first (n - 1) bytes. The function return value is the number of bytes
 * placed in buffer (which includes the newline character if encountered,
 * but excludes the terminating null byte). 
 */
static ssize_t read_line(int fd, void *buffer, size_t n)
{
	ssize_t num_read;                       /* # of bytes fetched by last read() */
	size_t tot_read;                        /* total bytes read so far */
	char *buf;
	char ch;
	
	if (n <= 0 || buffer == NULL) {
		errno = EINVAL;
		return -1;
	}
	
	buf = buffer;                           /* no pointer arithmetic on "void *" */
	
	tot_read = 0;
	for (;;) {
		num_read = read(fd, &ch, 1);
		
		if (num_read == -1) {
			if (errno == EINTR)     /* interrupted --> restart read() */
				continue;
			else
				return -1;      /* some other error */
			
		} else if (num_read == 0) {     /* EOF */
			if (tot_read == 0)      /* no bytes read; return 0 */
				return 0;
			else                    /* some bytes read; add '\0' */
				break;
			
		} else {                        /* 'numRead' must be 1 if we get here */
			if (tot_read < n - 1) { /* siscard > (n - 1) bytes */
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

/* send command "cmd" to modem and wait the "respond" from modem */
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

/* send comand "cmd" to modem and need to handle the respond async */
static int modem_cmd(int uart, char *cmd)
{
	printf("\n\nT:>>sent : %s", cmd);
	rs232_cputs(uart, cmd);

	return 0;
}

/*
 * modem util functions
 */

static int modem_make_call(int uart, char *num)
{
	char cmd[128] = {0};
	snprintf(cmd, sizeof(cmd), "ATD%s;\r\n", num);
	return modem_cmd(uart, cmd);
}

static int modem_answer_call(int uart)
{
	return modem_cmd(uart, "ATA\r\n");
}

static int modem_hangup_call(int uart)
{
	return modem_cmd(uart, "ATH\r\n");
}

static telnet_t *telnet;
static int do_echo;

/*
 * state define
 */
enum STATE {
	STATE_INIT = 0,
	STATE_INCOMING,    /* SIP_A <- SIP_B or modem_A <- modem_O */
	STATE_CALLING,     /* SIP_A -> SIP_B or modem_A -> modem_O */
	STATE_EARLY,       
	STATE_CONNECTING,  
	STATE_CONFIRMED,   
	STATE_DISCONNCTD,  
	STATE_TERMINATED,  
	STATE_UNKNOW,
};

typedef void (*callback)(struct evt *evt);

/* state machine */
void state_init(struct evt *evt);
void state_incoming(struct evt *evt);
void state_calling(struct evt *evt);
void state_early(struct evt *evt);
void state_connecting(struct evt *evt);
void state_confirmed(struct evt *evt);
void state_disconnctd(struct evt *evt);
void state_terminated(struct evt *evt);
void state_unknow(struct evt *evt);

struct state_tbl {
	int state;
	char *str;
	callback func;
} state_tbl[] = {
	{
		.state = STATE_INIT,
		.str = "IN INIT",
		.func = state_init,
	},
	
	{
		.state = STATE_INCOMING,
		.str = "CALL INCOMING",
		.func = state_incoming,
	},
	
	{
		.state = STATE_CALLING,
		.str = "IN CALLING",
		.func = state_calling,
	},
	
	{
		.state = STATE_EARLY,
		.str = "EARLY",
		.func = state_early,
	},
	
	{
		.state = STATE_CONNECTING,
		.str = "IN CONNECTING",
		.func = state_connecting,
	},
	
	{
		.state = STATE_CONFIRMED,
		.str = "CALL CONFIRMED",
		.func = state_confirmed,
	},
	
	{
		.state = STATE_DISCONNCTD,
		.str = "DISCONNCTD",
		.func = state_disconnctd,
	},
	
	{
		.state = STATE_TERMINATED,
		.str = "TERMINATED",
		.func = state_terminated,
	},
	
	{
		.state = STATE_UNKNOW,
		.str = "UNKNOW",
		.func = state_unknow,
	},
};

/*
 * event define 
 */

/* modem event */
enum EVENT {
	EVT_NONE = 1,

	/* modem event */
	EVT_MODEM_COLP,        /* +COLP:xxx, the call is connectd */
	EVT_MODEM_CLIP,        /* +CLIP:xxx, the call incomming  */
	EVT_MODEM_NO_CARRIER,  /* NO CARRIER */
	EVT_MODEM_OK,          /* OK */  
	EVT_MODEM_ERROR,       /* ERROR */   

	EVT_MODEM_MAX,         /* last modem event */

	
	/* sip event */
	EVT_SIP_CALLING = EVT_MODEM_MAX + 1,     /* SIP_A -> SIP_B */
	EVT_SIP_INCOMING,                        /* SIP_A <- SIP_B */
	EVT_SIP_EARLY,       
	EVT_SIP_CONNECTING,  
	EVT_SIP_CONFIRMED,   
	EVT_SIP_DISCONNCTD,

	EVT_SIP_MAX,          /* last sip event */

	/* mqtt event */
	EVT_MQTT_CALLING = EVT_SIP_MAX + 1,      /* Broker -> SIP_A (phone) */
	EVT_MQTT_CALLED,                         /* SIP_A -> Broker */
	
	EVT_UNKNOW,
};

struct event_map {
	enum EVENT event;
	char regex[128];
};

struct event_map modem_events[] = {
	{
		.event = EVT_NONE,
		.regex = "NONE",
	},

	{
		.event = EVT_MODEM_COLP,
		.regex = "+COLP",
	},

	{
		.event = EVT_MODEM_CLIP,
		.regex = "+CLIP",
	},

	{
		.event = EVT_MODEM_NO_CARRIER,
		.regex = "NO CARRIER",
	},

	{
		.event = EVT_MODEM_OK,
		.regex = "OK",
	},

	{
		.event = EVT_MODEM_ERROR,
		.regex = "ERROR",
	},
};

struct event_map sip_events[] = {
	{
		.event = EVT_SIP_CALLING,
		.regex = "state changed to CALLING",
	},

	{
		.event = EVT_SIP_INCOMING,
		.regex = "Press ca a to answer",
	},

	{
		.event = EVT_SIP_EARLY,
		.regex = "NONE",
	},

	{
		.event = EVT_SIP_CONNECTING,
		.regex = "state changed to CONNECTING",
	},

	{
		.event = EVT_SIP_CONFIRMED,
		.regex = "state changed to CONFIRMED",
	},

	{
		.event = EVT_SIP_DISCONNCTD,
		.regex = "is DISCONNECTED",
	},
};

static enum EVENT find_event(char *buf, int size, struct event_map *events, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		if (match(events[i].regex, buf)) {
			return events[i].event;
		}
	}
	
	return EVT_NONE;
}

static void modem_event_queue(char *buf, int size)
{
	enum EVENT event = EVT_NONE;

	event = find_event(buf, size, modem_events, NELEMS(modem_events));
	if (event != EVT_NONE) {
		struct evt *evt = malloc(sizeof(*evt));
		if (!evt) {
			fprintf(stderr, "malloc failed: %s\n", strerror(errno));
			return;
		}
		evt->event = event;
		snprintf(evt->val, sizeof(evt->val), "%s", buf);
		
		queue_add(&events_queue, evt, evt->event);
	}
}

static void sip_event_queue(char *buf, int size)
{
	enum EVENT event = EVT_NONE;

	event = find_event(buf, size, sip_events, NELEMS(sip_events));
	if (event != EVT_NONE) {
		struct evt *evt = malloc(sizeof(*evt));
		if (!evt) {
			fprintf(stderr, "malloc failed: %s\n", strerror(errno));
			return;
		}
		evt->event = event;
		snprintf(evt->val, sizeof(evt->val), "%s", buf);
		
		queue_add(&events_queue, evt, evt->event);
	}
}

/* the call init from */
enum CALL_INIT {
	INIT_NONE,
	INIT_FROM_MODEM,
	INIT_FROM_SIP,
};

/* the sip peer status */
enum PEER_STATUS {
	PEER_OFFLINE,
	PEER_ONLINE,
};

struct siproxy_ctrl {
	int sip_state;
	int modem_state;
	int mqtt_state;

	int state;
	
	int init_dir;

	char uart[255];        /* uart name for modem control */
	char sip_peer[255];    /* sip peer URL */
	int sip_peer_status;   /* online or offline */
	char sim_num[255];     /* the other phone number, 
				  get from modem or mqtt */

	int uart_fd;
	int sock_fd;

	pthread_t thread;      /* event handler thread */

	int dbg_level;
} siproxy = {
	.sip_state   = STATE_INIT,
	.modem_state = STATE_INIT,
	.mqtt_state  = STATE_INIT,

	.state = STATE_INIT,

	.sip_peer_status = PEER_OFFLINE,

	.init_dir = INIT_NONE,

	.uart_fd = -1,
	.sock_fd = -1,

	.thread = -1,

	.dbg_level = 0,
};

static void telnet_input(char *buffer, int size) 
{
	static char crlf[] = { '\r', '\n' };
	int i;

	printf("\n\nS:>>sent : ");
	for (i = 0; i != size; ++i) {
		/* if we got a CR or LF, replace with CRLF
		 * NOTE that usually you'd get a CR in UNIX, but in raw
		 * mode we get LF instead (not sure why)
		 */
		if (buffer[i] == '\r' || buffer[i] == '\n') {
			if (do_echo)
				printf("\r\n");
			telnet_send(telnet, crlf, 2);
		} else {
			if (do_echo)
				putchar(buffer[i]);
			telnet_send(telnet, buffer + i, 1);
		}
	}
	fflush(stdout);
}

/*
 * sip util functions
 */

static void sip_make_call(char *peer)
{
	char cmd[256] = {0};
	snprintf(cmd, sizeof(cmd), "call new %s\r\n", peer);
	telnet_input(cmd, strlen(cmd));
}

static void sip_answer_call()
{
	char cmd[256] = {0};
	snprintf(cmd, sizeof(cmd), "call answer 200\r\n");
	telnet_input(cmd, strlen(cmd));
}

static void sip_hangup_call()
{
	char cmd[256] = {0};
	snprintf(cmd, sizeof(cmd), "hA\r\n");
	telnet_input(cmd, strlen(cmd));
}

static const telnet_telopt_t telopts[] = {
	{ 
		.telopt = TELNET_TELOPT_ECHO,		
		.us = TELNET_WONT,
		.him = TELNET_DO   
	},
	
	{ 
		.telopt = TELNET_TELOPT_TTYPE,		
		.us = TELNET_WILL, 
		.him = TELNET_DONT 
	},
	
	{ 
		.telopt = TELNET_TELOPT_COMPRESS2,	
		.us = TELNET_WONT, 
		.him = TELNET_DO  
	},
	
	{ 
		.telopt = TELNET_TELOPT_MSSP,		
		.us = TELNET_WONT, 
		.him = TELNET_DO  
	},
	
	{
		.telopt = -1, 
		.us = 0, 
		.him = 0 
	}
};

static void cleanup(void)
{
	/* do nothing now */
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
	fflush(stdout);
	
	/* XXX: get modem event */
	if (n > 0)
		modem_event_queue((char *)buf, n);
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
		/* XXX: get sip event */
		fflush(stdout);
		char delim[] = "\n";
		char *token = NULL;
		char *str = (char *)ev->data.buffer;

		/* break the str with newline, then map it to sip event */
		for (token = strtok(str, delim); token; token = strtok(NULL, delim)) {
			sip_event_queue(token, strlen(token));
		}
		break;
		
	/* data must be sent */
	case TELNET_EV_SEND:
		_send(sock, ev->data.buffer, ev->data.size);
		break;
		
	/* request to enable remote feature (or receipt) */
	case TELNET_EV_WILL:
		/* we'll agree to turn off our echo if server
		   wants us to stop */
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

static void siproxy_reset()
{
	siproxy.sip_state   = STATE_INIT;
	siproxy.modem_state = STATE_INIT;
		
	siproxy.state = STATE_INIT;

	siproxy.init_dir = INIT_NONE;
}

void state_init(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		if (siproxy.init_dir == INIT_NONE) {
			siproxy.init_dir = INIT_FROM_MODEM;

			siproxy.modem_state = STATE_INCOMING;
			
			sip_make_call(siproxy.sip_peer);
			siproxy.sip_state = STATE_CALLING;

			siproxy.state = STATE_INCOMING;
		}
		break;
		
	case EVT_MODEM_COLP:
		/* do nothing */
		break;
		
	case EVT_MODEM_NO_CARRIER:
		/* do nothing */
		break;
		
	case EVT_MODEM_OK:
		/* do nothing */
		break;
		
	case EVT_MODEM_ERROR:
		/* do nothing */
		break;
		
	case EVT_SIP_CALLING:
		/* do nothing */
		break;
		
	case EVT_SIP_INCOMING:
		if (siproxy.init_dir == INIT_NONE) {
			siproxy.init_dir = INIT_FROM_SIP;

			siproxy.sip_state = STATE_INCOMING;
			
			modem_make_call(siproxy.uart_fd, "13761124413"); /* FIXME: fix the number */
			siproxy.modem_state = STATE_CALLING;

			siproxy.state = STATE_INCOMING;
		}
		break;
		
	case EVT_SIP_CONNECTING:
		/* do nothing */
		break;
		
	case EVT_SIP_CONFIRMED:
		/* do nothing */
		break;
		
	case EVT_SIP_DISCONNCTD:
		/* do nothin */
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_incoming(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		if (siproxy.init_dir == INIT_FROM_SIP) {
			siproxy.modem_state = STATE_CONFIRMED;
			
			sip_answer_call();
			siproxy.sip_state = STATE_CONFIRMED;

			siproxy.state = STATE_CONFIRMED;
		}
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		if (siproxy.init_dir == INIT_FROM_MODEM) {
			siproxy.sip_state = STATE_CONFIRMED;
			
			modem_answer_call(siproxy.uart_fd);
			siproxy.modem_state = STATE_CONFIRMED;

			siproxy.state = STATE_CONFIRMED;
		}
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_calling(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_early(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_connecting(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_confirmed(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		if (siproxy.init_dir == INIT_FROM_SIP) {
			sip_hangup_call();
		}

		if (siproxy.init_dir == INIT_FROM_MODEM) {
			sip_hangup_call();
		}
		siproxy_reset();
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		if (siproxy.init_dir == INIT_FROM_MODEM) {
			modem_hangup_call(siproxy.uart_fd);
		}

		if (siproxy.init_dir == INIT_FROM_SIP) {
			modem_hangup_call(siproxy.uart_fd);
		}
		siproxy_reset();
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_disconnctd(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_terminated(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state_unknow(struct evt *evt)
{
	switch (evt->event) {
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
	case EVT_SIP_CALLING:
		break;
		
	case EVT_SIP_INCOMING:
		break;
		
	case EVT_SIP_CONNECTING:
		break;
		
	case EVT_SIP_CONFIRMED:
		break;
		
	case EVT_SIP_DISCONNCTD:
		break;
		
	default:
		fprintf(stderr, "UNKNOW event\n");
		break;
	}
}

void state(struct evt *evt)
{
	int i;

	for (i = 0; i < NELEMS(state_tbl); i++) {
		if (siproxy.state == state_tbl[i].state) {
			state_tbl[i].func(evt);
		}
	}
}

void *evt_handler(void *threadid)
{
	int ret = -1;
	struct msg msg;
	struct evt *evt;
	
	while (1) {
		if (queue_length(&events_queue) > 0 && 
		    (ret = queue_get(&events_queue, NULL, &msg)) == 0) {
			evt = msg.data;
			state(evt);
			
			/* free the event after handler it */
			free(evt);
			evt = NULL;
		} else {
			/* no event */
			sleep(1);
		}
	}
}

int main(int argc, char **argv)
{
	char buffer[4096] = {0};
	int rs;
	int sock;
	int uart;
	int rc;
	struct sockaddr_in addr;
	struct pollfd pfd[2];
	struct addrinfo *ai;
	struct addrinfo hints;
	unsigned char buf[4096] = {0};

	/* check usage */
	if (argc != 5) {
		fprintf(stderr, "Usage:\n ./siproxy <host> <port> <sip_peer> <uart>\n");
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

	atexit(cleanup);

	/* set input echoing on by default */
	do_echo = 1;

	if (queue_init(&events_queue) == -1) {
		fprintf(stderr, "event queue init failed: %s\n", strerror(errno));
		return -1;
	}

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

	siproxy.uart_fd = uart;
	siproxy.sock_fd = sock;
	snprintf(siproxy.sip_peer, sizeof(siproxy.sip_peer), "%s", argv[3]);
	snprintf(siproxy.uart, sizeof(siproxy.uart), "%s", argv[4]);

	/* initialize MQTT client */
	mqtt_init();

	/* create the event handler thread */
	rc = pthread_create(&siproxy.thread, NULL, evt_handler, NULL);
	if (rc) {
		fprintf(stderr, "ERROR; return code from pthread_create() is %s\n", strerror(errno));
		exit(-1);
	}
	
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

