#define _GNU_SOURCE
#include "cpu_capacity.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *luthr_quota_name(enum luthr_quota status) {
  return status == LUTHR_QUOTA_LIMITED     ? "limited"
         : status == LUTHR_QUOTA_UNLIMITED ? "unlimited"
                                           : "unknown";
}

static int token(const char *list, const char *wanted) {
  size_t size = strlen(wanted);
  while (*list) {
    const char *end = strchr(list, ',');
    size_t length = end == NULL ? strlen(list) : (size_t)(end - list);
    if (length == size && memcmp(list, wanted, size) == 0)
      return 1;
    if (end == NULL)
      break;
    list = end + 1;
  }
  return 0;
}

static char *decode(const char *text) {
  char *copy = malloc(strlen(text) + 1), *out = copy;
  if (copy == NULL)
    return NULL;
  while (*text) {
    if (text[0] == '\\' && text[1] >= '0' && text[1] <= '7' && text[2] >= '0' &&
        text[2] <= '7' && text[3] >= '0' && text[3] <= '7') {
      *out++ =
          (char)((text[1] - '0') * 64 + (text[2] - '0') * 8 + text[3] - '0');
      text += 4;
    } else
      *out++ = *text++;
  }
  *out = '\0';
  return copy;
}

static int path_prefix(const char *path, const char *root) {
  size_t size = strlen(root);
  return strcmp(root, "/") == 0 || (strncmp(path, root, size) == 0 &&
                                    (path[size] == '/' || path[size] == '\0'));
}

static char *group_path(const char *file, int *version) {
  FILE *input = fopen(file, "r");
  char *line = NULL, *unified = NULL, *cpu = NULL;
  size_t size = 0;
  if (input == NULL)
    return NULL;
  while (getline(&line, &size, input) >= 0) {
    char *first = strchr(line, ':'), *second;
    if (first == NULL || (second = strchr(first + 1, ':')) == NULL)
      continue;
    *second++ = '\0';
    second[strcspn(second, "\n")] = '\0';
    if (token(first + 1, "cpu")) {
      free(cpu);
      cpu = decode(second);
    } else if (first[1] == '\0') {
      free(unified);
      unified = decode(second);
    }
  }
  free(line);
  fclose(input);
  if (cpu != NULL) {
    free(unified);
    *version = 1;
    return cpu;
  }
  *version = 2;
  return unified;
}

static int mount_path(const char *file, const char *group, int version,
                      char **root_out, char **point_out) {
  FILE *input = fopen(file, "r");
  char *line = NULL;
  size_t size = 0, best = 0;
  if (input == NULL)
    return -1;
  while (getline(&line, &size, input) >= 0) {
    char *separator = strstr(line, " - "), *cursor = line;
    char *root, *point, *field, *type, *options;
    int i;
    if (separator == NULL)
      continue;
    *separator = '\0';
    for (i = 0; i < 3; ++i)
      if (strsep(&cursor, " ") == NULL)
        break;
    field = strsep(&cursor, " ");
    root = field != NULL ? decode(field) : NULL;
    field = strsep(&cursor, " ");
    point = field != NULL ? decode(field) : NULL;
    cursor = separator + 3;
    type = strsep(&cursor, " ");
    (void)strsep(&cursor, " "); /* mount source */
    options = strsep(&cursor, " \n");
    if (root != NULL && point != NULL && type != NULL && options != NULL &&
        ((version == 2 && strcmp(type, "cgroup2") == 0) ||
         (version == 1 && strcmp(type, "cgroup") == 0 &&
          token(options, "cpu"))) &&
        path_prefix(group, root) && strlen(root) >= best) {
      free(*root_out);
      free(*point_out);
      *root_out = root;
      *point_out = point;
      best = strlen(root);
    } else {
      free(root);
      free(point);
    }
  }
  free(line);
  fclose(input);
  return *root_out == NULL ? -1 : 0;
}

/* Missing controller files are valid; a missing cgroup directory is unknown. */
static int quota_at(struct luthr_capacity *capacity, const char *directory,
                    int version) {
  char *path = NULL, word[32], extra;
  long long quota = 0, period = 0;
  FILE *input;
  int result, absent;
  if (asprintf(&path, "%s/%s", directory,
               version == 2 ? "cpu.max" : "cpu.cfs_quota_us") < 0)
    return -1;
  input = fopen(path, "r");
  free(path);
  absent = input == NULL && errno == ENOENT;
  if (input == NULL && !absent)
    return -1;
  if (version == 2) {
    if (absent)
      return 0;
    result = fscanf(input, "%31s %lld %c", word, &period, &extra);
    fclose(input);
    if (result != 2 || period <= 0)
      return -1;
    if (strcmp(word, "max") == 0)
      return 0;
    errno = 0;
    char *end;
    quota = strtoll(word, &end, 10);
    if (errno != 0 || *end != '\0' || quota <= 0)
      return -1;
  } else {
    result = absent ? 0 : fscanf(input, "%lld %c", &quota, &extra);
    if (input != NULL)
      fclose(input);
    if (!absent && result != 1)
      return -1;
    if (asprintf(&path, "%s/cpu.cfs_period_us", directory) < 0)
      return -1;
    input = fopen(path, "r");
    free(path);
    if (input == NULL)
      return absent && errno == ENOENT ? 0 : -1;
    result = fscanf(input, "%lld %c", &period, &extra);
    fclose(input);
    if (absent || result != 1 || period <= 0 || quota == 0)
      return -1;
    if (quota < 0)
      return 0;
  }
  if (capacity->status != LUTHR_QUOTA_LIMITED ||
      (long double)quota / period <
          (long double)capacity->quota_us / capacity->period_us) {
    capacity->status = LUTHR_QUOTA_LIMITED;
    capacity->quota_us = (uint64_t)quota;
    capacity->period_us = (uint64_t)period;
  }
  return 0;
}

int luthr_capacity_read(struct luthr_capacity *capacity, const char *groups,
                        const char *mounts) {
  char *group = NULL, *root = NULL, *point = NULL, *directory = NULL;
  int version, result = -1;
  memset(capacity, 0, sizeof(*capacity));
  if (sched_getaffinity(0, sizeof(capacity->affinity), &capacity->affinity) !=
      0)
    return -1;
  capacity->cpus = (unsigned)CPU_COUNT(&capacity->affinity);
  if (capacity->cpus == 0)
    return -1;
  group = group_path(groups, &version);
  if (group == NULL || group[0] != '/' || strstr(group, "/../") != NULL ||
      strcmp(group, "/..") == 0 ||
      (strlen(group) >= 3 && strcmp(group + strlen(group) - 3, "/..") == 0) ||
      mount_path(mounts, group, version, &root, &point) != 0)
    goto done;
  if (asprintf(&directory, "%s%s", point,
               strcmp(root, "/") == 0 ? group : group + strlen(root)) < 0) {
    directory = NULL;
    goto done;
  }
  /* Normalize only the doubled slash from a filesystem mounted at '/'. */
  if (strcmp(point, "/") == 0 && directory[1] == '/')
    memmove(directory, directory + 1, strlen(directory));
  size_t length = strlen(directory);
  if (length > 1 && directory[length - 1] == '/')
    directory[length - 1] = '\0';
  capacity->status = LUTHR_QUOTA_UNLIMITED;
  for (;;) {
    struct stat status;
    char *slash;
    if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode) ||
        quota_at(capacity, directory, version) != 0)
      goto done;
    if (strcmp(directory, point) == 0)
      break;
    slash = strrchr(directory, '/');
    if (slash == directory && strcmp(point, "/") == 0) {
      slash[1] = '\0';
      continue;
    }
    if (slash == NULL || (size_t)(slash - directory) < strlen(point))
      goto done;
    *slash = '\0';
  }
  result = 0;
done:
  free(group);
  free(root);
  free(point);
  free(directory);
  if (result != 0) {
    capacity->status = LUTHR_QUOTA_UNKNOWN;
    capacity->quota_us = capacity->period_us = 0;
  } else {
    long double effective = capacity->cpus;
    if (capacity->status == LUTHR_QUOTA_LIMITED &&
        (long double)capacity->quota_us / capacity->period_us < effective)
      effective = (long double)capacity->quota_us / capacity->period_us;
    capacity->effective_milli = (uint64_t)(effective * 1000);
  }
  return 0;
}
