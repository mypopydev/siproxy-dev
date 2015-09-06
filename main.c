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
#include "MQTTAsync.h"


#define ADDRESS     "tcp://121.42.52.171:1883"

#define CLIENTID    "100001Alice"
#define PEERID      "200002Bob"

/* Pub self/ Sub peer topic status */
#define TOPIC_STATUS       "/"CLIENTID"/Status"
#define PEERID_STATUS      "/"PEERID"/Status"

#define ONLINE             "Online"
#define OFFLINE            "Offline"

/* Sub: topic phone number Bob -> Alice, then ACK, 
 * Bob Pub, Alice Sub, then Alice ACK  */
#define TOPIC_BOB_CALLING  "/"PEERID"/Calling/PhoneOther"
#define TOPIC_BOB_ACK      "/"CLIENTID"/Get/PhoneOther"

/* Pub: topic phone number Bob <- Alice, then ACK,
   Alice Pub, Bob Sub, then Bob ACK */
#define TOPIC_ALICE_CALLED "/"CLIENTID"/Called/PhoneOther"
#define TOPIC_ALICE_ACK    "/"PEERID"Get/PhoneOther"

/* Pub self/ Sub peer topic SMS */
#define TOPIC_SMS       "/"CLIENTID"/SMS"
#define PEERID_SMS      "/"PEERID"/SMS"

#define QOS         2
#define TIMEOUT     10000L

#define NELEMS(array) (sizeof(array) / sizeof(array[0]))

#include <stdarg.h>
#include <sys/time.h>

void LOG(const char *fmt, ...) {
	char date[20];
	struct timeval tv;
	va_list args;

	/* print the timestamp */
	gettimeofday(&tv, NULL);
	strftime(date, NELEMS(date), "%Y-%m-%dT%H:%M:%S", localtime(&tv.tv_sec));
	printf("[%s.%03dZ] ", date, (int)tv.tv_usec/1000);

	/* printf like normal */
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

struct queue events_queue; /* events queue */

struct evt {
	int event;
	char val[1024];
};

/*
 * event define 
 */

/* modem/sip/mqtt events */
enum EVENT {
	EVT_NONE = 1,

	/* 
	 *    modem events 
	 */
	EVT_MODEM_COLP,        /* +COLP:xxx, the call is connectd */
	EVT_MODEM_CLIP,        /* +CLIP:xxx, the call incomming  */
	EVT_MODEM_NO_CARRIER,  /* NO CARRIER */
	EVT_MODEM_BUSY,        /* BUSY */
	EVT_MODEM_OK,          /* OK */  
	EVT_MODEM_ERROR,       /* ERROR */   
	EVT_MODEM_SMS,         /* +CMTI: "SM",xxx */
	
	EVT_MODEM_MAX,         /* last modem event */

	
	/* 
	 *    sip events 
	 */
	EVT_SIP_CALLING = EVT_MODEM_MAX + 1,     /* SIP_A -> SIP_B */
	EVT_SIP_INCOMING,                        /* SIP_A <- SIP_B */
	EVT_SIP_EARLY,       
	EVT_SIP_CONNECTING,  
	EVT_SIP_CONFIRMED,   
	EVT_SIP_DISCONNCTD,

	EVT_SIP_MAX,          /* last sip event */

	/* 
	 *    mqtt events 
	 */
	EVT_MQTT_CALLING = EVT_SIP_MAX + 1,      /* Broker -> SIP_A (phone) */
	EVT_MQTT_CALLED,                         /* SIP_A -> Broker */
	EVT_MQTT_STATUS,                         /* Peer online or offline */
	EVT_MQTT_SMS,                            /* Peer send SMS */

	EVT_MQTT_MAX,          /* last mqtt event */
	
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
		.event = EVT_MODEM_BUSY,
		.regex = "BUSY",
	},
	
	{
		.event = EVT_MODEM_OK,
		.regex = "OK",
	},

	{
		.event = EVT_MODEM_ERROR,
		.regex = "ERROR",
	},

	{
		.event = EVT_MODEM_SMS,
		.regex = "+CMTI: \"SM\"",
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

struct event_map mqtt_events[] = {
	{
		.event = EVT_MQTT_CALLING,
		.regex = TOPIC_BOB_CALLING,
	},

	{
		.event = EVT_MQTT_STATUS,
		.regex = PEERID_STATUS,
	},
	
	{
		.event = EVT_MQTT_SMS,
		.regex = PEERID_SMS,
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
	LOG("T:>>sent : %s", cmd);
	usleep(300000);
	
	while (1) {
		n = read_line(uart, buf, 4095);
		
		if (n > 0) {
			buf[n] = 0;   /* always put a "null" at the end of a string! */
			
			for (i = 0; i < n; i++) {
				if (buf[i] < 32) {  /* replace unreadable control-codes by dots */
					buf[i] = '.';
				}
			}
			LOG("T:<<recv : %s\n", (char *)buf);
			
		}
		
		if (n > 0 && match(respond, (char *)buf))
			break;
	}

	return 0;
}

/* send comand "cmd" to modem and need to handle the respond async */
static int modem_cmd(int uart, char *cmd)
{
	LOG("T:>>sent : %s", cmd);
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

static int modem_get_sms(int uart, char *index)
{
	char cmd[128] = {0};
	snprintf(cmd, sizeof(cmd), "AT+CMGR=%s\r\n", index);
	return modem_cmd(uart, cmd);
}

static int modem_delete_sms(int uart, char *index)
{
	char cmd[128] = {0};
	snprintf(cmd, sizeof(cmd), "AT+CMGD=%s\r\n", index);
	return modem_cmd(uart, cmd);
}

static int modem_send_sms(int uart, char *pdu, int tpdu_len)
{
	char cmd[128] = {0};
	char data[4096] = {0};
	snprintf(cmd, sizeof(cmd), "AT+CMGS=%d\r", tpdu_len);
	modem_cmd(uart, cmd);
	usleep(300000);
	snprintf(data, sizeof(data), "%s", pdu);
	data[strlen(data)] = 0x1a;
	return modem_cmd(uart, data);
}

static telnet_t *telnet;
static int do_echo;

void modem_event_sms_queue(char *buf, int size);
static void modem_event_queue(char *buf, int size)
{
	enum EVENT event = EVT_NONE;

	event = find_event(buf, size, modem_events, NELEMS(modem_events));
	if (event != EVT_NONE) {
		if (event == EVT_MODEM_SMS) {
			modem_event_sms_queue(buf, size);
			return;
		}
		
		struct evt *evt = malloc(sizeof(*evt));
		if (!evt) {
			fprintf(stderr, "malloc failed: %s\n", strerror(errno));
			return;
		}
		evt->event = event;
		snprintf(evt->val, sizeof(evt->val), "%s", buf);
		
		queue_add(&events_queue, evt, evt->event);

		return;
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

static void mqtt_event_queue(char *topic, char *playload, int palyloadlen)
{
	enum EVENT event = EVT_NONE;

	event = find_event(topic, strlen(topic), mqtt_events, NELEMS(mqtt_events));
	if (event != EVT_NONE) {
		struct evt *evt = malloc(sizeof(*evt));
		if (!evt) {
			fprintf(stderr, "malloc failed: %s\n", strerror(errno));
			return;
		}
		evt->event = event;
		snprintf(evt->val, palyloadlen+1, "%s", playload);
		
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


static void telnet_input(char *buffer, int size) 
{
	static char crlf[] = { '\r', '\n' };
	int i;

	LOG("\n\nS:>>sent : ");
	for (i = 0; i != size; ++i) {
		/* if we got a CR or LF, replace with CRLF
		 * NOTE that usually you'd get a CR in UNIX, but in raw
		 * mode we get LF instead (not sure why)
		 */
		if (buffer[i] == '\r' || buffer[i] == '\n') {
			if (do_echo)
				LOG("\r\n");
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
		
		for (i = 0; i < n; i++) {
			if (buf[i] < 32) {  /* replace unreadable control-codes by dots */
				buf[i] = '.';
			}
		}
		LOG("T:<<recv : %s\n", (char *)buf);
		
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
		LOG("S:%.*s\n", (int)ev->data.size, ev->data.buffer);
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

/* mqtt util functions */
MQTTAsync client;
volatile MQTTAsync_token deliveredtoken;
MQTTAsync_willOptions willOptions = MQTTAsync_willOptions_initializer;

static void addWillOptions(MQTTAsync_connectOptions *connectOptions) 
{
	willOptions.topicName = TOPIC_STATUS;
	willOptions.message = OFFLINE;
	willOptions.retained = 1;
	willOptions.qos = 2;

	connectOptions->will = &willOptions;
}

void connlost(void *context, char *cause)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	int rc;

	LOG("M:connection lost\n");
	LOG("     cause: %s\n", cause);

	LOG("M:reconnecting\n");
	conn_opts.keepAliveInterval = 30;
	conn_opts.cleansession = 1;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start connect, return code %d\n", rc);
	}
}

int msgarrvd(void *context, char *topicName, int topicLen, MQTTAsync_message *message)
{
	int i;
	char* payloadptr;
	
	LOG("M:message arrived\n");
	LOG("     topic: %s\n", topicName);
	LOG("   message: ");
	
	payloadptr = message->payload;
	for (i = 0; i < message->payloadlen; i++) {
		putchar(*payloadptr++);
	}
	putchar('\n');
	
	/* XXX: get mqtt event and queue */
	mqtt_event_queue(topicName, message->payload, message->payloadlen);
	
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void onDisconnect(void *context, MQTTAsync_successData *response)
{
	LOG("M:successful disconnection\n");
}

void onSend(void *context, MQTTAsync_successData *response)
{
	LOG("M:pub message with token value %d delivery confirmed\n", response->token);
}

void onConnectFailure(void *context, MQTTAsync_failureData *response)
{
	LOG("M:connect failed, rc %d\n", response ? response->code : 0);
}

void onSubscribe(void* context, MQTTAsync_successData* response)
{
	LOG("M:sub succeeded\n");
}

void onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
	LOG("M:sub failed, rc %d\n", response ? response->code : 0);
}

void onConnect(void *context, MQTTAsync_successData *response)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	LOG("M:successful connection\n");

	/* publish online status */
	opts.onSuccess = onSend;
	opts.context = client;

	pubmsg.payload = ONLINE;
	pubmsg.payloadlen = strlen(ONLINE);
	pubmsg.qos = 2;
	pubmsg.retained = 1;
	deliveredtoken = 0;

	if ((rc = MQTTAsync_sendMessage(client, TOPIC_STATUS, 
					&pubmsg, &opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start sendMessage, return code %d\n", rc);
 		exit(-1);	
	}

	/* subcribing to status topic */
	MQTTAsync_responseOptions substat_opts = MQTTAsync_responseOptions_initializer;
	LOG("M:sub to topic %s for client %s using QoS %d\n", PEERID_STATUS, CLIENTID, QOS);
	substat_opts.onSuccess = onSubscribe;
	substat_opts.onFailure = onSubscribeFailure;
	substat_opts.context = client;

	if ((rc = MQTTAsync_subscribe(client, PEERID_STATUS, QOS, 
				      &substat_opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start subscribe, return code %d\n", rc);
		exit(-1);	
	}

	/* subcribing to phone number topic */
	MQTTAsync_responseOptions subphone_opts = MQTTAsync_responseOptions_initializer;
	LOG("M:sub to topic %s for client %s using QoS %d\n", TOPIC_BOB_CALLING, CLIENTID, QOS);
	subphone_opts.onSuccess = onSubscribe;
	subphone_opts.onFailure = onSubscribeFailure;
	subphone_opts.context = client;

	if ((rc = MQTTAsync_subscribe(client, TOPIC_BOB_CALLING, 
				      QOS, &subphone_opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start subscribe, return code %d\n", rc);
		exit(-1);	
	}

	/* subcribing to SMS topic */
	MQTTAsync_responseOptions subsms_opts = MQTTAsync_responseOptions_initializer;
	LOG("M:sub to topic %s for client %s using QoS %d\n", PEERID_SMS, CLIENTID, QOS);
	subsms_opts.onSuccess = onSubscribe;
	subsms_opts.onFailure = onSubscribeFailure;
	subsms_opts.context = client;

	if ((rc = MQTTAsync_subscribe(client, PEERID_SMS, 
				      QOS, &subsms_opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start subscribe, return code %d\n", rc);
		exit(-1);	
	}
}

int mqtt_pub(char *topicName, char *payload, int payloadlen)
{
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;
	int i;

	LOG("M:pub topic : %s\n", topicName);
	LOG("   message: ");
	char *start = payload;
	for (i = 0; i < payloadlen; i++) {
		putchar(*start++);
	}
	putchar('\n');

	/* publish topic */
	opts.onSuccess = onSend;
	opts.context = client;

	pubmsg.payload = payload;
	pubmsg.payloadlen = payloadlen;
	pubmsg.qos = 2;
	pubmsg.retained = 1;
	deliveredtoken = 0;

	if ((rc = MQTTAsync_sendMessage(client, topicName, &pubmsg, 
					&opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start sendMessage, return code %d\n", rc);
 		exit(-1);	
	}

	return rc;
}

int mqtt_sub(char *topicName, int qos)
{
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	LOG("M:subscribing to topic %s for client %s using QoS%d\n\n", 
	       topicName, CLIENTID, qos);
	opts.onSuccess = onSubscribe;
	opts.onFailure = onSubscribeFailure;
	opts.context = client;

	if ((rc = MQTTAsync_subscribe(client, topicName, qos, 
				      &opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start subscribe, return code %d\n", rc);
		exit(-1);	
	}

	return rc;
}

int mqtt_init()
{
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	int rc;

	MQTTAsync_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);

	MQTTAsync_setCallbacks(client, NULL, connlost, msgarrvd, NULL);

	conn_opts.keepAliveInterval = 30;
	conn_opts.cleansession = 1;
	conn_opts.onSuccess = onConnect;
	conn_opts.onFailure = onConnectFailure;
	conn_opts.context = client;
	addWillOptions(&conn_opts);
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
		LOG("M:failed to start connect, return code %d\n", rc);
		exit(-1);	
	}

 	return rc;
}

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

static void siproxy_reset()
{
	siproxy.sip_state   = STATE_INIT;
	siproxy.modem_state = STATE_INIT;
	siproxy.mqtt_state = STATE_INIT;
		
	siproxy.state = STATE_INIT;

	siproxy.init_dir = INIT_NONE;
}

void modem_event_sms_queue(char *buf, int size)
{
	char index[8] = {0};
	char *start = NULL;
	char *end = NULL;
	char buffer[4096] = {0};
	int n,i;

	/* XXX: bypass the MMS */
	if (match("MMS PUSH", buf))
		return;
	
	struct evt *evt = malloc(sizeof(*evt));
	if (!evt) {
		fprintf(stderr, "malloc failed: %s\n", strerror(errno));
		return;
	}

	/* +CMTI: "SM",3, find the index to get the SMS */
	start = strstr(buf, ",");
	if ((end = strstr(buf, ".")) != NULL) {
		memcpy(index, start+1, end - (start+1));
	}
	if (start) {
		modem_get_sms(siproxy.uart_fd, index);
		usleep(300000);
	}

	while (1) {
		n = read_line(siproxy.uart_fd, buffer, 4095);
		if (n > 0) {
			buffer[n] = 0;   /* always put a "null" at the end of a string! */
			
			for (i = 0; i < n; i++) {
				if (buffer[i] < 32) {  /* replace unreadable control-codes by dots */
					buffer[i] = '.';
				}
			}
			LOG("T:<<recv : %s\n", (char *)buffer);
			
		}			

		/* bypass the echo AT+CMGR=18 */
		if (n > 0 && match("AT+CMGR", (char *)buffer))
			continue;
		
		/* bypass the +CMGR: 0,"",28 */
		if (n > 0 && match("+CMGR:", (char *)buffer))
			continue;

		/* bypass newline */
		if (n > 0 && buffer[0] == '.')
			continue;

		/* break when get OK */
		if (n > 0 && match("OK", (char *)buffer))
			break;
		
		evt->event = EVT_MODEM_SMS;
		snprintf(evt->val, strlen(buffer) - 1, "%s", buffer);
		LOG("E:<<recv : %s\n", (char *)buffer);
	}

	/* after queue the SMS event, delete it from modem */
	modem_delete_sms(siproxy.uart_fd, index);
	
	queue_add(&events_queue, evt, evt->event);

	return;
}

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

void state_init(struct evt *evt)
{
	switch (evt->event) {
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		strncpy(siproxy.sim_num, evt->val, strlen(evt->val));
		siproxy.mqtt_state = STATE_INCOMING;
		break;
		
	case EVT_MQTT_STATUS:
		break;

		/*
		 * modem events
		 */
	case EVT_MODEM_CLIP:
		if (siproxy.init_dir == INIT_NONE) {
			siproxy.init_dir = INIT_FROM_MODEM;

			siproxy.modem_state = STATE_INCOMING;
			/* pub phone number and call sip peer */
			char phone[256] = {0};
			char *start = NULL;
			char *end = NULL;
			start = strstr(evt->val, "\"");
			if (start) {
				end = strstr(start+1, "\"");
			}
			memcpy(phone, start+1, end - (start+1));
			mqtt_pub(TOPIC_ALICE_CALLED, phone, strlen(phone));
			sip_make_call(siproxy.sip_peer);
			siproxy.sip_state = STATE_CALLING;

			siproxy.state = STATE_INCOMING;
		}
		break;
		
	case EVT_MODEM_COLP:
		/* do nothing */
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		/* do nothing */
		break;
		
	case EVT_MODEM_OK:
		/* do nothing */
		break;
		
	case EVT_MODEM_ERROR:
		/* do nothing */
		break;

		/*
		 * sip events
		 */
	case EVT_SIP_CALLING:
		/* do nothing */
		break;
		
	case EVT_SIP_INCOMING:
		if (siproxy.init_dir == INIT_NONE && siproxy.mqtt_state == STATE_INCOMING) {
			siproxy.init_dir = INIT_FROM_SIP;

			siproxy.sip_state = STATE_INCOMING;
			
			modem_make_call(siproxy.uart_fd, siproxy.sim_num);
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;

		/*
		 * modem events
		 */		
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
	case EVT_MODEM_BUSY:
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

		/*
		 * sip events
		 */
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

void state_calling(struct evt *evt)
{
	switch (evt->event) {
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;
		
		/*
		 * modem events
		 */
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;

		/*
		 * sip events
		 */		
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;

		/*
		 * modem events
		 */		
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;
		
		/*
		 * sip events
		 */		
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;

		/*
		 * modem events
		 */		
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;

		/*
		 * sip events
		 */		
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;
		
		/*
		 * modem events
		 */
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
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

		/*
		 * sip events
		 */
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;

		/*
		 * modem events
		 */		
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;

		/*
		 * sip events
		 */		
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;

		/*
		 * modem events
		 */		
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;

		/*
		 * sip events
		 */	
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
		/*
		 * mqtt events
		 */
	case EVT_MQTT_CALLING:
		break;
		
	case EVT_MQTT_STATUS:
		break;
		
		/*
		 * modem events
		 */
	case EVT_MODEM_CLIP:
		break;
		
	case EVT_MODEM_COLP:
		break;
		
	case EVT_MODEM_NO_CARRIER:
	case EVT_MODEM_BUSY:
		break;
		
	case EVT_MODEM_OK:
		break;
		
	case EVT_MODEM_ERROR:
		break;

		/*
		 * modem events
		 */		
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
		/* handle SMS
		 * XXX: SMS no states, so don't put this event to state machine */
		switch(evt->event) {
		case EVT_MODEM_SMS:
			/* pub SMS to broker */
			mqtt_pub(TOPIC_SMS, evt->val, strlen(evt->val));
			return;
			break;
			
		case EVT_MQTT_SMS:
			/* send SMS to modem */
			modem_send_sms(siproxy.uart_fd, evt->val, (strlen(evt->val) - 2)/2);
			return;
			break;
		/* pass through */
		}
		
		if (siproxy.state == state_tbl[i].state) {
			LOG("STATE: -> %s\n", state_tbl[i].str);
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
			/* no event, sleep */
			usleep(200000);
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

	/* SMS with PDU mode and USC2 support */
	modem_cmd_wait(uart, "AT+CMGF=0\r\n", "OK");
	modem_cmd_wait(uart, "AT+CSCS=\"UCS2\"\r\n", "OK");

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
		fprintf(stderr, "ERROR; return code from pthread_create() is %s\n", 
			strerror(errno));
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

