#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "safequeue.h"

#include "proxyserver.h"

/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000
#define QUEUE_FULL_LOCAL -1 // Local definition for queue full

/*
 * Global configuration variables.
 * Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int num_listener;
int *listener_ports;
int num_workers;
char *fileserver_ipaddr;
int fileserver_port;
int max_queue_size;

SafeQueue *pq = NULL;

// syncronization variables

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t fill = PTHREAD_COND_INITIALIZER;

// a struct for the thread data

void send_error_response(int client_fd, status_code_t err_code, char *err_msg)
{
    http_start_response(client_fd, err_code);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    char *buf = malloc(strlen(err_msg) + 2);
    sprintf(buf, "%s\n", err_msg);
    http_send_string(client_fd, buf);
    return;
}

/*
 * forward the client request to the fileserver and
 * forward the fileserver response to the client
 */
void serve_request(int client_fd, char *buff_string)
{

    // this is the consumer

    printf("made it here\n");

    // need to pop the next element off the queue

    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1)
    {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }

    // create the full fileserver address
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;
    fileserver_address.sin_port = htons(fileserver_port);

    // connect to the fileserver
    int connection_status = connect(fileserver_fd, (struct sockaddr *)&fileserver_address,
                                    sizeof(fileserver_address));
    if (connection_status < 0)
    {
        // failed to connect to the fileserver
        printf("Failed to connect to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");
        return;
    }

    // successfully connected to the file server
    char *buffer = (char *)malloc(RESPONSE_BUFSIZE * sizeof(char));

    // forward the client request to the fileserver
    // int bytes_read = read(client_fd, buffer, RESPONSE_BUFSIZE);

    int ret = http_send_data(fileserver_fd, buff_string, strlen(buff_string));
    if (ret < 0)
    {
        printf("Failed to send request to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");
    }
    else
    {
        // forward the fileserver response to the client
        while (1)
        {
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);
            if (bytes_read <= 0) // fileserver_fd has been closed, break
                break;
            ret = http_send_data(client_fd, buffer, bytes_read);
            if (ret < 0)
            { // write failed, client_fd has been closed
                break;
            }
        }
    }

    // close the connection to the fileserver
    free(buffer);
    shutdown(fileserver_fd, SHUT_WR);
    close(fileserver_fd);
    close(client_fd);

    // Free resources and exit
    printf("are we exiting the function\n");
}

void handle_get_job_request(int client_fd, SafeQueue *queue)
{
    QueueNode node = get_work_nonblocking(queue); // gets the node from the queue

    if (node.client_fd == -1)
    {
        // The queue is empty, send an error response
        // printf("here we made it");
        char buf[50];
        sprintf(buf, "The Queue is empty");
        // need to send the error message
        // need and error code and and error message
        send_error_response(client_fd, QUEUE_EMPTY, buf);
        close(client_fd);
        return;
    }
    else
    {
        char buf[512];
        sprintf(buf, "%s\n", node.request_path);
        send_error_response(client_fd, OK, buf);
        close(client_fd);
        return;
    }
}

int server_fd; // this is a file descriptor for the server_fd
/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void *serve_forever(void *listener)
{
    // this is the producer thread

    struct http_client_request *rec = NULL;
    struct ThreadListener *l_curr = (struct ThreadListener *)listener;
    int *server_fd = &(l_curr->server_fd);

    // create a socket to listen
    *server_fd = socket(PF_INET, SOCK_STREAM, 0); //
    if (*server_fd == -1)
    {
        perror("Failed to create a new socket");
        exit(errno);
    }

    // manipulate options for the socket
    int socket_option = 1;
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1)
    {
        perror("Failed to set socket options");
        exit(errno);
    }

    int proxy_port = l_curr->listener_port;
    // create the full address of this proxyserver
    struct sockaddr_in proxy_address;
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(proxy_port); // listening port

    // bind the socket to the address and port number specified in
    if (bind(*server_fd, (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) == -1)
    {
        perror("Failed to bind on socket");
        exit(errno);
    }

    // starts waiting for the client to request a connection
    if (listen(*server_fd, 1024) == -1)
    {
        perror("Failed to listen on socket");
        exit(errno);
    }
    // spin up multiple threads that all call listen, each binded to their own port

    printf("Listening on port %d...\n", proxy_port);

    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);
    int client_fd;
    while (1)
    {
        client_fd = accept(*server_fd,
                           (struct sockaddr *)&client_address,
                           (socklen_t *)&client_address_length);
        if (client_fd < 0)
        {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n",
               inet_ntoa(client_address.sin_addr),
               client_address.sin_port);

        printf("before rec\n");
        rec = parse_client_request(client_fd); // this should parse the data for us

        printf("rec path is %s\n", rec->path);
        printf("rec delay is %d\n", rec->delay);
        printf("rec fd is %d\n", rec->fd);
        printf("rec buffer size is %ld\n", strlen(rec->buffer));
        printf("rec priority is %d\n", rec->priority);
        printf("------------------\n");

        if (rec == NULL)
        {
            perror("Error with parsing request\n");
            close(client_fd);
            continue;
        }

        // need to parse the buffer to get the path, the method and the delay

        if (strcmp(rec->path, GETJOBCMD) == 0)
        {
            // This is a GetJob request
            // Call a function to handle this specific request
            handle_get_job_request(client_fd, pq); // Assuming myQueue is your priority queue
            free(rec);
            continue;
        }
        else
        {

            // here I want to pass a queuenode object
            int check_add = add_work(pq, rec->path, rec->fd, rec->buffer, rec->priority, rec->delay); // need to change the sturct
            if (check_add == QUEUE_FULL_LOCAL)
            {
                send_error_response(client_fd, QUEUE_FULL, "Queue is full"); // QUEUE_FULL from proxyserver.h
                close(client_fd);
            }
            // if (check_add < 0){
            //     // we failed to add the work
            //     char buf[256];
            //     sprintf(buf, "Queue is full");
            //     send_error_response(client_fd, QUEUE_FULL, buf);
            //     close(client_fd);
            // }
            free(rec);
        }

        // close the connection to the client
    }

    shutdown(*server_fd, SHUT_RDWR);
    close(*server_fd);
    return NULL;
}

/*
 * Default settings for in the global configuration variables
 */
void default_settings()
{
    num_listener = 1;
    listener_ports = (int *)malloc(num_listener * sizeof(int));
    listener_ports[0] = 8000;

    num_workers = 1;

    fileserver_ipaddr = "127.0.0.1";
    fileserver_port = 3333;

    max_queue_size = 100;
}

void print_settings()
{
    printf("\t---- Setting ----\n");
    printf("\t%d listeners [", num_listener);
    for (int i = 0; i < num_listener; i++)
        printf(" %d", listener_ports[i]);
    printf(" ]\n");
    printf("\t%d workers\n", num_listener);
    printf("\tfileserver ipaddr %s port %d\n", fileserver_ipaddr, fileserver_port);
    printf("\tmax queue size  %d\n", max_queue_size);
    printf("\t  ----\t----\t\n");
}

void signal_callback_handler(int signum)
{
    printf("Caught signal %d: %s\n", signum, strsignal(signum));
    for (int i = 0; i < num_listener; i++)
    {
        if (close(server_fd) < 0)
            perror("Failed to close server_fd (ignoring)\n");
    }
    free(listener_ports);
    exit(0);
}

char *USAGE =
    "Usage: ./proxyserver [-l 1 8000] [-n 1] [-i 127.0.0.1 -p 3333] [-q 100]\n";

void exit_with_usage()
{
    fprintf(stderr, "%s", USAGE);
    exit(EXIT_SUCCESS);
}

void *worker_call(void *arg)
{
    SafeQueue *queue = (SafeQueue *)arg;

    while (1)
    { // You might want a mechanism to break out of this loop
        QueueNode node = get_work(queue);

        if (node.priority != -1)
        {
            // Process the request using serve_request
            // check to see if there is a delay
            if (node.delay > 0)
            {
                sleep(node.delay);
            }

            serve_request(node.client_fd, node.buffer); // Assuming serve_request takes a client_fd
            shutdown(node.client_fd, SHUT_WR);
            close(node.client_fd);

            // If node.request_path is dynamically allocated, remember to free it
            free(node.request_path);
            free(node.buffer);
        }

        // Check for a shutdown condition here
        // If shutdown, break out of the loop
    }

    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_callback_handler);

    /* Default settings */
    default_settings();

    int i;
    for (i = 1; i < argc; i++)
    {
        // this block of code sets up the number of listening ports  as well as the number of listeners
        if (strcmp("-l", argv[i]) == 0)
        {
            num_listener = atoi(argv[++i]);
            free(listener_ports);
            listener_ports = (int *)malloc(num_listener * sizeof(int));
            for (int j = 0; j < num_listener; j++)
            {
                listener_ports[j] = atoi(argv[++i]);
            }
        }
        else if (strcmp("-w", argv[i]) == 0)
        {
            num_workers = atoi(argv[++i]); // sets up number of workers
        }
        else if (strcmp("-q", argv[i]) == 0)
        {
            max_queue_size = atoi(argv[++i]); // sets up a maximum queue size
        }
        else if (strcmp("-i", argv[i]) == 0)
        {
            fileserver_ipaddr = argv[++i]; // the ip address
        }
        else if (strcmp("-p", argv[i]) == 0)
        {
            fileserver_port = atoi(argv[++i]); // the port of the file server
        }
        else
        {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            exit_with_usage();
        }
    }

    print_settings();
    // this is where we create the priority queue
    pq = create_queue(max_queue_size);

    // create the thread IDs
    pthread_t *listener_ids = (pthread_t *)malloc(num_listener * sizeof(pthread_t)); // create as many threadIDs as we need
    int *listener_server_fds = (int *)malloc(num_listener * sizeof(int));

    for (int j = 0; j < num_listener; j++)
    {
        struct ThreadListener *l1 = (struct ThreadListener *)malloc(sizeof(struct ThreadListener)); // create a theadData struct
        l1->server_fd = listener_server_fds[j];
        l1->listener_port = listener_ports[j];
        l1->thread_id = j;

        // Create the thread
        if (pthread_create(&listener_ids[j], NULL, serve_forever, (void *)l1) != 0)
        { // creates a thread for each listener
            perror("Failed to create thread");
            exit(EXIT_FAILURE);
        }
    }

    // Now create the workers
    pthread_t *worker_ids = (pthread_t *)malloc(num_listener * sizeof(pthread_t)); // create as many threadIDs as we need
    for (int j = 0; j < num_workers; j++)
    {
        if (pthread_create(&worker_ids[j], NULL, worker_call, (void *)pq) != 0)
        {
            perror("Failed to create thread");
            exit(EXIT_FAILURE);
        }
    }

    // wait for all the listenerers to finish
    for (int j = 0; j < num_listener; j++)
    {
        if (pthread_join(listener_ids[j], NULL) != 0)
        {
            perror("Failed to join thread");
            exit(EXIT_FAILURE);
        }
    }

    // wait for all num_workers
    for (int i = 0; i < num_workers; i++)
    {
        pthread_join(worker_ids[i], NULL);
    }

    // Clean up resources
    free(listener_ids);

    return EXIT_SUCCESS;
}
