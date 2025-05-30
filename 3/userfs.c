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
  size_t size;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
  struct file *file;
  enum open_flags flags;
  int file_offset;
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
      ufs_error_code = UFS_ERR_NO_MEM;
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
    current_file->block_list = b;
    current_file->last_block = b;
    current_file->number_block = 1;
    current_file->size = 0;
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
  if (fd == -1) {
    int new_capacity = file_descriptor_capacity * 2;
    if (new_capacity == 0)
      new_capacity = 10;

    struct filedesc **new_fds =
        realloc(file_descriptors, new_capacity * sizeof(struct filedesc *));
    if (new_fds == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    file_descriptors = new_fds;
    for (int i = file_descriptor_capacity; i < new_capacity; ++i) {
      file_descriptors[i] = NULL;
    }
    fd = file_descriptor_capacity;
    file_descriptor_capacity = new_capacity;
  }
  struct filedesc *desc = calloc(1, sizeof(struct filedesc));
  if (desc == NULL) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }
  desc->file = current_file;
  desc->file_offset = 0;

  if (!(flags & (UFS_READ_ONLY | UFS_WRITE_ONLY | UFS_READ_WRITE))) {
    if ((flags & UFS_CREATE) || (flags == 0)) {
      desc->flags = flags | UFS_READ_WRITE;
    } else {
      desc->flags = flags;
    }
  } else {
    desc->flags = flags;
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
  struct filedesc *desc = file_descriptors[fd];
  struct file *file = desc->file;
  if (!(desc->flags & UFS_WRITE_ONLY) && !(desc->flags & UFS_READ_WRITE)) {
    ufs_error_code = UFS_ERR_NO_PERMISSION;
    return -1;
  }
  if (desc->file_offset + size > MAX_FILE_SIZE) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }
  size_t written = 0;
  size_t offset = 0;
  int current_file_offset = desc->file_offset;
  while (written < size) {
    int count_number = current_file_offset / BLOCK_SIZE;
    int block_offset = current_file_offset % BLOCK_SIZE;

    struct block *current_block = file->block_list;
    int index_block = 0;
    while (current_block != NULL && index_block < count_number) {
      current_block = current_block->next;
      index_block++;
    }
    while (index_block <= count_number) {
      if (current_block == NULL) {
        if (((size_t)(file->number_block + 1)) * BLOCK_SIZE > MAX_FILE_SIZE) {
          ufs_error_code = UFS_ERR_NO_MEM;
          return written;
        }

        struct block *new_block = calloc(1, sizeof(struct block));
        if (new_block == NULL) {
          ufs_error_code = UFS_ERR_NO_MEM;
          return written;
        }
        new_block->memory = calloc(BLOCK_SIZE, 1);
        if (new_block->memory == NULL) {
          free(new_block);
          ufs_error_code = UFS_ERR_NO_MEM;
          return written;
        }

        if (file->last_block == NULL) {
          file->block_list = new_block;
        } else {
          file->last_block->next = new_block;
          new_block->prev = file->last_block;
        }
        file->last_block = new_block;
        file->number_block++;

        current_block = new_block;
      }
      break;
    }
    if (current_block == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return written;
    }
    size_t available = BLOCK_SIZE - block_offset;
    size_t to_write =
        (size - written < available) ? (size - written) : available;

    memcpy(current_block->memory + block_offset, buf + offset, to_write);
    written += to_write;
    offset += to_write;
    current_file_offset += to_write;
    if (current_block->occupied < block_offset + (int)to_write) {
      current_block->occupied = block_offset + to_write;
    }
  }
  desc->file_offset = current_file_offset;
  if ((size_t)desc->file_offset > file->size) {
    file->size = desc->file_offset;
  }
  return written;
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
  ufs_error_code = UFS_ERR_NO_ERR;
  if (check_file(fd) == -1) {
    return -1;
  }
  struct filedesc *desc = file_descriptors[fd];
  struct file *file = desc->file;
  if (!(desc->flags & UFS_READ_ONLY) && !(desc->flags & UFS_READ_WRITE)) {
    ufs_error_code = UFS_ERR_NO_PERMISSION;
    return -1;
  }
  size_t read = 0;
  size_t offset = 0;
  int current_file_offset = desc->file_offset;
  while (read < size) {
    int block_index = current_file_offset / BLOCK_SIZE;
    int block_offset = current_file_offset % BLOCK_SIZE;
    struct block *current_block = file->block_list;
    for (int i = 0; i < block_index; ++i) {
      if (current_block == NULL) {
        break;
      }
      current_block = current_block->next;
    }
    if (current_block == NULL) {
      break;
    }
    size_t available = current_block->occupied - block_offset;
    if (available <= 0) {
      break;
    }
    size_t to_read = (size - read < available) ? (size - read) : available;

    memcpy(buf + offset, current_block->memory + block_offset, to_read);
    read += to_read;
    offset += to_read;
    current_file_offset += to_read;
  }
  desc->file_offset = current_file_offset;
  return read;
}

int ufs_close(int fd) {
  ufs_error_code = UFS_ERR_NO_ERR;
  if (check_file(fd) == -1) {
    return -1;
  }
  file_descriptors[fd]->file->refs--;
  if (file_descriptors[fd]->file->metka &&
      file_descriptors[fd]->file->refs == 0) {
    free_block(file_descriptors[fd]->file);
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
  ufs_error_code = UFS_ERR_NO_ERR;
  if (check_file(fd) == -1) {
    return -1;
  }
  struct filedesc *desc = file_descriptors[fd];
  struct file *file = desc->file;
  if (!(desc->flags & UFS_WRITE_ONLY) && !(desc->flags & UFS_READ_WRITE)) {
    ufs_error_code = UFS_ERR_NO_PERMISSION;
    return -1;
  }
  size_t size = file->size;
  if (new_size == size) {
    return 0;
  }
  if (new_size > MAX_FILE_SIZE) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }
  if (new_size < size) {
    int index_block = (new_size == 0) ? -1 : ((int)new_size - 1) / BLOCK_SIZE;
    int offset = (new_size == 0) ? 0 : (new_size - 1) % BLOCK_SIZE + 1;
    struct block *current = file->block_list;
    struct block *prev_block = NULL;
    int block_count = 0;
    while (current != NULL && block_count <= index_block) {
      prev_block = current;
      current = current->next;
      block_count++;
    }
    while (current != NULL) {
      struct block *delete = current;
      current = current->next;
      free(delete->memory);
      free(delete);
      file->number_block--;
    }
    if (prev_block != NULL) {
      prev_block->next = NULL;
      file->last_block = prev_block;
      prev_block->occupied = offset;
      memset(prev_block->memory + offset, 0, BLOCK_SIZE - offset);
    } else {
      file->block_list = NULL;
      file->last_block = NULL;
      file->number_block = 0;
    }
  } else {
    int index_block = (new_size - 1) / BLOCK_SIZE;
    int offset = (new_size - 1) % BLOCK_SIZE + 1;

    if (file->block_list == NULL) {
      struct block *new_block = calloc(1, sizeof(struct block));
      if (new_block == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      new_block->memory = calloc(BLOCK_SIZE, 1);
      if (new_block->memory == NULL) {
        free(new_block);
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      file->block_list = new_block;
      file->last_block = new_block;
      file->number_block = 1;
    }

    while (file->number_block <= index_block) {
      if (((size_t)(file->number_block + 1)) * BLOCK_SIZE > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        file->size = file->number_block * BLOCK_SIZE;
        if (file->last_block != NULL) {
          file->size -= (BLOCK_SIZE - file->last_block->occupied);
        }
        for (int i = 0; i < file_descriptor_capacity; ++i) {
          if (file_descriptors[i] != NULL &&
              file_descriptors[i]->file == file &&
              (size_t)file_descriptors[i]->file_offset > file->size) {
            file_descriptors[i]->file_offset = file->size;
          }
        }
        return -1;
      }
      struct block *new_block = calloc(1, sizeof(struct block));
      if (new_block == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      new_block->memory = calloc(BLOCK_SIZE, 1);
      if (new_block->memory == NULL) {
        free(new_block);
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
      }
      file->last_block->next = new_block;
      new_block->prev = file->last_block;
      file->last_block = new_block;
      file->number_block++;
    }
    if (file->last_block != NULL) {
      if (file->last_block->occupied < offset) {
        memset(file->last_block->memory + file->last_block->occupied, 0,
               offset - file->last_block->occupied);
      }
      file->last_block->occupied = offset;
      if (new_size > 0 && new_size % BLOCK_SIZE == 0) {
        file->last_block->occupied = BLOCK_SIZE;
      }
    }
  }
  file->size = new_size;
  for (int i = 0; i < file_descriptor_capacity; ++i) {
    if (file_descriptors[i] != NULL && file_descriptors[i]->file == file &&
        (size_t)file_descriptors[i]->file_offset > new_size) {
      file_descriptors[i]->file_offset = new_size;
    }
  }
  return 0;
}

#endif

void ufs_destroy(void) {
  for (int i = 0; i < file_descriptor_capacity; ++i) {
    if (file_descriptors[i] != NULL) {
      ufs_close(i);
    }
  }
  if (file_descriptors != NULL) {
    free(file_descriptors);
    file_descriptors = NULL;
  }
  struct file *current_file = file_list;
  while (current_file != NULL) {
    struct file *next_file = current_file->next;
    struct block *current_block = current_file->block_list;
    while (current_block != NULL) {
      struct block *next_block = current_block->next;
      free(current_block->memory);
      free(current_block);
      current_block = next_block;
    }
    free(current_file->name);
    free(current_file);
    current_file = next_file;
  }
  file_descriptor_capacity = 0;
  file_descriptor_count = 0;
  file_list = NULL;
  ufs_error_code = UFS_ERR_NO_ERR;
}
