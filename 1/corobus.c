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
static void wakeup_queue_suspend_this(struct wakeup_queue *queue) {
  struct wakeup_entry entry;
  entry.coro = coro_this();
  rlist_add_tail_entry(&queue->coros, &entry, base);
  coro_suspend();
  rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
void wakeup_queue_wakeup_first(struct wakeup_queue *queue) {
  if (rlist_empty(&queue->coros)) return;
  struct wakeup_entry *entry =
      rlist_first_entry(&queue->coros, struct wakeup_entry, base);
  if (entry != NULL) {
    rlist_del_entry(entry, base);
    coro_wakeup(entry->coro);
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
};

struct coro_bus {
  struct coro_bus_channel **channels;
  int channel_count;
  int channel_capacity;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code coro_bus_errno(void) { return global_error; }

void coro_bus_errno_set(enum coro_bus_error_code err) { global_error = err; }

void free_channel(struct coro_bus_channel *channel) {
  if (channel != NULL) {
    while (rlist_empty(&channel->recv_queue.coros) == 0) {
      wakeup_queue_wakeup_first(&channel->recv_queue);
    }
    while (rlist_empty(&channel->send_queue.coros) == 0) {
      wakeup_queue_wakeup_first(&channel->send_queue);
    }
    free(channel->data.data);
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
  for (int i = 0; i < bus->channel_capacity; i++) {
    free_channel(bus->channels[i]);
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
      return -1;
    }
    bus->channel_capacity = bus->channel_capacity * 2;
    bus->channels = new;
  }
  return 0;
}

int take_description(struct coro_bus *bus, struct coro_bus_channel *channel,
                     size_t size_limit) {
  int description = 0;
  for (int i = 0; i < bus->channel_capacity; i++) {
    if (bus->channels[i] == NULL) {
      description = i;
      break;
    }
  }
  if (description == bus->channel_capacity) {
    free(channel->data.data);
    free(channel);
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }
  rlist_create(&channel->recv_queue.coros);
  rlist_create(&channel->send_queue.coros);
  bus->channels[description] = channel;
  bus->channel_count = bus->channel_count + 1;
  bus->channels[description]->size_limit = size_limit;
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return description;
}

int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit) {
  if (bus == NULL || size_limit == 0) {
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }
  if (realloc_memory_channel(bus) == -1) {
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }
  struct coro_bus_channel *channel = calloc(1, sizeof(struct coro_bus_channel));
  if (channel == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }
  channel->data.data = calloc(size_limit, sizeof(channel->data.data));
  if (channel->data.data == NULL) {
    free(channel);
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
  }
  return take_description(bus, channel, size_limit);
}

void coro_bus_channel_close(struct coro_bus *bus, int channel) {
  if (bus == NULL || channel < 0 || bus->channels[channel] == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return;
  }
  free_channel(bus->channels[channel]);
  bus->channels[channel] = NULL;
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
}

int coro_bus_send(struct coro_bus *bus, int channel, unsigned data) {
  if (bus == NULL || bus->channel_count == 0 || bus->channel_capacity == 0 ||
      channel < 0 || bus->channels[channel] == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }
  while (bus->channels[channel]->size_limit <=
         bus->channels[channel]->data.size) {
    wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
  }
  data_vector_append(&bus->channels[channel]->data, data);
  wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data) {
  if (bus == NULL || bus->channel_count == 0 || bus->channel_capacity == 0 ||
      channel < 0 || bus->channels[channel] == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }
  if (bus->channels[channel]->size_limit <= bus->channels[channel]->data.size) {
    coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
    return -1;
  }
  data_vector_append(&bus->channels[channel]->data, data);
  wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data) {
  if (bus == NULL || bus->channel_count == 0 || bus->channel_capacity == 0 ||
      channel < 0 || bus->channels[channel] == NULL) {
    coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    return -1;
  }
  while (bus->channels[channel]->data.size == 0) {
    wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
  }
  *data = data_vector_pop_first(&bus->channels[channel]->data);
  wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
  coro_bus_errno_set(CORO_BUS_ERR_NONE);
  return 0;
}

int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data) {
  if (bus == NULL || bus->channel_count == 0 || bus->channel_capacity == 0 ||
      channel < 0 || bus->channels[channel] == NULL) {
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
  /* IMPLEMENT THIS FUNCTION */
  (void)bus;
  (void)data;
  coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
  return -1;
}

int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data) {
  /* IMPLEMENT THIS FUNCTION */
  (void)bus;
  (void)data;
  coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
  return -1;
}

#endif

#if NEED_BATCH

int coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data,
                    unsigned count) {
  /* IMPLEMENT THIS FUNCTION */
  (void)bus;
  (void)channel;
  (void)data;
  (void)count;
  coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
  return -1;
}

int coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data,
                        unsigned count) {
  /* IMPLEMENT THIS FUNCTION */
  (void)bus;
  (void)channel;
  (void)data;
  (void)count;
  coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
  return -1;
}

int coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data,
                    unsigned capacity) {
  /* IMPLEMENT THIS FUNCTION */
  (void)bus;
  (void)channel;
  (void)data;
  (void)capacity;
  coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
  return -1;
}

int coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data,
                        unsigned capacity) {
  /* IMPLEMENT THIS FUNCTION */
  (void)bus;
  (void)channel;
  (void)data;
  (void)capacity;
  coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
  return -1;
}

#endif
