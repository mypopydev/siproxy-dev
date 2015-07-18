#ifndef UTIL_H
#define UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#define NELEMS(array) (sizeof(array) / sizeof(array[0]))

struct queue events_queue; /* events queue */

struct evt {
	int event;
	char val[1024];
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
	EVT_MQTT_STATUS,                         /* Peer online or offline */

	EVT_MQTT_MAX,          /* last mqtt event */
	
	EVT_UNKNOW,
};


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
