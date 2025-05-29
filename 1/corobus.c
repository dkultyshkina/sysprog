#include "corobus.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "libcoro.h"
#include "rlist.h"

struct data_vector {
  unsigned *data;
  size_t size;
  size_t capacity;
};

#if 1 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void data_vector_append_many(struct data_vector *vector,
                                    const unsigned *data, size_t count) {
  if (vector->size + count > vector->capacity) {
    if (vector->capacity == 0)
      vector->capacity = 4;
    else
      vector->capacity *= 2;
    if (vector->capacity < vector->size + count)
      vector->capacity = vector->size + count;
    vector->data =
        realloc(vector->data, sizeof(vector->data[0]) * vector->capacity);
  }
  memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
  vector->size += count;
}

/** Append a single message to the vector. */
static void data_vector_append(struct data_vector *vector, unsigned data) {
  data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void data_vector_pop_first_many(struct data_vector *vector,
                                       unsigned *data, size_t count) {
  assert(count <= vector->size);
  memcpy(data, vector->data, sizeof(data[0]) * count);
  vector->size -= count;
  memmove(vector->data, &vector->data[count],
          vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned data_vector_pop_first(struct data_vector *vector) {
  unsigned data = 0;
  data_vector_pop_first_many(vector, &data, 1);
  return data;
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
  struct rlist base;
  struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
  struct rlist coros;
};

#if 1 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void wakeup_queue_suspend_this(struct wakeup_queue *queue,
                                      bool *closed) {
  struct wakeup_entry entry;
  entry.coro = coro_this();
  rlist_add_tail_entry(&queue->coros, &entry, base);
  if (*closed == true) {
    rlist_del_entry(&entry, base);
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return;
  }
  coro_suspend();
  if (!rlist_empty(&entry.base)) {
    rlist_del_entry(&entry, base);
  }
}

/** Wakeup the first coroutine in the queue. */
void wakeup_queue_wakeup_first(struct wakeup_queue *queue) {
  if (rlist_empty(&queue->coros)) {
    return;
  }
  struct wakeup_entry *entry =
      rlist_first_entry(&queue->coros, struct wakeup_entry, base);
  if (entry != NULL) {
    coro_wakeup(entry->coro);
  }
}

static void wakeup_queue_wakeup_all(struct wakeup_queue *queue) {
  while (!rlist_empty(&queue->coros)) {
    struct wakeup_entry *entry =
        rlist_first_entry(&queue->coros, struct wakeup_entry, base);
    if (entry != NULL) {
      rlist_del_entry(entry, base);
      coro_wakeup(entry->coro);
    }
  }
}

#endif

struct coro_bus_channel {
  /** Channel max capacity. */
  size_t size_limit;
  /** Coroutines waiting until the channel is not full. */
  struct wakeup_queue send_queue;
  /** Coroutines waiting until the channel is not empty. */
  struct wakeup_queue recv_queue;
  /** Message queue. */
  struct data_vector data;

  bool closed_flag;
};

struct coro_bus {
  struct coro_bus_channel **channels;
  size_t channel_count;
  size_t channel_capacity;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code coro_bus_errno(void) { return global_error; }

void coro_bus_errno_set(enum coro_bus_error_code err) { global_error = err; }

void free_channel(struct coro_bus_channel *channel) {
  if (channel != NULL) {
    channel->closed_flag = true;
    wakeup_queue_wakeup_all(&channel->send_queue);
    wakeup_queue_wakeup_all(&channel->recv_queue);
    coro_yield();
    free(channel->data.data);
    free(channel);
  }
}

struct coro_bus *coro_bus_new(void) {
  struct coro_bus *bus = calloc(1, sizeof(struct coro_bus));
  if (bus == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return NULL;
  }

  bus->channels = calloc(20, sizeof(struct coro_bus_channel *));
  if (bus->channels == NULL) {
    free(bus);
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return NULL;
  }
  bus->channel_count = 0;
  bus->channel_capacity = 20;
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return bus;
}

void coro_bus_delete(struct coro_bus *bus) {
  if (bus == NULL) {
    return;
  }
  for (size_t i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] != NULL) {
      free_channel(bus->channels[i]);
      bus->channels[i] = NULL;
    }
  }
  free(bus->channels);
  free(bus);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
}

int realloc_memory_channel(struct coro_bus *bus) {
  if (bus->channel_count >= (bus->channel_capacity)) {
    struct coro_bus_channel **new =
        realloc(bus->channels,
                2 * bus->channel_capacity * sizeof(struct coro_bus_channel *));
    if (new == NULL) {
      coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
      return -1;
    }
    for (size_t i = bus->channel_capacity; i < 2 * bus->channel_capacity; i++) {
      new[i] = NULL;
    }
    bus->channel_capacity = 2 * bus->channel_capacity;
    bus->channels = new;
  }
  return 0;
}

int take_description(struct coro_bus *bus, size_t size_limit) {
  int description = -1;
  for (size_t i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] == NULL) {
      description = i;
      break;
    }
  }

  if (description == -1) {
    if (realloc_memory_channel(bus) != 0) {
      return -1;
    }
    description = bus->channel_capacity;
  }

  for (size_t i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] == NULL) {
      description = i;
      break;
    }
  }

  struct coro_bus_channel *new_bus_channel =
      calloc(1, sizeof(struct coro_bus_channel));
  if (new_bus_channel == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }

  new_bus_channel->size_limit = size_limit;
  new_bus_channel->closed_flag = false;

  rlist_create(&new_bus_channel->send_queue.coros);
  rlist_create(&new_bus_channel->recv_queue.coros);
  new_bus_channel->data.data = NULL;
  new_bus_channel->data.size = 0;
  new_bus_channel->data.capacity = 0;

  bus->channels[description] = new_bus_channel;
  bus->channel_count++;
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return description;
}

int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit) {
  if (bus == NULL || size_limit <= 0) {
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }
  return take_description(bus, size_limit);
}

void coro_bus_channel_close(struct coro_bus *bus, int channel) {
  if (bus == NULL || channel < 0 || bus->channels[channel] == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return;
  }
  struct coro_bus_channel *new = bus->channels[channel];
  if (new->closed_flag == true) {
    return;
  }
  free_channel(new);
  bus->channels[channel] = NULL;
  bus->channel_count--;
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
}

int coro_bus_send(struct coro_bus *bus, int channel, unsigned data) {
  if (bus == NULL || bus->channel_count == 0 || bus->channel_capacity == 0 ||
      channel < 0 || bus->channels[channel] == NULL ||
      bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  struct coro_bus_channel *bus_channel = bus->channels[channel];

  while (bus_channel->data.size == bus_channel->size_limit) {
    if (bus_channel->closed_flag == true) {
      coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
      return -1;
    }
    wakeup_queue_suspend_this(&bus_channel->send_queue,
                              &bus_channel->closed_flag);
    if (bus_channel->closed_flag == true) {
      coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
      return -1;
    }
  }

  data_vector_append(&bus_channel->data, data);
  wakeup_queue_wakeup_first(&bus_channel->recv_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data) {
  if (bus == NULL || bus->channel_count == 0 || bus->channel_capacity == 0 ||
      channel < 0 || bus->channels[channel] == NULL ||
      bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  if (bus->channels[channel]->data.size == bus->channels[channel]->size_limit) {
    coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
    return -1;
  }

  data_vector_append(&bus->channels[channel]->data, data);
  wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data) {
  if (bus == NULL || channel < 0 || bus->channels[channel] == NULL ||
      data == NULL || bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  struct coro_bus_channel *bus_channel = bus->channels[channel];

  while (bus_channel->data.size == 0) {
    if (bus_channel->closed_flag == true) {
      coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
      return -1;
    }
    wakeup_queue_suspend_this(&bus_channel->recv_queue,
                              &bus_channel->closed_flag);
    if (bus_channel->closed_flag == true) {
      coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
      return -1;
    }
  }

  *data = data_vector_pop_first(&bus_channel->data);
  wakeup_queue_wakeup_first(&bus_channel->send_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data) {
  if (bus == NULL || channel < 0 || bus->channels[channel] == NULL ||
      data == NULL || bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  if (bus->channels[channel]->data.size == 0) {
    coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
    return -1;
  }

  *data = data_vector_pop_first(&bus->channels[channel]->data);
  wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

#if NEED_BROADCAST

int coro_bus_broadcast(struct coro_bus *bus, unsigned data) {
  if (bus == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  bool has_channels = false;
  for (size_t i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] != NULL && bus->channels[i]->closed_flag == false) {
      has_channels = true;
      break;
    }
  }

  if (has_channels == false) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  int result = 0;
  while (true) {
    result = coro_bus_try_broadcast(bus, data);
    if (result == 0) {
      break;
    }

    if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
      for (size_t i = 0; i < bus->channel_count; ++i) {
        if (bus->channels[i] != NULL &&
            bus->channels[i]->closed_flag == false &&
            bus->channels[i]->data.size >= bus->channels[i]->size_limit) {
          wakeup_queue_suspend_this(&bus->channels[i]->send_queue,
                                    &bus->channels[i]->closed_flag);
          break;
        }
      }
    }
  }

  for (size_t i = 0; i < bus->channel_count; ++i) {
    if (bus->channels[i] != NULL && bus->channels[i]->closed_flag == false &&
        bus->channels[i]->data.size >= bus->channels[i]->size_limit) {
      wakeup_queue_wakeup_first(&bus->channels[i]->send_queue);
    }
  }
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return result;
}

int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data) {
  if (bus == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  bool has_channels = false;

  for (size_t i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] != NULL && bus->channels[i]->closed_flag == false) {
      has_channels = true;
      if (bus->channels[i]->data.size >= bus->channels[i]->size_limit) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
      }
    }
  }

  if (has_channels == false) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  for (size_t i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] != NULL && bus->channels[i]->closed_flag == false) {
      data_vector_append(&bus->channels[i]->data, data);
      wakeup_queue_wakeup_first(&bus->channels[i]->recv_queue);
    }
  }
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

#endif

#if NEED_BATCH

int coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data,
                    unsigned count) {
  if (bus == NULL || channel < 0 || (size_t)channel >= bus->channel_capacity ||
      bus->channels[channel] == NULL || data == NULL || count == 0 ||
      bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  int sent = 0;
  while (true) {
    sent = coro_bus_try_send_v(bus, channel, data, count);
    if (sent != -1) {
      break;
    }
    wakeup_queue_suspend_this(&bus->channels[channel]->send_queue,
                              &bus->channels[channel]->closed_flag);
  }

  if (bus->channels[channel]->data.size < bus->channels[channel]->size_limit) {
    wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
  }
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return sent;
}

int coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data,
                        unsigned count) {
  if (bus == NULL || channel < 0 || bus->channels[channel] == NULL ||
      data == NULL || count == 0) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  if (bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  struct coro_bus_channel *bus_channel = bus->channels[channel];
  unsigned int available = bus_channel->size_limit - bus_channel->data.size;

  if (available == 0) {
    coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
    return -1;
  }

  unsigned int to_send = 0;
  if (count > available) {
    to_send = available;
  } else {
    to_send = count;
  }

  data_vector_append_many(&bus_channel->data, data, to_send);

  if (to_send > 0) {
    wakeup_queue_wakeup_first(&bus_channel->recv_queue);
  }

  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return to_send;
}

int coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data,
                    unsigned capacity) {
  if (bus == NULL || channel < 0 || (size_t)channel >= bus->channel_capacity ||
      bus->channels[channel] == NULL || data == NULL || capacity == 0 ||
      bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  int recv = 0;

  while (true) {
    recv = coro_bus_try_recv_v(bus, channel, data, capacity);
    if (recv != -1) {
      break;
    }
    wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue,
                              &bus->channels[channel]->closed_flag);
  }

  if (bus->channels[channel]->data.size > 0) {
    wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
  }

  return recv;
}

int coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data,
                        unsigned capacity) {
  if (bus == NULL || channel < 0 || (size_t)channel >= bus->channel_capacity ||
      bus->channels[channel] == NULL || data == NULL || capacity == 0 ||
      bus->channels[channel]->closed_flag == true) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }

  if (bus->channels[channel]->data.size == 0) {
    coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
    return -1;
  }

  size_t size = bus->channels[channel]->data.size;
  if (bus->channels[channel]->data.size > capacity) {
    size = capacity;
  }

  data_vector_pop_first_many(&bus->channels[channel]->data, data, size);
  wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);

  return size;
}
#endif