#ifndef MQTT_H
#define MQTT_H

#ifdef __cplusplus
extern "C" {
#endif


#define ADDRESS     "tcp://121.42.52.171:1883"

#define CLIENTID    "100001Alice"
#define PEERID      "200002Bob"

/* topic status */
#define TOPIC_STATUS       "/"CLIENTID"/Status"
#define PEERID_STATUS      "/"PEERID"/Status"
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
 
int mqtt_init();

int mqtt_pub(char *topicName, char *payload, int payloadlen);

int mqtt_sub(char *topicName, int qos);
  
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
