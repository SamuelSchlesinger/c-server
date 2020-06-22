#include <errno.h>
#include <error.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*****************************************************************************

Title: Playing with Servers in C
Copyright: (c) 2020 Samuel Schlesinger
Maintainer: sgschlesinger@gmail.com
License: MIT

*****************************************************************************/

// unrecoverable error with useful message
#define panic(msg) {\
  int errno_saved = errno;\
  fprintf(stderr,\
    "FAILURE!\nfile: %s,\nline: %d,\nfunction: %s,\nerrno: %d,\nerrstr: %s",\
     __FILE__,\
    __LINE__,\
    __func__,\
    errno_saved,\
    strerror(errno_saved)\
    );\
  exit(EXIT_FAILURE);\
}

// essential information about a client
struct client {
  int socket;
  // socket the client is connected to
  struct sockaddr_in address;
  // address the client connected from
};

// a locked buffer/client pair
struct client_buffer {  
  // a lock that anyone who uses this must take
  sem_t lock; 
  // a buffer containing all of the we've read from a client
  char* buffer;
  // the size of the allocated buffer
  int size;
  // the number of bytes read into the buffer so far
  int bytes_read;
  // the client we are reading from
  struct client client;
};

struct client_buffer* make_client_buffer
  ( // the client to read from
    struct client client
    // the initial size of the buffer
  , int initial_size
  ) 
{
  if (initial_size < 0)
    panic("initial_size of client_buffer less than 0")
  struct client_buffer* client_buffer = (struct client_buffer*) malloc(sizeof(struct client_buffer));
  client_buffer->buffer = (char *) malloc(sizeof(char) * initial_size);
  client_buffer->size = initial_size;
  client_buffer->bytes_read = 0;
  client_buffer->client = client;
  if (sem_init
        ( // the location of the lock
          &client_buffer->lock,
          // this lock is shared with all threads 
          0,
          // value of the lock
          1) == -1)
    panic("failed to initialize client_buffer semaphore")
  return client_buffer;
}

// caller must completely own this buffer, as its contents may be ripped out
int read_available(struct client_buffer *client_buffer) {
  int available;
  // how many bytes are available on the socket?
  ioctl(client_buffer->client.socket, FIONREAD, &available);
  if (available > 0) {
    // make sure we have enough room
    if (client_buffer->size - client_buffer->bytes_read > available) {
      // resize buffer if necessary
      client_buffer->buffer = realloc(client_buffer->buffer, sizeof(char) * client_buffer->bytes_read + available + client_buffer->size);
    }
    int length = read(client_buffer->client.socket, client_buffer->buffer + client_buffer->bytes_read, available);
    if (length != available)
      panic("reading available bytes")
    return available;
  }
  return 0;
}

// safely slices the client_buffer, checking for thread safety
void slice_client_buffer
  ( 
    // the client_buffer we want a slice of
    struct client_buffer* client_buffer,
    // the leftmost index of the slice (unchecked)
    int l,
    // the rightmost index of the slice (unchecked)
    int r,
    // the location we are copying to
    char* to
  )
{
  sem_wait(&client_buffer->lock);

  int i;
  for (i = l; i <= r; i++) {
    to[i] = client_buffer->buffer[i];
  }

  sem_post(&client_buffer->lock);
}

// a configuration for the server
struct config {
  // address the server will live on
  struct sockaddr_in address;
  // maximum number of connections allowed to be pending for the server's socket
  unsigned short connection_backlog;
  // number of worker threads you want to be onliny
  unsigned int nworkers;
  // maximum number of requests we process simultaneously
  unsigned int  nrequests;
};

// make a new config
struct config make_config
  ( // the port to bind to
    uint16_t port
    // the ip address to bind to
  , long ip
    // the number of connections to allow in the backlog of the socket
  , short connection_backlog
    // the number of workers which process requests
  , short nworkers
    // the maximum number of requests which we will process at once
  , short nrequests
  ) 
{
  struct config config;
  config.address.sin_family = AF_INET;
  config.address.sin_port = htons(port);
  config.address.sin_addr.s_addr = htonl(ip);
  config.connection_backlog = connection_backlog;
  config.nworkers = nworkers;
  config.nrequests = nrequests;
  return config;
};

// handle to a servant
struct server {
  // the socket the server is listening on
  int socket;
  // the configuration of the server
  struct config config;
};

// make a new server
struct server initialize_server
  ( // the configuration of the server
    struct config config
  ) 
{
  struct server server;
  server.config = config;

  // create a new TCP socket
  if ((server.socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    panic("failed to create socket")

  // bind the TCP socket to the IP address and port specified in the config
  if (bind(server.socket, (struct sockaddr *) &server.config.address, sizeof(server.config.address)) == -1)
    panic("failed to bind socket")

  // put the TCP socket into a passive state, accepting peer connections
  if (listen(server.socket, config.connection_backlog) == -1)
    panic("failed to listen on socket")

  return server;
}

// running a server
void run_server(void (*handle_client)(struct client_buffer* client_buffer), struct server server) {
  // loop, waiting for connections and adding them to the queue
  while (true) {
    struct client client;
    socklen_t client_address_size = (socklen_t) sizeof(struct sockaddr_in);

    // accept a peer connection
    if ((client.socket = accept(server.socket, (struct sockaddr *) &client.address, &client_address_size)) == -1)
      panic("failed to accept connection")
    
    // handle the client requests
    handle_client(make_client_buffer(client, 1024));
  }
}

// handling an individual client
void handle_client(struct client_buffer* client_buffer) {
  sem_wait(&client_buffer->lock);
  int read = 0;
  while (read == 0) {
    read = read_available(client_buffer);
    printf(".");
  }
  printf("\nReceived %d bytes\n", read);
  sem_post(&client_buffer->lock);
}

int main() {
  struct config config = make_config
    ( 8080
    , INADDR_ANY
    , 500
    , PROCESSOR_COUNT
    , PROCESSOR_COUNT * 100
    );
  struct server server = initialize_server(config);
  run_server(handle_client, server);
}
