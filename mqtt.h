#ifndef MQTT_H
#define MQTT_H

#ifdef __cplusplus
extern "C" {
#endif
 
int mqtt_init();

int mqtt_pub(char *topicName, char *payload, int payloadlen);

int mqtt_sub(char *topicName, int qos);
  
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
