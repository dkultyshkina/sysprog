#include "chat_client.h"
#include "chat.h"

#include <stdio.h>

void trim_client_message(char *str) {
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

bool is_empty_client_message(const char *str) {
  while (*str) {
    if (!isspace(*str)) {
      return false;
    }
    str++;
  }
  return true;
}

static bool update_client_events(struct client_data *c_data,
                                 uint32_t new_events) {
  new_events |= EPOLLET;
  if (c_data->current_events == new_events) {
    return true;
  }
  struct epoll_event ev;
  ev.events = new_events;
  ev.data.ptr = c_data;
  if (epoll_ctl(c_data->client->epoll_fd, EPOLL_CTL_DEL, c_data->fd, &ev) ==
          -1 &&
      errno != ENOENT) {
    c_data->client->is_closed = true;
    return false;
  }
  if (epoll_ctl(c_data->client->epoll_fd, EPOLL_CTL_ADD, c_data->fd, &ev) ==
      0) {
    c_data->current_events = new_events;
    return true;
  } else {
    c_data->client->is_closed = true;
    return false;
  }
  c_data->client->is_closed = true;
  return false;
}

struct chat_client *chat_client_new(const char *name) {
  (void)name;
  struct chat_client *client = calloc(1, sizeof(*client));
  if (client == NULL) {
    return NULL;
  }
  client->socket = -1;
  client->epoll_fd = -1;
  client->c_data = NULL;
  client->is_closed = false;
  client->msg_capacity = 8;
  client->msg_size = 0;
  client->in_msg = malloc(client->msg_capacity * sizeof(struct chat_message));
  if (client->in_msg == NULL) {
    free(client);
    return NULL;
  }
  client->out_capacity = 1024;
  client->out_size = 0;
  client->out_buffer = malloc(client->out_capacity);
  if (client->out_buffer == NULL) {
    free(client->in_msg);
    free(client);
    return NULL;
  }
  client->partial_capacity = 1024;
  client->partial_size = 0;
  client->partial_buffer = malloc(client->partial_capacity);
  if (client->partial_buffer == NULL) {
    free(client->out_buffer);
    free(client->in_msg);
    free(client);
    return NULL;
  }
  return client;
}

void chat_client_delete(struct chat_client *client) {
  if (client == NULL) {
    return;
  }
  if (client->epoll_fd >= 0 && client->socket >= 0) {
    epoll_ctl(client->epoll_fd, EPOLL_CTL_DEL, client->socket, NULL);
  }
  if (client->epoll_fd >= 0) {
    close(client->epoll_fd);
  }
  if (client->socket >= 0) {
    close(client->socket);
  }
  free(client->c_data);
  for (size_t i = 0; i < client->msg_size; i++) {
    free(client->in_msg[i].data);
  }
  free(client->in_msg);
  free(client->out_buffer);
  free(client->partial_buffer);
  free(client);
}

int chat_client_connect(struct chat_client *client, const char *addr) {
  if (client == NULL || client->socket != -1) {
    return CHAT_ERR_ALREADY_STARTED;
  }
  char *host_copy = strdup(addr);
  if (host_copy == NULL) {
    return CHAT_ERR_SYS;
  }
  char *port_str = strchr(host_copy, ':');
  if (port_str == NULL) {
    free(host_copy);
    return CHAT_ERR_NO_ADDR;
  }
  *port_str++ = 0;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *result;
  if (getaddrinfo(host_copy, port_str, &hints, &result) != 0) {
    free(host_copy);
    return CHAT_ERR_NO_ADDR;
  }
  free(host_copy);
  int sock = -1;
  for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) {
      continue;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
      close(sock);
      sock = -1;
      continue;
    }
    if (connect(sock, rp->ai_addr, rp->ai_addrlen) == -1) {
      if (errno != EINPROGRESS) {
        close(sock);
        sock = -1;
        continue;
      }
    }
    break;
  }
  freeaddrinfo(result);
  if (sock == -1) {
    return CHAT_ERR_SYS;
  }
  client->socket = sock;
  client->epoll_fd = epoll_create1(0);
  if (client->epoll_fd == -1) {
    close(client->socket);
    client->socket = -1;
    return CHAT_ERR_SYS;
  }
  client->c_data = malloc(sizeof(struct client_data));
  if (client->c_data == NULL) {
    close(client->epoll_fd);
    close(client->socket);
    client->epoll_fd = -1;
    client->socket = -1;
    return CHAT_ERR_SYS;
  }
  client->c_data->fd = client->socket;
  client->c_data->client = client;
  client->c_data->current_events = 0;
  if (update_client_events(client->c_data, EPOLLIN | EPOLLOUT | EPOLLET |
                                               EPOLLRDHUP) == false) {
    free(client->c_data);
    close(client->epoll_fd);
    close(client->socket);
    client->c_data = NULL;
    client->socket = -1;
    client->epoll_fd = -1;
    return CHAT_ERR_SYS;
  }
  return 0;
}

struct chat_message *chat_client_pop_next(struct chat_client *client) {
  if (client == NULL || client->msg_size == 0) {
    return NULL;
  }
  struct chat_message msg = client->in_msg[0];
  memmove(client->in_msg, client->in_msg + 1,
          (client->msg_size - 1) * sizeof(*client->in_msg));

  client->msg_size--;
  struct chat_message *result = malloc(sizeof(struct chat_message));
  if (result == NULL) {
    return NULL;
  }
  result->data = strdup(msg.data);
  result->size = msg.size;
  free(msg.data);
  return result;
}

void get_client_in_data(struct chat_client *client, const char *data,
                        size_t size) {
  if (client == NULL || data == NULL || size == 0) {
    return;
  }
  if (client->partial_size + size + 1 > client->partial_capacity) {
    size_t new_capacity = (client->partial_size + size + 1) * 2;
    char *new = realloc(client->partial_buffer, new_capacity);
    if (new == NULL) {
      client->is_closed = true;
      return;
    }
    client->partial_buffer = new;
    client->partial_capacity = new_capacity;
  }
  memcpy(client->partial_buffer + client->partial_size, data, size);
  client->partial_size = client->partial_size + size;
  client->partial_buffer[client->partial_size] = '\0';
  char *start = client->partial_buffer;
  char *end;
  while (
      (end = memchr(start, '\n',
                    client->partial_size - (start - client->partial_buffer)))) {
    size_t msg_len = end - start;
    char *msg = malloc(msg_len + 1);
    if (msg == NULL) {
      client->is_closed = true;
      break;
    }
    memcpy(msg, start, msg_len);
    msg[msg_len] = '\0';
    trim_client_message(msg);
    if (!is_empty_client_message(msg)) {
      if (client->msg_size == client->msg_capacity) {
        client->msg_capacity *= 2;
        struct chat_message *new_in_msg = realloc(
            client->in_msg, client->msg_capacity * sizeof(struct chat_message));
        if (new_in_msg == NULL) {
          client->is_closed = true;
          free(msg);
          break;
        }
        client->in_msg = new_in_msg;
      }
      client->in_msg[client->msg_size].data = msg;
      client->in_msg[client->msg_size].size = strlen(msg);
      client->msg_size++;
    } else {
      free(msg);
    }
    start = end + 1;
  }
  size_t remaining = client->partial_size - (start - client->partial_buffer);
  if (remaining > 0 && start != client->partial_buffer) {
    memmove(client->partial_buffer, start, remaining);
  } else if (remaining == 0) {
    client->partial_buffer[0] = '\0';
  }
  client->partial_size = remaining;
  client->partial_buffer[client->partial_size] = '\0';
}

int chat_client_update(struct chat_client *client, double timeout) {
  if (client == NULL || client->socket == -1 || client->epoll_fd == -1 ||
      client->c_data == NULL) {
    return CHAT_ERR_NOT_STARTED;
  }
  if (client->is_closed == true) {
    return CHAT_ERR_SYS;
  }
  int epoll_timeout;
  if (timeout < 0) {
    epoll_timeout = -1;
  } else if (timeout == 0) {
    epoll_timeout = 10;
  } else {
    epoll_timeout = (int)(timeout * 1000);
  }
  struct epoll_event events[1];
  int number = epoll_wait(client->epoll_fd, events, 1, epoll_timeout);
  if (number == 0) {
    return CHAT_ERR_TIMEOUT;
  }
  if (number == -1) {
    if (errno == EINTR)
      return CHAT_ERR_TIMEOUT;
    client->is_closed = true;
    return CHAT_ERR_SYS;
  }
  struct client_data *c_data = (struct client_data *)events[0].data.ptr;
  if (events[0].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    client->is_closed = true;
    return CHAT_ERR_SYS;
  }
  if (events[0].events & EPOLLOUT) {
    if (client->out_size > 0) {
      ssize_t sent = send(client->socket, client->out_buffer, client->out_size,
                          MSG_NOSIGNAL);
      if (sent > 0) {
        memmove(client->out_buffer, client->out_buffer + sent,
                client->out_size - sent);
        client->out_size -= sent;

        if (client->out_size == 0) {
          update_client_events(c_data, EPOLLIN | EPOLLET | EPOLLRDHUP);
        }
      } else if (sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        client->is_closed = true;
        return CHAT_ERR_SYS;
      }
    }
  }
  if (events[0].events & EPOLLIN) {
    char buffer[1024];
    ssize_t received;
    while ((received = recv(client->socket, buffer, sizeof(buffer), 0)) > 0) {
      get_client_in_data(client, buffer, received);
      if (client->is_closed == true)
        break;
    }
    if ((received == 0) ||
        (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      client->is_closed = true;
      return CHAT_ERR_SYS;
    }
  }
  return 0;
}

int chat_client_get_descriptor(const struct chat_client *client) {
  return client->socket;
}

int chat_client_get_events(const struct chat_client *client) {
  if (client == NULL || client->socket == -1) {
    return 0;
  }
  int events = CHAT_EVENT_INPUT;
  if (client->out_size > 0) {
    events |= CHAT_EVENT_OUTPUT;
  }
  return events;
}

int chat_client_feed(struct chat_client *client, const char *msg,
                     uint32_t msg_size) {
  if (client == NULL || msg == NULL || client->socket == -1 ||
      client->c_data == NULL) {
    return CHAT_ERR_NOT_STARTED;
  }
  if (client->is_closed) {
    return CHAT_ERR_SYS;
  }
  if (msg_size == 0) {
    return 0;
  }
  if (client->out_size + msg_size + 1 < client->out_size) {
    client->is_closed = true;
    return CHAT_ERR_SYS;
  }
  if (client->out_size + msg_size + 1 > client->out_capacity) {
    size_t new_capacity = (client->out_size + msg_size + 1) * 2;
    char *new = realloc(client->out_buffer, new_capacity);
    if (new == NULL) {
      client->is_closed = true;
      return CHAT_ERR_SYS;
    }
    client->out_buffer = new;
    client->out_capacity = new_capacity;
  }
  memcpy(client->out_buffer + client->out_size, msg, msg_size);
  client->out_size += msg_size;
  if (update_client_events(client->c_data, EPOLLIN | EPOLLOUT | EPOLLET |
                                               EPOLLRDHUP) == false) {
    return CHAT_ERR_SYS;
  }
  return 0;
}
