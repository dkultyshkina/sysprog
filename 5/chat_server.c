#include "chat_server.h"
#include "chat.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void trim_server_message(char *str) {
  if (str == NULL) {
    return;
  }
  char *start = str;
  while (isspace(*start)) {
    start++;
  }
  if (start != str) {
    memmove(str, start, strlen(start) + 1);
  }
  char *end = str + strlen(str) - 1;
  while (end >= str && isspace(*end)) {
    end--;
  }
  *(end + 1) = '\0';
}

bool is_empty_server_message(const char *str) {
  while (*str) {
    if (!isspace(*str)) {
      return false;
    }
    str++;
  }
  return true;
}

static bool update_peer_events(struct peer_data *pd, uint32_t new_events) {
  new_events |= EPOLLET;
  if (pd->current_events == new_events) {
    return true;
  }
  struct epoll_event ev;
  ev.events = new_events;
  ev.data.ptr = pd;
  if (epoll_ctl(pd->server->epoll_fd, EPOLL_CTL_DEL, pd->fd, NULL) == -1 &&
      errno != ENOENT) {
    return false;
  }
  if (epoll_ctl(pd->server->epoll_fd, EPOLL_CTL_ADD, pd->fd, &ev) == 0) {
    pd->current_events = new_events;
    return true;
  } else {
    return false;
  }
  if (pd->peer != NULL) {
    pd->peer->is_closed = true;
  }
  return false;
}

struct chat_server *chat_server_new(void) {
  struct chat_server *server = calloc(1, sizeof(*server));
  if (server == NULL) {
    return NULL;
  }
  server->socket = -1;
  server->epoll_fd = -1;
  server->msg_capacity = 8;
  server->messages = malloc(server->msg_capacity * sizeof(struct chat_message));
  if (server->messages == NULL) {
    free(server);
    return NULL;
  }
  server->msg_count = 0;
  server->peer_count = 0;
  return server;
}

static struct chat_peer *create_peer(int socket) {
  struct chat_peer *peer = calloc(1, sizeof(struct chat_peer));
  if (peer == NULL) {
    return NULL;
  }
  peer->socket = socket;
  peer->partial_capacity = 1024;
  peer->partial_in = malloc(peer->partial_capacity);
  if (peer->partial_in == NULL) {
    free(peer);
    return NULL;
  }
  peer->partial_size = 0;

  peer->out_capacity = 1024;
  peer->out_buffer = malloc(peer->out_capacity);
  if (peer->out_buffer == NULL) {
    free(peer->partial_in);
    free(peer);
    return NULL;
  }
  peer->out_size = 0;
  peer->p_data = NULL;
  peer->is_closed = false;
  return peer;
}

static void free_peer(struct chat_peer *peer) {
  if (peer == NULL) {
    return;
  }
  if (peer->p_data != NULL && peer->p_data->server != NULL &&
      peer->p_data->server->epoll_fd >= 0) {
    epoll_ctl(peer->p_data->server->epoll_fd, EPOLL_CTL_DEL, peer->socket,
              NULL);
  }
  close(peer->socket);
  free(peer->partial_in);
  free(peer->out_buffer);
  free(peer->p_data);
  free(peer);
}

void chat_server_delete(struct chat_server *server) {
  if (server == NULL) {
    return;
  }
  free(server->listener_pd);
  if (server->epoll_fd >= 0) {
    close(server->epoll_fd);
  }
  if (server->socket >= 0) {
    close(server->socket);
  }
  for (size_t i = 0; i < server->peer_count; i++) {
    free_peer(server->peers[i]);
  }
  if (server->messages) {
    for (size_t i = 0; i < server->msg_count; i++) {
      free(server->messages[i].data);
    }
    free(server->messages);
  }
  free(server);
}

int chat_server_listen(struct chat_server *server, uint16_t port) {
  if (server->socket >= 0) {
    return CHAT_ERR_ALREADY_STARTED;
  }
  int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (sock < 0) {
    return CHAT_ERR_SYS;
  }
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  int optval = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
    close(sock);
    return CHAT_ERR_SYS;
  }
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
    close(sock);
    return (errno == EADDRINUSE) ? CHAT_ERR_PORT_BUSY : CHAT_ERR_SYS;
  }
  if (listen(sock, SOMAXCONN) < 0) {
    close(sock);
    return CHAT_ERR_SYS;
  }
  server->socket = sock;
  server->epoll_fd = epoll_create1(0);
  if (server->epoll_fd == -1) {
    close(server->socket);
    return CHAT_ERR_SYS;
  }
  struct peer_data *pd = malloc(sizeof(struct peer_data));
  if (pd == NULL) {
    close(server->epoll_fd);
    close(server->socket);
    return CHAT_ERR_SYS;
  }
  pd->fd = server->socket;
  pd->peer = NULL;
  pd->server = server;
  pd->current_events = 0;
  if (update_peer_events(pd, EPOLLIN | EPOLLET) == false) {
    free(pd);
    close(server->epoll_fd);
    close(server->socket);
    return CHAT_ERR_SYS;
  }
  server->listener_pd = pd;
  return 0;
}

void get_in_data(struct chat_server *server, struct chat_peer *peer,
                 const char *data, size_t size) {
  size_t new_size = peer->partial_size + size;
  if (new_size < peer->partial_size) {
    peer->is_closed = true;
    return;
  }
  if (new_size + 1 > peer->partial_capacity) {
    size_t new_capacity = new_size * 2 + 1;
    if (new_capacity < new_size + 1) {
      peer->is_closed = true;
      return;
    }
    char *new_buf = realloc(peer->partial_in, new_capacity);
    if (new_buf == NULL) {
      peer->is_closed = true;
      return;
    }
    peer->partial_in = new_buf;
    peer->partial_capacity = new_capacity;
  }
  memcpy(peer->partial_in + peer->partial_size, data, size);
  peer->partial_size = new_size;
  peer->partial_in[peer->partial_size] = '\0';
  char *start = peer->partial_in;
  while (1) {
    char *end =
        memchr(start, '\n', peer->partial_size - (start - peer->partial_in));
    if (end == NULL) {
      break;
    }
    size_t msg_len = end - start;
    char *msg = malloc(msg_len + 1);
    if (msg == NULL) {
      peer->is_closed = true;
      break;
    }
    memcpy(msg, start, msg_len);
    msg[msg_len] = '\0';
    trim_server_message(msg);
    if (!is_empty_server_message(msg)) {
      if (server->msg_count == server->msg_capacity) {
        size_t new_msg_capacity = server->msg_capacity * 2;
        if (new_msg_capacity == 0)
          new_msg_capacity = 8;
        if (new_msg_capacity < server->msg_capacity) {
          free(msg);
          peer->is_closed = true;
          break;
        }
        struct chat_message *new_msgs = realloc(
            server->messages, new_msg_capacity * sizeof(struct chat_message));
        if (new_msgs == NULL) {
          free(msg);
          peer->is_closed = true;
          break;
        }
        server->messages = new_msgs;
        server->msg_capacity = new_msg_capacity;
      }
      server->messages[server->msg_count].data = msg;
      server->messages[server->msg_count].size = strlen(msg);
      server->msg_count++;
    } else {
      free(msg);
    }
    start = end + 1;
  }
  size_t remaining = peer->partial_size - (start - peer->partial_in);
  if (remaining > 0) {
    memmove(peer->partial_in, start, remaining);
  } else if (start != peer->partial_in) {
    peer->partial_in[peer->partial_size] = '\0';
  }
  peer->partial_size = remaining;
}

struct chat_message *chat_server_pop_next(struct chat_server *server) {
  if (server == NULL || server->msg_count == 0) {
    return NULL;
  }
  struct chat_message *msg = malloc(sizeof(struct chat_message));
  if (msg == NULL) {
    return NULL;
  }
  msg->data = strdup(server->messages[0].data);
  msg->size = server->messages[0].size;
  free(server->messages[0].data);
  memmove(server->messages, server->messages + 1,
          (server->msg_count - 1) * sizeof(*server->messages));
  server->msg_count--;
  return msg;
}

int chat_server_update(struct chat_server *server, double timeout) {
  if (server == NULL || server->socket < 0 || server->epoll_fd < 0) {
    return CHAT_ERR_NOT_STARTED;
  }
  int epoll_timeout;
  if (timeout < 0) {
    epoll_timeout = -1;
  } else {
    epoll_timeout = (int)(timeout * 1000);
  }
  struct epoll_event events[30];
  int number = epoll_wait(server->epoll_fd, events, 30, epoll_timeout);
  if (number == 0) {
    return CHAT_ERR_TIMEOUT;
  }
  if (number == -1) {
    if (errno == EINTR) {
      return CHAT_ERR_TIMEOUT;
    }
    return CHAT_ERR_SYS;
  }
  struct chat_peer *peers_to_close[100];
  size_t number_peers = 0;
  for (int i = 0; i < number; i++) {
    struct peer_data *pd = (struct peer_data *)events[i].data.ptr;
    struct chat_peer *peer = pd->peer;
    bool current = false;
    if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
      current = true;
    }
    if (pd->peer == NULL) {
      if (current) {
        return CHAT_ERR_SYS;
      }
      while (true) {
        int client_sock = accept(server->socket, NULL, NULL);
        if (client_sock < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          continue;
        }
        if (server->peer_count >= 100) {
          close(client_sock);
          continue;
        }
        int flags = fcntl(client_sock, F_GETFL, 0);
        if (flags == -1 ||
            fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
          close(client_sock);
          continue;
        }
        struct chat_peer *new_peer = create_peer(client_sock);
        if (new_peer == NULL) {
          close(client_sock);
          continue;
        }
        new_peer->p_data = malloc(sizeof(*new_peer->p_data));
        if (new_peer->p_data == NULL) {
          free_peer(new_peer);
          continue;
        }
        new_peer->p_data->fd = client_sock;
        new_peer->p_data->peer = new_peer;
        new_peer->p_data->server = server;
        new_peer->p_data->current_events = 0;
        if (update_peer_events(new_peer->p_data, EPOLLIN | EPOLLET) == false) {
          free(new_peer->p_data);
          free_peer(new_peer);
          continue;
        }
        server->peers[server->peer_count++] = new_peer;
      }
    } else {
      if (peer->is_closed) {
        continue;
      }
      if (current == true) {
        peer->is_closed = true;
      }
      if (peer->is_closed == false && (events[i].events & EPOLLIN)) {
        char buffer[1024];
        ssize_t received;
        size_t msg_count = server->msg_count;
        while ((received = recv(peer->socket, buffer, sizeof(buffer), 0)) > 0) {
          get_in_data(server, peer, buffer, received);
          if (peer->is_closed)
            break;
        }
        if (received == 0 ||
            (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          peer->is_closed = true;
        }
        if (server->msg_count > msg_count && peer->is_closed == false) {
          for (size_t m = msg_count; m < server->msg_count; m++) {
            struct chat_message *new_msg = &server->messages[m];
            for (size_t j = 0; j < server->peer_count; ++j) {
              struct chat_peer *other_peer = server->peers[j];
              if (other_peer == NULL || other_peer->is_closed == true ||
                  other_peer == peer) {
                continue;
              }
              size_t required_capacity =
                  other_peer->out_size + new_msg->size + 1;
              if (required_capacity < other_peer->out_size) {
                other_peer->is_closed = true;
                continue;
              }
              if (required_capacity > other_peer->out_capacity) {
                size_t new_out_capacity = required_capacity * 2;
                if (new_out_capacity < required_capacity) {
                  other_peer->is_closed = true;
                  continue;
                }
                char *new_out_buffer =
                    realloc(other_peer->out_buffer, new_out_capacity);
                if (new_out_buffer == NULL) {
                  other_peer->is_closed = true;
                  continue;
                }
                other_peer->out_buffer = new_out_buffer;
                other_peer->out_capacity = new_out_capacity;
              }
              memcpy(other_peer->out_buffer + other_peer->out_size,
                     new_msg->data, new_msg->size);
              other_peer->out_size += new_msg->size;
              other_peer->out_buffer[other_peer->out_size++] = '\n';
              update_peer_events(other_peer->p_data,
                                 EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP);
            }
          }
        }
      }
      if (peer->is_closed == false && (events[i].events & EPOLLOUT)) {
        if (peer->out_size > 0) {
          ssize_t sent = send(peer->socket, peer->out_buffer, peer->out_size,
                              MSG_NOSIGNAL);
          if (sent > 0) {
            memmove(peer->out_buffer, peer->out_buffer + sent,
                    peer->out_size - sent);
            peer->out_size -= sent;
            if (peer->out_size == 0) {
              update_peer_events(pd, EPOLLIN | EPOLLET | EPOLLRDHUP);
            }
          } else if (sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            peer->is_closed = true;
          }
        } else {
          update_peer_events(pd, EPOLLIN | EPOLLET | EPOLLRDHUP);
        }
      }
      if (peer->is_closed == true) {
        if (number_peers < 100) {
          peers_to_close[number_peers++] = peer;
        } else {
          epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, peer->socket, NULL);
          for (size_t j = 0; j < server->peer_count; j++) {
            if (server->peers[j] == peer) {
              memmove(&server->peers[j], &server->peers[j + 1],
                      (server->peer_count - j - 1) * sizeof(*server->peers));
              server->peer_count--;
              break;
            }
          }
          free_peer(peer);
        }
      }
    }
  }
  for (size_t k = 0; k < number_peers; ++k) {
    struct chat_peer *peer_to_close = peers_to_close[k];
    for (size_t j = 0; j < server->peer_count; j++) {
      if (server->peers[j] == peer_to_close) {
        memmove(&server->peers[j], &server->peers[j + 1],
                (server->peer_count - j - 1) * sizeof(*server->peers));
        server->peer_count--;
        break;
      }
    }
    free_peer(peer_to_close);
  }
  return 0;
}

int chat_server_get_events(const struct chat_server *server) {
  if (server == NULL || server->socket == -1) {
    return 0;
  }
  int events = CHAT_EVENT_INPUT;
  for (size_t i = 0; i < server->peer_count; i++) {
    if (server->peers[i] != NULL && server->peers[i]->out_size > 0) {
      events |= CHAT_EVENT_OUTPUT;
      break;
    }
  }
  return events;
}

int chat_server_get_descriptor(const struct chat_server *server) {
#if NEED_SERVER_FEED
  /* IMPLEMENT THIS FUNCTION if want +5 points. */

  /*
   * Server has multiple sockets - own and from connected clients. Hence
   * you can't return a socket here. But if you are using epoll/kqueue,
   * then you can return their descriptor. These descriptors can be polled
   * just like sockets and will return an event when any of their owned
   * descriptors has any events.
   *
   * For example, assume you created an epoll descriptor and added to
   * there a listen-socket and a few client-sockets. Now if you will call
   * poll() on the epoll's descriptor, then on return from poll() you can
   * be sure epoll_wait() can return something useful for some of those
   * sockets.
   */
#endif
  (void)server;
  return -1;
}

int chat_server_feed(struct chat_server *server, const char *msg,
                     uint32_t msg_size) {
#if NEED_SERVER_FEED
  /* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
  (void)server;
  (void)msg;
  (void)msg_size;
  return CHAT_ERR_NOT_IMPLEMENTED;
}

int chat_server_get_socket(const struct chat_server *server) {
  return server->socket;
}
