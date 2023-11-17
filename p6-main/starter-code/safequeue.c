#include "safequeue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Helper function to swap two nodes
void swap(QueueNode *a, QueueNode *b)
{
    QueueNode temp = *a;
    *a = *b;
    *b = temp;
}

// Function to create a new queue
SafeQueue *create_queue(int capacity)
{
    SafeQueue *queue = (SafeQueue *)malloc(sizeof(SafeQueue));
    if (!queue)
        return NULL;

    queue->nodes = (QueueNode *)malloc(capacity * sizeof(QueueNode));
    if (!queue->nodes)
    {
        free(queue);
        return NULL;
    }

    queue->size = 0;
    queue->capacity = capacity;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);

    return queue;
}

// Function to resize the heap
int resize_heap(SafeQueue *queue)
{
    int new_capacity = queue->capacity * 2;
    QueueNode *new_nodes = (QueueNode *)realloc(queue->nodes, new_capacity * sizeof(QueueNode));
    if (!new_nodes)
        return -1; // Failed to resize

    queue->nodes = new_nodes;
    queue->capacity = new_capacity;
    return 0;
}

// Function to extract priority from the request path
int extract_priority(const char *path)
{
    int priority = 0;
    if (sscanf(path, "/%d", &priority) == 1)
    {
        return priority;
    }
    return -1; // Default or error priority
}

// Function to add work to the queue (heap)
void add_work(SafeQueue *queue, const char *request_path, int client_fd, char * buffer)
{
    printf("inside add work request path is %s\n", request_path); 
    printf("client fd is %d\n", client_fd); 
    printf("Client's Request in add work:\n");
    int i;
    for (i = 0; i < strlen(buffer); ++i) {
        // Print each character until a newline character is encountered
        if (buffer[i] != '\n') {
            putchar(buffer[i]);
        } else {
            // If a newline character is encountered, print a newline
            putchar('\n');
        }
    }
    printf("End of Client's Request in add work\n");


    pthread_mutex_lock(&queue->mutex);

    if (queue->size >= queue->capacity)
    {
        if (resize_heap(queue) != 0)
        {
            // Handle the failure to resize
            pthread_mutex_unlock(&queue->mutex);
            return;
        }
    }

    // Create a new QueueNode with extracted priority
    QueueNode node;
    node.request_path = strdup(request_path); // Allocate and copy the path
    node.buffer = strdup(buffer); // add the buffer to the node
    node.priority = extract_priority(request_path);
    node.client_fd = client_fd; 

    // Add new node at the end of the heap
    queue->nodes[queue->size] = node;
    int current = queue->size;

    // Heapify-up procedure
    while (current > 0 && queue->nodes[current].priority > queue->nodes[(current - 1) / 2].priority)
    {
        swap(&queue->nodes[current], &queue->nodes[(current - 1) / 2]);
        current = (current - 1) / 2;
    }

    queue->size++;
    pthread_cond_signal(&queue->cond); // Signal any waiting worker threads
    pthread_mutex_unlock(&queue->mutex);
}

// Function to get the highest priority work (heap root). Is used when a thread can afford to wait for work to become available. This is common in worker threads that have nothing else to do but process requests.
QueueNode get_work(SafeQueue *queue)
{
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == 0)
    {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }

    QueueNode node = queue->nodes[0]; // Get the root node (max priority)
    queue->nodes[0] = queue->nodes[queue->size - 1];
    queue->size--;

    // Heapify-down procedure
    int current = 0;
    while (current < queue->size / 2)
    {
        int largest = current;
        int left_child = 2 * current + 1;
        int right_child = 2 * current + 2;

        if (left_child < queue->size && queue->nodes[left_child].priority > queue->nodes[largest].priority)
        {
            largest = left_child;
        }

        if (right_child < queue->size && queue->nodes[right_child].priority > queue->nodes[largest].priority)
        {
            largest = right_child;
        }

        if (largest != current)
        {
            swap(&queue->nodes[current], &queue->nodes[largest]);
            current = largest;
        }
        else
        {
            break;
        }
    }

    pthread_mutex_unlock(&queue->mutex);
    return node;
}

// Non-blocking function to get the highest priority work. Is used when a thread needs to perform other tasks if no work is available, which is typical in listener threads that must remain responsive to new incoming connections or requests.
QueueNode get_work_nonblocking(SafeQueue *queue)
{
    pthread_mutex_lock(&queue->mutex);

    if (queue->size == 0)
    {
        pthread_mutex_unlock(&queue->mutex);
        return (QueueNode){ .client_fd = -1 }; // Return an empty node to indicate queue is empty
    }

    QueueNode node = queue->nodes[0];
    queue->nodes[0] = queue->nodes[queue->size - 1];
    queue->size--;

    int current = 0;
    while (current < queue->size / 2)
    {
        int largest = current;
        int left_child = 2 * current + 1;
        int right_child = 2 * current + 2;

        if (left_child < queue->size && queue->nodes[left_child].priority > queue->nodes[largest].priority)
        {
            largest = left_child;
        }

        if (right_child < queue->size && queue->nodes[right_child].priority > queue->nodes[largest].priority)
        {
            largest = right_child;
        }

        if (largest != current)
        {
            swap(&queue->nodes[current], &queue->nodes[largest]);
            current = largest;
        }
        else
        {
            break;
        }
    }

    pthread_mutex_unlock(&queue->mutex);
    return node;
}

// Function to destroy the queue and free resources
void destroy_queue(SafeQueue *queue)
{
    if (queue)
    {
        // Free each node's request_path
        for (int i = 0; i < queue->size; i++)
        {
            free(queue->nodes[i].request_path);
        }
        pthread_mutex_destroy(&queue->mutex);
        pthread_cond_destroy(&queue->cond);
        free(queue);
    }
}
int get_size(SafeQueue *queue){
    return queue->size; 
}
