#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "MQTTAsync.h"

#define ADDRESS     "tcp://121.42.52.171:1883"

#define CLIENTID    "10001Alice"
#define PEERID      "20002Bob"

/* topic status */
#define TOPIC_STATUS       "/"CLIENTID"/Status"
#define ONLINE             "Online"
#define OFFLINE            "Offline"

/* topic phone number Bob -> Alice, then ACK, 
 * Bob Pub, Alice Sub, then Alice ACK  */
#define TOPIC_BOB_CALLING  "/"PEERID"/Calling/PhoneOther"
#define TOPIC_ACK_BOB      "/"CLIENTID"/Get/PhoneOther"

/* topic phone number Bob <- Alice, then ACK,
   Alice Pub, Alice Sub, then Bob ACK */
#define TOPIC_ALICE_CALLED "/"CLIENTID"/Called/PhoneOther"
#define TOPIC_ALICE_ACK    "/"PEERID"Get/PhoneOther"

#define QOS         2
#define TIMEOUT     10000L

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

	printf("\nM:connection lost\n");
	printf("     cause: %s\n", cause);

	printf("M:reconnecting\n");
	conn_opts.keepAliveInterval = 30;
	conn_opts.cleansession = 1;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
		printf("M:failed to start connect, return code %d\n", rc);
	}
}

int msgarrvd(void *context, char *topicName, int topicLen, MQTTAsync_message *message)
{
	int i;
	char* payloadptr;
	
	printf("M:message arrived\n");
	printf("     topic: %s\n", topicName);
	printf("   message: ");
	
	payloadptr = message->payload;
	for(i=0; i<message->payloadlen; i++) {
		putchar(*payloadptr++);
	}
	putchar('\n');
	
	/* XXX: add mqtt event queue */
	
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

void onDisconnect(void *context, MQTTAsync_successData *response)
{
	printf("M:successful disconnection\n");
}

void onSend(void *context, MQTTAsync_successData *response)
{
	printf("M:message with token value %d delivery confirmed\n", response->token);
}

void onConnectFailure(void *context, MQTTAsync_failureData *response)
{
	printf("M:connect failed, rc %d\n", response ? response->code : 0);
}

void onSubscribe(void* context, MQTTAsync_successData* response)
{
	printf("M:subscribe succeeded\n");
}

void onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
	printf("M:subscribe failed, rc %d\n", response ? response->code : 0);
}

void onConnect(void *context, MQTTAsync_successData *response)
{
	MQTTAsync client = (MQTTAsync)context;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	printf("M:successful connection\n");

	/* publish online status */
	opts.onSuccess = onSend;
	opts.context = client;

	pubmsg.payload = ONLINE;
	pubmsg.payloadlen = strlen(ONLINE);
	pubmsg.qos = 2;
	pubmsg.retained = 1;
	deliveredtoken = 0;

	if ((rc = MQTTAsync_sendMessage(client, TOPIC_STATUS, &pubmsg, &opts)) != MQTTASYNC_SUCCESS) {
		printf("M:failed to start sendMessage, return code %d\n", rc);
 		exit(-1);	
	}
}

int mqtt_pub(char *topicName, char *payload, int payloadlen)
{
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	printf("M:publish topic : %s\n", topicName);

	/* publish topic */
	opts.onSuccess = onSend;
	opts.context = client;

	pubmsg.payload = payload;
	pubmsg.payloadlen = payloadlen;
	pubmsg.qos = 2;
	pubmsg.retained = 1;
	deliveredtoken = 0;

	if ((rc = MQTTAsync_sendMessage(client, topicName, &pubmsg, &opts)) != MQTTASYNC_SUCCESS) {
		printf("M:failed to start sendMessage, return code %d\n", rc);
 		exit(-1);	
	}

	return rc;
}

int mqtt_sub(char *topicName, int qos)
{
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	printf("M:subscribing to topic %s\nfor client %s using QoS%d\n\n", 
	       topicName, CLIENTID, qos);
	opts.onSuccess = onSubscribe;
	opts.onFailure = onSubscribeFailure;
	opts.context = client;

	if ((rc = MQTTAsync_subscribe(client, topicName, qos, &opts)) != MQTTASYNC_SUCCESS)
	{
		printf("M:failed to start subscribe, return code %d\n", rc);
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
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS){
		printf("M:failed to start connect, return code %d\n", rc);
		exit(-1);	
	}

 	return rc;
}
  
