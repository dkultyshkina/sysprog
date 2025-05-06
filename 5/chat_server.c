#include "chat_server.h"
#include "chat.h"

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

void update_peer_events(struct peer_data *pd, uint32_t new_events) {
  if (pd->current_events == new_events) {
    return;
  }
  struct epoll_event ev;
  ev.events = new_events;
  ev.data.ptr = pd;

  epoll_ctl(pd->server->epoll_fd, EPOLL_CTL_DEL, pd->fd, NULL);
  if (epoll_ctl(pd->server->epoll_fd, EPOLL_CTL_ADD, pd->fd, &ev) == 0) {
    pd->current_events = new_events;
  }
}

struct chat_server *chat_server_new(void) {
  struct chat_server *server = calloc(1, sizeof(*server));
  if (server == NULL) {
    return NULL;
  }
  server->socket = -1;
  server->epoll_fd = -1;
  server->msg_capacity = 8;
  server->messages = malloc(server->msg_capacity * sizeof(*server->messages));
  return server;
}

static struct chat_peer *create_peer(int socket) {
  struct chat_peer *peer = calloc(1, sizeof(*peer));
  if (peer == NULL) {
    return NULL;
  }
  peer->socket = socket;
  peer->partial_capacity = 1024;
  peer->partial_in = malloc(peer->partial_capacity);
  peer->out_capacity = 1024;
  peer->out_buffer = malloc(peer->out_capacity);
  peer->p_data = NULL;
  return peer;
}

static void free_peer(struct chat_peer *peer) {
  if (peer == NULL) {
    return;
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
  if (server->epoll_fd >= 0) {
    close(server->epoll_fd);
  }
  if (server->socket >= 0) {
    close(server->socket);
  }
  for (size_t i = 0; i < server->peer_count; i++) {
    free_peer(server->peers[i]);
  }

  for (size_t i = 0; i < server->msg_count; i++) {
    free(server->messages[i].data);
  }
  free(server->messages);
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

  struct peer_data *pd = malloc(sizeof(*pd));
  if (pd == NULL) {
    close(server->epoll_fd);
    close(server->socket);
    return CHAT_ERR_SYS;
  }

  pd->fd = server->socket;
  pd->peer = NULL;
  pd->server = server;
  pd->current_events = EPOLLIN;

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.ptr = pd;

  if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->socket, &ev) == -1) {
    free(pd);
    close(server->epoll_fd);
    close(server->socket);
    return CHAT_ERR_SYS;
  }

  return 0;
}

void get_in_data(struct chat_server *server, struct chat_peer *peer,
                 const char *data, size_t size) {
  size_t new_size = peer->partial_size + size;
  if (new_size + 1 > peer->partial_capacity) {
    peer->partial_capacity = new_size * 2 + 1;
    char *new_buf = realloc(peer->partial_in, peer->partial_capacity);
    if (!new_buf)
      return;
    peer->partial_in = new_buf;
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
      break;
    }
    memcpy(msg, start, msg_len);
    msg[msg_len] = '\0';

    trim_server_message(msg);
    if (is_empty_server_message(msg) == false) {
      if (server->msg_count == server->msg_capacity) {
        server->msg_capacity *= 2;
        struct chat_message *new_msgs = realloc(
            server->messages, server->msg_capacity * sizeof(*server->messages));
        if (new_msgs == NULL) {
          free(msg);
          break;
        }
        server->messages = new_msgs;
      }

      server->messages[server->msg_count].data = msg;
      server->messages[server->msg_count].size = msg_len;
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
    peer->partial_in[0] = '\0';
  }
  peer->partial_size = remaining;
}

struct chat_message *chat_server_pop_next(struct chat_server *server) {
  if (server == NULL || server->msg_count == 0) {
    return NULL;
  }
  struct chat_message *msg = malloc(sizeof(*msg));
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
  if (server == NULL || server->socket < 0 || server->epoll_fd < 0)
    return CHAT_ERR_NOT_STARTED;

  struct epoll_event events[30];
  int number = epoll_wait(server->epoll_fd, events, 30, timeout * 1000);

  if (number == 0) {
    return CHAT_ERR_TIMEOUT;
  }

  if (number == -1) {
    return CHAT_ERR_SYS;
  }

  for (int i = 0; i < number; i++) {
    struct peer_data *pd = events[i].data.ptr;

    if (pd->peer == NULL) {
      while (1) {
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
        if (flags == -1) {
          close(client_sock);
        } else {
          fcntl(client_sock, F_SETFL, O_NONBLOCK);
        }

        struct chat_peer *peer = create_peer(client_sock);
        if (peer == NULL) {
          close(client_sock);
          continue;
        }

        peer->p_data = malloc(sizeof(*peer->p_data));
        if (peer->p_data == NULL) {
          free_peer(peer);
          continue;
        }

        peer->p_data->fd = client_sock;
        peer->p_data->peer = peer;
        peer->p_data->server = server;
        peer->p_data->current_events = EPOLLIN | EPOLLET;

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = peer->p_data;

        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_sock, &ev) ==
            -1) {
          free_peer(peer);
          continue;
        }

        server->peers[server->peer_count++] = peer;
      }
    } else {
      struct chat_peer *peer = pd->peer;
      bool close_peer = false;

      if (events[i].events & EPOLLIN) {
        char buf[1024];
        ssize_t received;

        while ((received = recv(peer->socket, buf, sizeof(buf), 0)) > 0) {
          get_in_data(server, peer, buf, received);
        }

        if (received == 0 || (received == -1 && errno != EAGAIN)) {
          close_peer = true;
        }
      }

      if (close_peer == false && (events[i].events & EPOLLOUT)) {
        if (peer->out_size > 0) {
          ssize_t sent =
              send(peer->socket, peer->out_buffer, peer->out_size, 0);
          if (sent > 0) {
            memmove(peer->out_buffer, peer->out_buffer + sent,
                    peer->out_size - sent);
            peer->out_size -= sent;

            if (peer->out_size == 0) {
              update_peer_events(pd, EPOLLIN | EPOLLET);
            }
          } else if (sent == -1 && errno != EAGAIN) {
            close_peer = true;
          }
        }
      }

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        close_peer = true;
      }

      if (close_peer == true) {
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
  return 0;
}

int chat_server_get_events(const struct chat_server *server) {
  if (server == NULL || server->socket == -1)
    return 0;

  int events = CHAT_EVENT_INPUT;
  for (size_t i = 0; i < server->peer_count; i++) {
    if (server->peers[i]->out_size > 0) {
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