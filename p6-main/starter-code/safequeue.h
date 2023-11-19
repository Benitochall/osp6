#ifndef SAFEQUEUE_H
#define SAFEQUEUE_H

#include <pthread.h>

// Define the struct for a queue node.
typedef struct {
    char* request_path; // Assuming request path is a string
    int priority;       // Priority of the request
    int client_fd;      // client fd
    char * buffer; // the buffer
    int delay; // the delay
} QueueNode;

// Define the struct for the priority queue.
typedef struct {
    QueueNode* nodes;           // Dynamic array of nodes
    int size;                   // Current number of elements in the heap
    int capacity;               // Current capacity of the heap
    pthread_mutex_t mutex;      // Mutex for synchronizing access
    pthread_cond_t cond;        // Condition variable for blocking `get_work`
} SafeQueue;

// Function declarations.
SafeQueue* create_queue(int capacity);
int add_work(SafeQueue *queue, const char *request_path, int client_fd, char * buffer, int priority, int delay);
QueueNode get_work(SafeQueue* queue);
QueueNode get_work_nonblocking(SafeQueue* queue);
void destroy_queue(SafeQueue* queue); // For cleanup
int get_size(SafeQueue *queue);

// Function to extract priority from the request path
int extract_priority(const char* path);

#endif // SAFEQUEUE_H
