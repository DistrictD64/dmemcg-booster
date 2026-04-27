#ifndef CGROUP_H
#define CGROUP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>

#define MAX_PATH_LEN 4096
#define MAX_LINE_LEN 1024
#define MAX_CONTROLLERS 64
#define MAX_DEVICES 32

typedef struct {
    char device_name[256];
    unsigned long long limit;
} DMemLimitEntry;

typedef struct {
    DMemLimitEntry entries[MAX_DEVICES];
    int count;
} DMemLimit;

typedef struct {
    char path[MAX_PATH_LEN];
} CGroup;

/* Function declarations */
void cgroup_root(CGroup *cg);
int cgroup_is_root(const CGroup *cg);
void cgroup_from_path(CGroup *cg, const char *path);
int cgroup_get_descendants(const CGroup *cg, CGroup **descendants, int max_count);
void cgroup_get_name(const CGroup *cg, char *name, size_t name_size);
int cgroup_get_parent(const CGroup *cg, CGroup *parent);
int cgroup_get_active_controllers(const CGroup *cg, char controllers[][256], int max_count);
int cgroup_add_controller(CGroup *cg, const char *controller);
int parse_limits_file(const char *filepath, DMemLimit *limit);
void write_limits_file(const char *filepath, const DMemLimit *limit);
int cgroup_device_memory_capacity(const CGroup *cg, DMemLimit *limit);
void cgroup_write_device_memory_low(CGroup *cg, const DMemLimit *limit);
char* trim_whitespace(char *str);

#endif /* CGROUP_H */
