#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "xni.h"
#include "xni_internal.h"


#define PROTOCOL_NAME "tcp-nlmills-20120809"
#define ALIGN(val,align) (((val)+(align)-1UL) & ~((align)-1UL))


struct tcp_control_block {
  xni_allocate_fn_t allocate_fn;
  xni_free_fn_t free_fn;
  int num_sockets;
};

struct tcp_context {
  // inherited from struct xni_context
  struct xni_protocol *protocol;

  // added by struct tcp_context
  struct tcp_control_block control_block;
};

struct tcp_socket {
  int sockd;
  int busy;
  int eof;
};

struct tcp_connection {
  // inherited from struct xni_connection
  struct tcp_context *context;

  // added by struct tcp_connection
  struct tcp_target_buffer **send_buffers;  // NULL-terminated
  struct tcp_target_buffer **receive_buffers;  // NULL-terminated
  pthread_mutex_t buffer_mutex;
  pthread_cond_t buffer_cond;

  int destination;

  struct tcp_socket *sockets;
  int num_sockets;
  pthread_mutex_t socket_mutex;
  pthread_cond_t socket_cond;
};

struct tcp_target_buffer {
  // inherited from xni_target_buffer
  struct tcp_connection *connection;
  void *data;
  size_t target_offset;
  int data_length;

  // added by tcp_target_buffer
  int busy;
  void *header;
};


static const size_t TCP_DATA_MESSAGE_HEADER_SIZE = 12;


static void *tcp_default_allocate(size_t size, int alignment)
{
  void *memptr;

  if (posix_memalign(&memptr, alignment, size))
    return NULL;

  return memptr;
}

static void tcp_default_free(void *memptr)
{
  free(memptr);
}

int xni_allocate_tcp_control_block(xni_allocate_fn_t allocate_fn, xni_free_fn_t free_fn, int num_sockets, xni_control_block_t *cb_)
{
  struct tcp_control_block **cb = (struct tcp_control_block**)cb_;

  // either both `allocate' and `free' must be set or neither
  if ((allocate_fn || free_fn) && (!allocate_fn || !free_fn))
    return XNI_ERR;

  // progress would never be made...
  if (num_sockets < 1 && num_sockets != XNI_TCP_DEFAULT_NUM_SOCKETS)
    return XNI_ERR;

  struct tcp_control_block *tmp = calloc(1, sizeof(*tmp));
  tmp->allocate_fn = (allocate_fn ? allocate_fn : tcp_default_allocate);
  tmp->free_fn = (free_fn ? free_fn : tcp_default_free);
  tmp->num_sockets = num_sockets;

  *cb = tmp;
  return XNI_OK;
}

int xni_free_tcp_control_block(xni_control_block_t *cb_)
{
  struct tcp_control_block **cb = (struct tcp_control_block**)cb_;

  free(*cb);

  *cb = NULL;
  return XNI_OK;
}

static int tcp_context_create(xni_protocol_t proto_, xni_control_block_t cb_, xni_context_t *ctx_)
{
  struct xni_protocol *proto = proto_;
  struct tcp_control_block *cb = (struct tcp_control_block*)cb_;
  struct tcp_context **ctx = (struct tcp_context**)ctx_;

  assert(strcmp(proto->name, PROTOCOL_NAME) == 0);

  // fill in a new context
  struct tcp_context *tmp = calloc(1, sizeof(*tmp));
  tmp->protocol = proto;
  tmp->control_block = *cb;

  *ctx = tmp;
  return XNI_OK;
}

//TODO: do something with open connections
static int tcp_context_destroy(xni_context_t *ctx_)
{
  struct tcp_context **ctx = (struct tcp_context **)ctx_;

  free(*ctx);

  *ctx = NULL;
  return XNI_OK;
}

static int tcp_accept_connection(xni_context_t ctx_, struct xni_endpoint* local, int nbuf, size_t bufsiz, xni_connection_t* conn_)
{
  struct tcp_context *ctx = (struct tcp_context*)ctx_;
  struct tcp_connection **conn = (struct tcp_connection**)conn_;

  const int num_sockets = (ctx->control_block.num_sockets == XNI_TCP_DEFAULT_NUM_SOCKETS
                           ? nbuf
                           : ctx->control_block.num_sockets);

  // listening sockets
  int servers[num_sockets];
  for (int i = 0; i < num_sockets; i++)
    servers[i] = -1;

  // connected sockets
  struct tcp_socket *clients = malloc(num_sockets*sizeof(*clients));
  for (int i = 0; i < num_sockets; i++) {
    clients[i].sockd = -1;
    clients[i].busy = 0;
    clients[i].eof = 0;
  }

  struct tcp_connection *tmpconn = calloc(1, sizeof(*tmpconn));

  // pre-allocate target buffer objects
  struct tcp_target_buffer **target_buffers = calloc((nbuf + 1), sizeof(*target_buffers));
  for (int i = 0; i < nbuf; i++) {
    struct tcp_target_buffer *tb;
    const size_t structsz = sizeof(*tb) + TCP_DATA_MESSAGE_HEADER_SIZE + bufsiz + 512;  // 511 better?
    if ((tb = ctx->control_block.allocate_fn(structsz, 512)) == NULL)
      goto error_out;

    tb->connection = tmpconn;
    tb->data = (void*)ALIGN((uintptr_t)tb + sizeof(*tb) + TCP_DATA_MESSAGE_HEADER_SIZE, 512);
    tb->target_offset = 0;
    tb->data_length = -1;
    tb->busy = 0;
    tb->header = ((char*)tb)+sizeof(*tb);

    target_buffers[i] = tb;
  }
  
  // place all server sockets in the listening state
  for (int i = 0; i < num_sockets; i++) {
    if ((servers[i] = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      goto error_out;
    }

    int optval = 1;
    setsockopt(servers[i], SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(local->port + i));
    addr.sin_addr.s_addr = inet_addr(local->host);
    if (bind(servers[i], (struct sockaddr*)&addr, sizeof(addr))) {
      perror("bind");
      goto error_out;
    }

    if (listen(servers[i], 1)) {
      perror("listen");
      goto error_out;
    }
  }

  // wait for all connections from the client
  for (int i = 0; i < num_sockets; i++) {
    if ((clients[i].sockd = accept(servers[i], NULL, NULL)) == -1) {
      perror("accept");
      goto error_out;
    }
    close(servers[i]);
    servers[i] = -1;
  }

  tmpconn->context = ctx;
  tmpconn->receive_buffers = target_buffers;
  pthread_mutex_init(&tmpconn->buffer_mutex, NULL);
  pthread_cond_init(&tmpconn->buffer_cond, NULL);
  tmpconn->destination = 1;
  tmpconn->sockets = clients;
  tmpconn->num_sockets = num_sockets;
  pthread_mutex_init(&tmpconn->socket_mutex, NULL);
  pthread_cond_init(&tmpconn->socket_cond, NULL);

  *conn = tmpconn;
  return XNI_OK;

 error_out:
  // close any opened client sockets
  for (int i = 0; i < num_sockets; i++)
    if (clients[i].sockd != -1)
      close(clients[i].sockd);

  // close any opened server sockets
  for (int i = 0; i < num_sockets; i++)
    if (servers[i] != -1)
      close(servers[i]);

  // free any allocated target buffers
  for (struct tcp_target_buffer **ptr = target_buffers; *ptr; ptr++)
    ctx->control_block.free_fn(*ptr);

  free(target_buffers);
  free(tmpconn);
  free(clients);

  return XNI_ERR;
}

static int tcp_connect(xni_context_t ctx_, struct xni_endpoint* remote, int nbuf, size_t bufsiz, xni_connection_t* conn_)
{
  struct tcp_context *ctx = (struct tcp_context*)ctx_;
  struct tcp_connection **conn = (struct tcp_connection**)conn_;

  const int num_sockets = (ctx->control_block.num_sockets == XNI_TCP_DEFAULT_NUM_SOCKETS
                           ? nbuf
                           : ctx->control_block.num_sockets);

  // connected sockets
  struct tcp_socket *servers = malloc(num_sockets*sizeof(*servers));
  for (int i = 0; i < num_sockets; i++) {
    servers[i].sockd = -1;
    servers[i].busy = 0;
    servers[i].eof = 1;
  }
  
  struct tcp_connection *tmpconn = calloc(1, sizeof(*tmpconn));

  // pre-allocate target buffer objects
  struct tcp_target_buffer **target_buffers = calloc((nbuf + 1), sizeof(*target_buffers));
  for (int i = 0; i < nbuf; i++) {
    struct tcp_target_buffer *tb;
    const size_t structsz = sizeof(*tb) + TCP_DATA_MESSAGE_HEADER_SIZE + bufsiz + 512;  // 511 better?
    if ((tb = ctx->control_block.allocate_fn(structsz, 512)) == NULL)
      goto error_out;

    tb->connection = tmpconn;
    tb->data = (void*)ALIGN((uintptr_t)tb + sizeof(*tb) + TCP_DATA_MESSAGE_HEADER_SIZE, 512);
    tb->target_offset = 0;
    tb->data_length = -1;
    tb->busy = 0;
    tb->header = ((char*)tb)+sizeof(*tb);

    target_buffers[i] = tb;
  }

  for (int i = 0; i < num_sockets; i++) {
    if ((servers[i].sockd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      goto error_out;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(remote->port + i));
    addr.sin_addr.s_addr = inet_addr(remote->host);
    if (connect(servers[i].sockd, (struct sockaddr*)&addr, sizeof(addr))) {
      perror("connect");
      goto error_out;
    }
  }

  //TODO: allocate target buffer list and attach

  tmpconn->context = ctx;
  tmpconn->send_buffers = target_buffers;
  pthread_mutex_init(&tmpconn->buffer_mutex, NULL);
  pthread_cond_init(&tmpconn->buffer_cond, NULL);
  tmpconn->destination = 0;
  tmpconn->sockets = servers;
  tmpconn->num_sockets = num_sockets;
  pthread_mutex_init(&tmpconn->socket_mutex, NULL);
  pthread_cond_init(&tmpconn->socket_cond, NULL);

  *conn = tmpconn;
  return XNI_OK;

 error_out:
  // close any opened server sockets
  for (int i = 0; i < num_sockets; i++)
    if (servers[i].sockd != -1)
      close(servers[i].sockd);

  // free any allocated target buffers
  for (struct tcp_target_buffer **ptr = target_buffers; *ptr; ptr++)
    ctx->control_block.free_fn(*ptr);

  free(target_buffers);
  free(tmpconn);
  free(servers);

  return XNI_ERR;
}

//XXX: this is not going to be thread safe??
//alternatives: a flag that signals shutdown state
//and freeing the buffers as they become available on freelist
static int tcp_close_connection(xni_connection_t *conn_)
{
  struct tcp_connection **conn = (struct tcp_connection**)conn_;

  struct tcp_connection *c = *conn;

  for (int i = 0; i < c->num_sockets; i++)
    if (c->sockets[i].sockd != -1)
      close(c->sockets[i].sockd);

  struct tcp_target_buffer **buffers = (c->destination ? c->receive_buffers : c->send_buffers);
  while (*buffers++)
    c->context->control_block.free_fn(*buffers);

  free(c);

  *conn = NULL;
  return XNI_OK;
}

static int tcp_request_target_buffer(xni_connection_t conn_, xni_target_buffer_t *targetbuf_)
{
  struct tcp_connection *conn = (struct tcp_connection*)conn_;
  struct tcp_target_buffer **targetbuf = (struct tcp_target_buffer**)targetbuf_;

  if (conn->destination)
    return XNI_ERR;

  struct tcp_target_buffer *tb = NULL;
  pthread_mutex_lock(&conn->buffer_mutex);
  while (tb == NULL) {
    for (struct tcp_target_buffer **ptr = conn->send_buffers; *ptr != NULL; ptr++)
      if (!(*ptr)->busy) {
        tb = *ptr;
        tb->busy = 1;
        break;
      }
    if (tb == NULL)
      pthread_cond_wait(&conn->buffer_cond, &conn->buffer_mutex);
  }
  pthread_mutex_unlock(&conn->buffer_mutex);
    
  *targetbuf = tb;
  return XNI_OK;
}

//TODO: what happens on error? stream state is trashed
static int tcp_send_target_buffer(xni_target_buffer_t *targetbuf_)
{
  struct tcp_target_buffer **targetbuf = (struct tcp_target_buffer**)targetbuf_;
  struct tcp_target_buffer *tb = *targetbuf;

  if (tb->connection->destination || tb->data_length < 1)
    return XNI_ERR;

  // encode the message header
  uint64_t tmp64 = tb->target_offset;
  memcpy(tb->header, &tmp64, 8);
  uint32_t tmp32 = tb->data_length;
  memcpy(((char*)tb->header)+8, &tmp32, 4);

  // locate a free socket
  struct tcp_socket *socket = NULL;
  pthread_mutex_lock(&tb->connection->socket_mutex);
  while (socket == NULL) {
    for (int i = 0; i < tb->connection->num_sockets; i++)
      if (!tb->connection->sockets[i].busy) {
        socket = tb->connection->sockets+i;
        socket->busy = 1;
        break;
      }
    if (socket == NULL)
      pthread_cond_wait(&tb->connection->socket_cond, &tb->connection->socket_mutex);
  }
  pthread_mutex_unlock(&tb->connection->socket_mutex);

  // send the message (header + data payload)
  const size_t total = (size_t)((char*)tb->data - (char*)tb->header) + tb->data_length;
  for (size_t sent = 0; sent < total;) {
    ssize_t cnt = send(socket->sockd, (char*)tb->header+sent, (total - sent), 0);
    //TODO: fix adding after EINTR logic
    if (cnt != -1)
      sent += cnt;
    else if (errno != EINTR) {
      perror("send");
      return XNI_ERR;
    }
  }

  // mark the socket as free
  pthread_mutex_lock(&tb->connection->socket_mutex);
  socket->busy = 0;
  pthread_cond_signal(&tb->connection->socket_cond);
  pthread_mutex_unlock(&tb->connection->socket_mutex);

  // mark the buffer as free
  pthread_mutex_lock(&tb->connection->buffer_mutex);
  tb->busy = 0;
  pthread_cond_signal(&tb->connection->buffer_cond);
  pthread_mutex_unlock(&tb->connection->buffer_mutex);

  *targetbuf = NULL;
  return XNI_OK;
}

static int tcp_receive_target_buffer(xni_connection_t conn_, xni_target_buffer_t *targetbuf_)
{
  struct tcp_connection *conn = (struct tcp_connection*)conn_;
  struct tcp_target_buffer **targetbuf = (struct tcp_target_buffer**)targetbuf_;
  int return_code = XNI_ERR;

  // grab a free buffer
  struct tcp_target_buffer *tb = NULL;
  pthread_mutex_lock(&conn->buffer_mutex);
  while (tb == NULL) {
    for (struct tcp_target_buffer **ptr = conn->receive_buffers; *ptr; ptr++) {
      if (!(*ptr)->busy) {
        tb = *ptr;
        tb->busy = 1;
        break;
      }
    }
    if (tb == NULL)
      pthread_cond_wait(&conn->buffer_cond, &conn->buffer_mutex);
  }
  pthread_mutex_unlock(&conn->buffer_mutex);

  struct tcp_socket *socket = NULL;
  while (socket == NULL) {
    // prepare to call select(2)
    fd_set outfds;
    FD_ZERO(&outfds);
    int maxsd = -1;

    pthread_mutex_lock(&conn->socket_mutex);
    for (int i = 0; i < conn->num_sockets; i++) {
      if (!conn->sockets[i].eof && conn->sockets[i].sockd != -1) {
        FD_SET(conn->sockets[i].sockd, &outfds);
        if (conn->sockets[i].sockd > maxsd)
          maxsd = conn->sockets[i].sockd;
      }
    }
    pthread_mutex_unlock(&conn->socket_mutex);
    if (maxsd == -1) {
      return_code = XNI_EOF;
      goto buffer_out;
    }

    if (select((maxsd + 1), &outfds, NULL, NULL, NULL) < 1)
      goto buffer_out;

    pthread_mutex_lock(&conn->socket_mutex);
    for (int i = 0; i < conn->num_sockets; i++)
      if (FD_ISSET(conn->sockets[i].sockd, &outfds) &&
          !conn->sockets[i].busy &&
          !conn->sockets[i].eof &&
          conn->sockets[i].sockd != -1) {
        socket = conn->sockets+i;
        socket->busy = 1;
        break;
      }
    //XXX: yes, no, maybe??
    //TODO: really only want to wait in case all sockets were busy
    //if (socket == NULL)
    //  pthread_cond_wait(&conn->socket_cond, &conn->socket_mutex);
    pthread_mutex_unlock(&conn->socket_mutex);
  }

  char *recvbuf = (char*)tb->header;
  size_t total = (size_t)((char*)tb->data - (char*)tb->header);
  for (size_t received = 0; received < total;) {
    ssize_t cnt = recv(socket->sockd, recvbuf+received, (total - received), 0);
    if (cnt == 0) {  //TODO: handle true EOF; true EOF only on first read
      return_code = XNI_EOF;
      goto socket_out;
    } else if (cnt == -1) {  //TODO: handle EINTR
      perror("recv");
      goto socket_out;
    } else
      received += cnt;
  }

  uint64_t target_offset;
  memcpy(&target_offset, recvbuf, 8);
  uint32_t data_length;
  memcpy(&data_length, recvbuf+8, 4);

  recvbuf = (char*)tb->data;
  total = data_length;
  for (size_t received = 0; received < total;) {
    ssize_t cnt = recv(socket->sockd, recvbuf+received, (total - received), 0);
    if (cnt == 0)  // failure EOF
      goto socket_out;
    else if (cnt == -1) {  //TODO: EINTR
      perror("recv");
      goto socket_out;
    } else
      received += cnt;
  }

  //TODO: sanity checks (e.g. tb->connection)
  tb->target_offset = target_offset;
  tb->data_length = (int)data_length;
  
  *targetbuf = tb;
  return_code = XNI_OK;

 socket_out:
  // mark the socket as free
  pthread_mutex_lock(&conn->socket_mutex);
  socket->busy = 0;
  if (return_code == XNI_EOF)
    socket->eof = 1;
  //TODO: re-enable this
  //pthread_cond_signal(&conn->socket_cond);
  pthread_mutex_unlock(&conn->socket_mutex);

 buffer_out:
  if (return_code != XNI_OK) {
    // mark the buffer as free
    pthread_mutex_lock(&conn->buffer_mutex);
    tb->busy = 0;
    pthread_cond_signal(&conn->buffer_cond);
    pthread_mutex_unlock(&conn->buffer_mutex);
  }

  return return_code;
}

static int tcp_release_target_buffer(xni_target_buffer_t *targetbuf_)
{
  struct tcp_target_buffer **targetbuf = (struct tcp_target_buffer**)targetbuf_;
  struct tcp_target_buffer *tb = *targetbuf;

  tb->target_offset = 0;
  tb->data_length = -1;

  pthread_mutex_lock(&tb->connection->buffer_mutex);
  tb->busy = 0;
  pthread_cond_signal(&tb->connection->buffer_cond);
  pthread_mutex_unlock(&tb->connection->buffer_mutex);
                         
  *targetbuf = NULL;
  return XNI_OK;
}


static struct xni_protocol protocol_tcp = {
  .name = PROTOCOL_NAME,
  .context_create = tcp_context_create,
  .context_destroy = tcp_context_destroy,
  .accept_connection = tcp_accept_connection,
  .connect = tcp_connect,
  .close_connection = tcp_close_connection,
  .request_target_buffer = tcp_request_target_buffer,
  .send_target_buffer = tcp_send_target_buffer,
  .receive_target_buffer = tcp_receive_target_buffer,
  .release_target_buffer = tcp_release_target_buffer,
};

struct xni_protocol *xni_protocol_tcp = &protocol_tcp;