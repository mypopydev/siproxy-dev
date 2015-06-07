#ifndef QUEUE_H_
#define QUEUE_H_

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @defgroup Queue Queue
 *
 * Little API for waitable queues, typically used for passing messages
 * between threads.
 *
 */

/**
 * @mainpage
 */

/**
 * A thread message.
 *
 * @ingroup Queue
 *
 * This is used for passing to #queue_get for retreive messages.
 * the date is stored in the data member, the message type in the  #msgtype.
 *
 * Typical:
 * @code
 * struct msg;
 * struct myfoo *foo;
 * while(1)
 *      ret = queue_get(&queue, NULL, &message);
 *      ..
 *      foo = msg.data;
 *      switch(msg.msgtype){
 *              ...
 *      }
 * }
 * @endcode
 *
 */
struct msg{
        /**
         * Holds the data.
         */
        void *data;
        /**
         * Holds the message type
         */
        int msgtype;
        /**
        * Holds the current queue lenght. Might not be meaningful if there's several readers
        */
        int qlength;

};


/**
 * A Queue
 *
 * @ingroup Queue
 *
 * You should threat this struct as opaque, never ever set/get any
 * of the variables. You have been warned.
 */
struct queue {
/**
 * Length of the queue, never set this, never read this.
 * Use #queue_length to read it.
 */
        long length;
/**
 * Mutex for the queue, never touch.
 */
        pthread_mutex_t mutex;
/**
 * Condition variable for the queue, never touch.
 */
        pthread_cond_t cond;
/**
 * Internal pointers for the queue, never touch.
 */
        struct msglist *first,*last;
/**
 * Internal cache of msglists
 */
	struct msglist *msgpool;
/**
 * No. of elements in the msgpool
 */
	long msgpool_length;
};

/**
 * Initializes a queue.
 *
 * @ingroup Queue
 *
 * queue_init initializes a new queue. A new queue must always
 * be initialized before it is used.
 *
 * @param queue Pointer to the queue that should be initialized
 * @return 0 on success see pthread_mutex_init
 */
int queue_init(struct queue *queue);

/**
 * Adds a message to a queue
 *
 * @ingroup Queue
 *
 * queue_add adds a "message" to the specified queue, a message
 * is just a pointer to a anything of the users choice. Nothing is copied
 * so the user must keep track on (de)allocation of the data.
 * A message type is also specified, it is not used for anything else than
 * given back when a message is retreived from the queue.
 *
 * @param queue Pointer to the queue on where the message should be added.
 * @param data the "message".
 * @param msgtype a long specifying the message type, choice of the user.
 * @return 0 on succes ENOMEM if out of memory EINVAL if queue is NULL
 */
int queue_add(struct queue *queue, void *data, int msgtype);

/**
 * Gets a message from a queue
 *
 * @ingroup Queue
 *
 * queue_get gets a message from the specified queue, it will block
 * the caling thread untill a message arrives, or the (optional) timeout occurs.
 * If timeout is NULL, there will be no timeout, and thread_queue_get will wait
 * untill a message arrives.
 *
 * struct timespec is defined as:
 * @code
 *      struct timespec {
 *                 long    tv_sec;         // seconds
 *                 long    tv_nsec;        // nanoseconds
 *             };
 * @endcode
 *
 * @param queue Pointer to the queue to wait on for a message.
 * @param timeout timeout on how long to wait on a message
 * @param msg pointer that is filled in with mesagetype and data
 *
 * @return 0 on success EINVAL if queue is NULL ETIMEDOUT if timeout occurs
 */
int queue_get(struct queue *queue, const struct timespec *timeout, struct msg *msg);


/**
 * Gets the length of a queue
 *
 * @ingroup Queue
 *
 * queue_length returns the number of messages waiting in the queue
 *
 * @param queue Pointer to the queue for which to get the length
 * @return the length(number of pending messages) in the queue
 */
long queue_length(struct queue *queue);

/**
 * @ingroup Queue
 * Cleans up the queue.
 *
 * queue_cleanup cleans up and destroys the queue.
 * This will remove all messages from a queue, and reset it. If
 * freedata is != 0 free(3) will be called on all pending messages in the queue
 * You cannot call this if there are someone currently adding or getting messages
 * from the queue.
 * After a queue have been cleaned, it cannot be used again untill #thread_queue_init
 * has been called on the queue.
 *
 * @param queue Pointer to the queue that should be cleaned
 * @param freedata set to nonzero if free(3) should be called on remaining
 * messages
 * @return 0 on success EINVAL if queue is NULL EBUSY if someone is holding any locks on the queue
 */
int queue_cleanup(struct queue *queue, int freedata);

#ifdef __cplusplus
}
#endif

#endif
