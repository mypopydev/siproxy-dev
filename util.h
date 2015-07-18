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


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
