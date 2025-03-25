#include "userfs.h"
#include <stdbool.h>
#include <stddef.h>

enum {
  BLOCK_SIZE = 512,
  MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
  /** Block memory. */
  char *memory;
  /** How many bytes are occupied. */
  int occupied;
  /** Next block in the file. */
  struct block *next;
  /** Previous block in the file. */
  struct block *prev;
  int offset_read;
  int offset_write;
};

struct file {
  /** Double-linked list of file blocks. */
  struct block *block_list;
  /**
   * Last block in the list above for fast access to the end
   * of file.
   */
  struct block *last_block;
  /** How many file descriptors are opened on the file. */
  int refs;
  /** File name. */
  char *name;
  /** Files are stored in a double-linked list. */
  struct file *next;
  struct file *prev;
  int number_block;
  bool metka;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
  struct file *file;
  enum open_flags flags;
  int current_offset;
  struct block *current_block;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

int check_file(int fd) {
  if (fd < 0 || fd >= file_descriptor_capacity) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }
  if (file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }
  return 0;
}

struct file *find_file(const char *filename) {
  struct file *current_file = file_list;
  while (current_file != NULL) {
    if (strcmp(current_file->name, filename) == 0 && !current_file->metka) {
      break;
    }
    current_file = current_file->next;
  }
  return current_file;
}

void free_block(struct file *current_file) {
  struct block *current = current_file->block_list;
  while (current != NULL) {
    struct block *next = current->next;
    free(current->memory);
    free(current);
    current = next;
  }
  if (current_file->prev != NULL) {
    current_file->prev->next = current_file->next;
  } else {
    file_list = current_file->next;
  }

  if (current_file->next != NULL) {
    current_file->next->prev = current_file->prev;
  }
  free(current_file->name);
  free(current_file);
}

enum ufs_error_code ufs_errno() { return ufs_error_code; }

int ufs_open(const char *filename, int flags) {
  ufs_error_code = UFS_ERR_NO_ERR;
  if (file_descriptors == NULL) {
    file_descriptors = calloc(10, sizeof(struct filedesc *));
    if (file_descriptors == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    file_descriptor_count = 0;
    file_descriptor_capacity = 10;
  }
  struct file *current_file = find_file(filename);
  if (current_file == NULL && !(flags & UFS_CREATE)) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (current_file == NULL) {
    current_file = calloc(1, sizeof(struct file));
    if (current_file == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    current_file->name = strdup(filename);
    if (current_file->name == NULL) {
      free(current_file);
      return -1;
    }
    struct block *b = calloc(1, sizeof(struct block));
    if (b == NULL) {
      free(current_file->name);
      free(current_file);
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    b->memory = calloc(BLOCK_SIZE, 1);
    if (b->memory == NULL) {
      free(b);
      free(current_file->name);
      free(current_file);
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    if (current_file->block_list == NULL) {
      current_file->block_list = b;
      current_file->last_block = b;
    } else {
      if (current_file->last_block->next != NULL) {
        current_file->last_block->next = b;
      }
      b->prev = current_file->last_block;
      current_file->last_block = b;
    }
    if (file_list != NULL) {
      current_file->next = file_list;
      file_list->prev = current_file;
    }
    file_list = current_file;
  }

  int fd = -1;
  for (int i = 0; i < file_descriptor_capacity; ++i) {
    if (file_descriptors[i] == NULL) {
      fd = i;
      break;
    }
  }
  struct filedesc *desc = calloc(1, sizeof(struct filedesc));
  if (desc == NULL) {
    ufs_error_code = UFS_ERR_NO_MEM;
    free(current_file->name);
    free(current_file);
    return -1;
  }
  desc->file = current_file;
  desc->flags = flags;
  struct block *current = current_file->block_list;
  for (int i = -1; i < current_file->number_block; i++) {
    current->offset_read = 0;
    current->offset_write = 0;
    current = current->next;
  }
  file_descriptors[fd] = desc;
  current_file->refs++;
  file_descriptor_count++;
  return fd;
}

ssize_t ufs_write(int fd, const char *buf, size_t size) {

  if (check_file(fd) == -1) {
    return -1;
  }

  struct filedesc *current_desc = file_descriptors[fd];
  struct file *current_file = current_desc->file;
  struct block *current_block = current_file->block_list;

  for (int i = 0; i < current_file->number_block; i++) {
    current_block = current_block->next;
  }

  if ((current_block->occupied + current_file->number_block * BLOCK_SIZE +
       size) > MAX_FILE_SIZE) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  size_t buffer = 0;
  while (buffer < size) {
    struct block *current = current_file->last_block;
    if (current->occupied == BLOCK_SIZE) {
      if (current->next == NULL) {
        current->next = calloc(1, sizeof(struct block));
        if (current->next == NULL) {
          ufs_error_code = UFS_ERR_NO_MEM;
          return -1;
        }
        current->next->memory = calloc(1, BLOCK_SIZE);
        if (current->next->memory == NULL) {
          free(current->next);
          ufs_error_code = UFS_ERR_NO_MEM;
          return -1;
        }
        if (current_file->block_list == NULL) {
          current_file->block_list = current->next;
          current_file->last_block = current->next;
        } else {
          if (current_file->last_block->next != NULL) {
            current_file->last_block->next = current->next;
          }
          current->next->prev = current_file->last_block;
          current_file->last_block = current->next;
        }
        if (file_list != NULL) {
          current_file->next = file_list;
          file_list->prev = current_file;
        }
        file_list = current_file;
        current_block = current_file->last_block;
        current_block->occupied = 0;
        current_block->offset_read = 0;
      }
      current_file->number_block++;
    }

    size_t available = BLOCK_SIZE - current_block->offset_read;
    size_t to_write = (available < size - buffer) ? available : size - buffer;

    memcpy(current_block->memory + current_block->offset_read, buf + buffer,
           to_write);

    current_block->offset_write += to_write;
    buffer += to_write;

    char numbers[100];
    memcpy(numbers, current_block->memory, 10);
    if ((current_block->offset_write + current_block->offset_read) >
        current_block->occupied) {
      current_block->occupied =
          current_block->offset_write + current_block->offset_read;
    }
  }
  return buffer;
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
  ufs_error_code = UFS_ERR_NO_ERR;

  if (check_file(fd) == -1) {
    return -1;
  }

  struct filedesc *desc = file_descriptors[fd];
  struct file *f = desc->file;

  if (f->block_list == NULL) {
    return 0;
  }

  size_t total_read = 0;
  struct block *current_block = f->block_list;
  int current_offset = current_block->offset_read;
  while (total_read < size && current_block != NULL) {
    size_t available = current_block->occupied - current_offset;
    if (available <= 0) {
      current_block = current_block->next;
      current_offset = 0;
      continue;
    }

    size_t to_read =
        (available < size - total_read) ? available : size - total_read;
    memcpy(buf + total_read, current_block->memory + current_offset, to_read);

    total_read += to_read;
    current_offset += to_read;
    desc->current_block = current_block;
    desc->current_offset = current_offset;
    if (current_offset == BLOCK_SIZE) {
      current_block = current_block->next;
      current_offset = 0;
      if (current_block) {
        desc->current_block = current_block;
        desc->current_offset = 0;
      }
    }
    desc->current_block->offset_read = current_offset;
  }
  return total_read;
}

int ufs_close(int fd) {
  ufs_error_code = UFS_ERR_NO_ERR;
  if (check_file(fd) == -1) {
    return -1;
  }
  free(file_descriptors[fd]);
  file_descriptors[fd] = NULL;
  file_descriptor_count--;
  return 0;
}

int ufs_delete(const char *filename) {
  ufs_error_code = UFS_ERR_NO_ERR;
  struct file *current_file = find_file(filename);
  if (current_file == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }
  if (current_file->refs != 0) {
    current_file->metka = true;
    return 0;
  }
  free_block(current_file);
  return 0;
}

#if NEED_RESIZE

int ufs_resize(int fd, size_t new_size) {
  /* IMPLEMENT THIS FUNCTION */
  (void)fd;
  (void)new_size;
  ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
  return -1;
}

#endif

void ufs_destroy(void) {
  for (int i = 0; i < file_descriptor_count; i++) {
    if (file_descriptors[i] != NULL) {
      free(file_descriptors[i]);
    }
  }
  if (file_descriptors != NULL) {
    free(file_descriptors);
  }
  while (file_list != NULL) {
    ufs_delete(file_list->name);
  }
  file_descriptor_count = 0;
  file_descriptor_capacity = 0;
}