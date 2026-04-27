#include "cgroup.h"

char* trim_whitespace(char *str) {
    char *end;
    
    /* Trim leading space */
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) {
        return str;
    }
    
    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    /* Write new null terminator */
    *(end + 1) = '\0';
    
    return str;
}

void cgroup_root(CGroup *cg) {
    strncpy(cg->path, "/sys/fs/cgroup", MAX_PATH_LEN - 1);
    cg->path[MAX_PATH_LEN - 1] = '\0';
}

int cgroup_is_root(const CGroup *cg) {
    return strcmp(cg->path, "/sys/fs/cgroup/") == 0 || strcmp(cg->path, "/sys/fs/cgroup") == 0;
}

void cgroup_from_path(CGroup *cg, const char *path) {
    if (strncmp(path, "/sys/fs/cgroup/", 15) != 0) {
        fprintf(stderr, "Error: Path must start with /sys/fs/cgroup/\n");
        return;
    }
    strncpy(cg->path, path, MAX_PATH_LEN - 1);
    cg->path[MAX_PATH_LEN - 1] = '\0';
}

int cgroup_get_descendants(const CGroup *cg, CGroup **descendants, int max_count) {
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char full_path[MAX_PATH_LEN];
    int count = 0;
    
    dir = opendir(cg->path);
    if (!dir) {
        return 0;
    }
    
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(full_path, MAX_PATH_LEN, "%s/%s", cg->path, entry->d_name);
        
        if (stat(full_path, &st) != 0) {
            continue;
        }
        
        if (!S_ISDIR(st.st_mode)) {
            continue;
        }
        
        descendants[count] = malloc(sizeof(CGroup));
        if (descendants[count]) {
            cgroup_from_path(descendants[count], full_path);
            count++;
        }
    }
    
    closedir(dir);
    return count;
}

void cgroup_get_name(const CGroup *cg, char *name, size_t name_size) {
    if (cgroup_is_root(cg)) {
        name[0] = '\0';
        return;
    }
    
    char *last_slash = strrchr(cg->path, '/');
    if (last_slash && last_slash[1] != '\0') {
        strncpy(name, last_slash + 1, name_size - 1);
        name[name_size - 1] = '\0';
    } else {
        name[0] = '\0';
    }
}

int cgroup_get_parent(const CGroup *cg, CGroup *parent) {
    if (cgroup_is_root(cg)) {
        return 0;
    }
    
    char *last_slash = strrchr(cg->path, '/');
    if (!last_slash || last_slash == cg->path) {
        return 0;
    }
    
    size_t parent_len = last_slash - cg->path;
    if (parent_len == 0) {
        return 0;
    }
    
    strncpy(parent->path, cg->path, parent_len);
    parent->path[parent_len] = '\0';
    
    /* Handle root case */
    if (strcmp(parent->path, "/sys/fs/cgroup") == 0) {
        /* Keep it as is */
    }
    
    return 1;
}

int cgroup_get_active_controllers(const CGroup *cg, char controllers[][256], int max_count) {
    char filepath[MAX_PATH_LEN];
    FILE *fp;
    char line[MAX_LINE_LEN];
    int count = 0;
    
    snprintf(filepath, MAX_PATH_LEN, "%s/cgroup.subtree_control", cg->path);
    
    fp = fopen(filepath, "r");
    if (!fp) {
        return -1;
    }
    
    if (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim_whitespace(line);
        char *token = strtok(trimmed, " ");
        
        while (token && count < max_count) {
            if (strlen(token) > 0) {
                strncpy(controllers[count], token, 255);
                controllers[count][255] = '\0';
                count++;
            }
            token = strtok(NULL, " ");
        }
    }
    
    fclose(fp);
    return count;
}

int cgroup_add_controller(CGroup *cg, const char *controller) {
    char filepath[MAX_PATH_LEN];
    FILE *fp;
    char control[256];
    
    snprintf(filepath, MAX_PATH_LEN, "%s/cgroup.subtree_control", cg->path);
    
    snprintf(control, sizeof(control), "+%s", controller);
    
    fp = fopen(filepath, "w");
    if (!fp) {
        if (errno == ENOENT) {
            return -ENOENT;
        }
        return -errno;
    }
    
    fprintf(fp, "%s", control);
    fclose(fp);
    
    return 0;
}

int parse_limits_file(const char *filepath, DMemLimit *limit) {
    FILE *fp;
    char line[MAX_LINE_LEN];
    
    fp = fopen(filepath, "r");
    if (!fp) {
        return 0;
    }
    
    limit->count = 0;
    
    while (fgets(line, sizeof(line), fp) && limit->count < MAX_DEVICES) {
        char *trimmed = trim_whitespace(line);
        if (strlen(trimmed) == 0) {
            continue;
        }
        
        char device_name[256];
        char value_str[256];
        
        if (sscanf(trimmed, "%255s %255s", device_name, value_str) != 2) {
            fprintf(stderr, "WARNING: Unexpected number of words in dmem limit string: \"%s\"\n", trimmed);
            continue;
        }
        
        if (strcmp(value_str, "max") == 0) {
            strncpy(limit->entries[limit->count].device_name, device_name, 255);
            limit->entries[limit->count].device_name[255] = '\0';
            limit->entries[limit->count].limit = ULLONG_MAX;
            limit->count++;
        } else {
            char *endptr;
            unsigned long long val = strtoull(value_str, &endptr, 10);
            if (*endptr == '\0') {
                strncpy(limit->entries[limit->count].device_name, device_name, 255);
                limit->entries[limit->count].device_name[255] = '\0';
                limit->entries[limit->count].limit = val;
                limit->count++;
            } else {
                fprintf(stderr, "WARNING: Could not parse dmem limit number: \"%s\"\n", trimmed);
            }
        }
    }
    
    fclose(fp);
    return limit->count > 0 ? 1 : 0;
}

void write_limits_file(const char *filepath, const DMemLimit *limit) {
    FILE *fp;
    
    fp = fopen(filepath, "w");
    if (!fp) {
        if (errno != EACCES) {
            fprintf(stderr, "WARNING: Could not write dmem limit file: %s!\n", strerror(errno));
        }
        return;
    }
    
    for (int i = 0; i < limit->count; i++) {
        fprintf(fp, "%s ", limit->entries[i].device_name);
        if (limit->entries[i].limit == ULLONG_MAX) {
            fprintf(fp, "max\n");
        } else {
            fprintf(fp, "%llu\n", limit->entries[i].limit);
        }
    }
    
    fclose(fp);
}

int cgroup_device_memory_capacity(const CGroup *cg, DMemLimit *limit) {
    char filepath[MAX_PATH_LEN];
    
    snprintf(filepath, MAX_PATH_LEN, "%s/dmem.capacity", cg->path);
    
    return parse_limits_file(filepath, limit);
}

void cgroup_write_device_memory_low(CGroup *cg, const DMemLimit *limit) {
    char filepath[MAX_PATH_LEN];
    
    snprintf(filepath, MAX_PATH_LEN, "%s/dmem.low", cg->path);
    
    write_limits_file(filepath, limit);
}
