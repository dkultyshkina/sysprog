#include "thread_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

enum status { NEW, IN_QUEUED, RUNNING, FINISHED };

struct thread_task {
  thread_task_f function;
  void *arg;
  void *result;

  pthread_mutex_t mutex;
  pthread_cond_t finished_condition;

  struct thread_pool *pool;
  struct thread_task *next;

  enum status status_task;
  bool detach;
};

struct thread_pool {
  pthread_t *threads;

  int max_thread_count;
  int current_thread_count;
  int create_thread_count;
  int active_thread_count;

  struct thread_task *queue_head;
  struct thread_task *queue_tail;
  int task_count;

  pthread_mutex_t mutex;
  pthread_cond_t available_task_condition;
  pthread_cond_t no_task_condition;

  bool stop;
};

void *start_thread(void *arg) {
  struct thread_pool *pool = (struct thread_pool *)arg;
  while (true) {
    pthread_mutex_lock(&pool->mutex);

    while ((pool->stop == false) && (pool->queue_head == NULL)) {
      pthread_cond_wait(&pool->available_task_condition, &pool->mutex);
    }

    if (pool->stop == true && (pool->queue_head == NULL)) {
      pool->current_thread_count--;
      pthread_mutex_unlock(&pool->mutex);
      pthread_exit(NULL);
    }

    struct thread_task *current = pool->queue_head;
    if (current == NULL) {
      pthread_mutex_unlock(&pool->mutex);
      continue;
    }

    pool->queue_head = current->next;
    if (pool->queue_head == NULL) {
      pool->queue_tail = NULL;
    }
    pool->task_count--;
    pool->active_thread_count++;
    pthread_mutex_unlock(&pool->mutex);
    pthread_mutex_lock(&current->mutex);
    current->status_task = RUNNING;
    pthread_mutex_unlock(&current->mutex);
    void *result = NULL;
    if (pool->stop == false) {
      result = current->function(current->arg);
    }
    pthread_mutex_lock(&current->mutex);
    current->result = result;
    bool detached = current->detach;
    current->status_task = FINISHED;
    pthread_cond_broadcast(&current->finished_condition);
    pthread_mutex_unlock(&current->mutex);
    if (detached == true) {
      pthread_mutex_destroy(&current->mutex);
      pthread_cond_destroy(&current->finished_condition);
      free(current);
    }
    pthread_mutex_lock(&pool->mutex);
    pool->active_thread_count--;
    if (pool->task_count == 0 && pool->active_thread_count == 0) {
      pthread_cond_signal(&pool->no_task_condition);
    }

    pthread_mutex_unlock(&pool->mutex);
  }
  return NULL;
};

int thread_pool_new(int max_thread_count, struct thread_pool **pool) {
  if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }
  struct thread_pool *new_pool = calloc(1, sizeof(struct thread_pool));
  if (new_pool == NULL) {
    return -1;
  }
  new_pool->threads = calloc(max_thread_count, sizeof(pthread_t));
  if (new_pool->threads == NULL) {
    free(new_pool);
    return -1;
  }
  new_pool->max_thread_count = max_thread_count;
  new_pool->current_thread_count = 0;
  new_pool->active_thread_count = 0;
  new_pool->create_thread_count = 0;
  new_pool->queue_head = NULL;
  new_pool->queue_tail = NULL;
  new_pool->task_count = 0;
  new_pool->stop = false;

  pthread_mutex_init(&new_pool->mutex, NULL);
  pthread_cond_init(&new_pool->available_task_condition, NULL);
  pthread_cond_init(&new_pool->no_task_condition, NULL);
  *pool = new_pool;
  return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool) {
  if (pool == NULL) {
    return 0;
  }
  pthread_mutex_lock((pthread_mutex_t *)&pool->mutex);
  int count = pool->current_thread_count;
  pthread_mutex_unlock((pthread_mutex_t *)&pool->mutex);
  return count;
}

int thread_pool_delete(struct thread_pool *pool) {
  if (pool == NULL) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }

  pthread_mutex_lock(&pool->mutex);
  if (pool->task_count > 0 || pool->active_thread_count > 0) {
    pthread_mutex_unlock(&pool->mutex);
    return TPOOL_ERR_HAS_TASKS;
  }

  pool->stop = true;
  pthread_cond_broadcast(&pool->available_task_condition);
  pthread_mutex_unlock(&pool->mutex);

  for (int i = 0; i < pool->create_thread_count; i++) {
    if (pool->threads[i]) {
      pthread_join(pool->threads[i], NULL);
    }
  }

  struct thread_task *task = pool->queue_head;
  while (task != NULL) {
    struct thread_task *next = task->next;
    pthread_mutex_destroy(&task->mutex);
    pthread_cond_destroy(&task->finished_condition);
    free(task);
    task = next;
  }

  pool->queue_tail = NULL;
  pool->task_count = 0;

  pthread_mutex_destroy(&pool->mutex);
  pthread_cond_destroy(&pool->available_task_condition);
  pthread_cond_destroy(&pool->no_task_condition);
  free(pool->threads);
  free(pool);

  return 0;
}

int thread_pool_push_task(struct thread_pool *pool, struct thread_task *task) {
  if (pool == NULL || task == NULL || pool->stop == true) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }
  if (pool->task_count >= TPOOL_MAX_TASKS) {
    return TPOOL_ERR_TOO_MANY_TASKS;
  }
  if (task->status_task != NEW && task->status_task != FINISHED) {
    return TPOOL_ERR_TASK_IN_POOL;
  }
  pthread_mutex_lock(&pool->mutex);
  task->status_task = IN_QUEUED;
  task->pool = pool;
  task->next = NULL;
  if (pool->queue_head == NULL) {
    pool->queue_head = task;
    pool->queue_tail = task;
  } else {
    pool->queue_tail->next = task;
    pool->queue_tail = task;
  }
  pool->task_count++;
  if ((pool->current_thread_count < pool->max_thread_count) &&
      (pool->active_thread_count == pool->current_thread_count)) {
    if (pool->create_thread_count < pool->max_thread_count) {
      if (pthread_create(&pool->threads[pool->create_thread_count], NULL,
                         start_thread, pool) == 0) {
        pool->current_thread_count++;
        pool->create_thread_count++;
      } else {
        pool->task_count--;
        thread_task_delete(task);
        pthread_mutex_unlock(&pool->mutex);
        return 0;
      }
    }
  }
  pthread_cond_signal(&pool->available_task_condition);
  pthread_mutex_unlock(&pool->mutex);
  return 0;
}

int thread_task_new(struct thread_task **task, thread_task_f function,
                    void *arg) {
  if (function == NULL || task == NULL) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }
  struct thread_task *new_task = calloc(1, sizeof(struct thread_task));
  if (new_task == NULL) {
    return -1;
  }
  new_task->function = function;
  new_task->arg = arg;
  new_task->status_task = NEW;
  new_task->result = NULL;
  new_task->pool = NULL;
  new_task->next = NULL;
  pthread_mutex_init(&new_task->mutex, NULL);
  pthread_cond_init(&new_task->finished_condition, NULL);
  *task = new_task;
  return 0;
}

bool thread_task_is_finished(const struct thread_task *task) {
  if (task == NULL) {
    return false;
  }
  return task->status_task == FINISHED;
}

bool thread_task_is_running(const struct thread_task *task) {
  if (task == NULL) {
    return false;
  }
  return task->status_task == RUNNING;
}

int thread_task_join(struct thread_task *task, void **result) {
  if (task == NULL || result == NULL) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }
  pthread_mutex_lock(&task->mutex);
  if (task->status_task == NEW || task->pool == NULL) {
    pthread_mutex_unlock(&task->mutex);
    return TPOOL_ERR_TASK_NOT_PUSHED;
  }

  if (task->status_task == FINISHED) {
    *result = task->result;
    pthread_mutex_unlock(&task->mutex);
    return 0;
  }

  while (task->status_task != FINISHED) {
    pthread_cond_wait(&task->finished_condition, &task->mutex);
  }
  pthread_mutex_unlock(&task->mutex);

  if (result != NULL) {
    *result = task->result;
  }
  return 0;
}

#if NEED_TIMED_JOIN

int thread_task_timed_join(struct thread_task *task, double timeout,
                           void **result) {
  if (task == NULL || result == NULL) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }

  if (task->status_task == NEW || task->pool == NULL) {
    return TPOOL_ERR_TASK_NOT_PUSHED;
  }

  if (task->status_task == FINISHED) {
    *result = task->result;
    return 0;
  }

  if (timeout <= 0) {
    return TPOOL_ERR_TIMEOUT;
  }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += (time_t)timeout;
  ts.tv_nsec += (long)((timeout - (time_t)timeout) * 1e9);

  pthread_mutex_lock(&task->mutex);

  int wait = 0;
  while (task->status_task != FINISHED && wait != ETIMEDOUT) {
    wait = pthread_cond_timedwait(&task->finished_condition, &task->mutex, &ts);
  }

  if (wait == ETIMEDOUT) {
    pthread_mutex_unlock(&task->mutex);
    return TPOOL_ERR_TIMEOUT;
  }
  *result = task->result;
  pthread_mutex_unlock(&task->mutex);
  return 0;
}

#endif

int thread_task_delete(struct thread_task *task) {
  if (task == NULL) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }
  pthread_mutex_lock(&task->mutex);
  enum status status = task->status_task;
  bool is_detached = task->detach;
  if (status == IN_QUEUED || status == RUNNING) {
    pthread_mutex_unlock(&task->mutex);
    return TPOOL_ERR_TASK_IN_POOL;
  }
  if (is_detached == true && status == FINISHED) {
    return TPOOL_ERR_INVALID_ARGUMENT;
  }
  pthread_mutex_destroy(&task->mutex);
  pthread_cond_destroy(&task->finished_condition);
  free(task);
  return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif