#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dbus/dbus.h>
#include "cgroup.h"

#define MAX_UNITS 1024
#define CGROUP_BASE "/sys/fs/cgroup"

typedef struct {
    char units[MAX_UNITS][512];
    int count;
} UnitQueue;

/* Forward declarations */
static char* user_slice_id(CGroup *cgroup);
static int try_activate_dmem_controller(CGroup *cgroup, int system);
static void propagate_dmem_activation(CGroup *cgroup, int system);
static void activate_dmem_in_descendants(CGroup *cgroup, int system);
static void handle_new_unit(DBusConnection *connection, const char *unit_path, int system);

static char* user_slice_id(CGroup *cgroup) {
    CGroup parent;
    static char user_id[64];
    
    if (!cgroup_get_parent(cgroup, &parent)) {
        return NULL;
    }
    
    CGroup current = parent;
    
    while (1) {
        char name[256];
        cgroup_get_name(&current, name, sizeof(name));
        
        /* Check if name starts with "user-" and ends with ".slice" */
        if (strncmp(name, "user-", 5) == 0) {
            size_t len = strlen(name);
            if (len > 6 && strcmp(name + len - 6, ".slice") == 0) {
                /* Extract the UID part */
                char uid_str[64];
                strncpy(uid_str, name + 5, len - 6 - 5);
                uid_str[len - 6 - 5] = '\0';
                
                /* Verify it's a valid number */
                char *endptr;
                strtoul(uid_str, &endptr, 10);
                if (*endptr == '\0') {
                    strncpy(user_id, uid_str, sizeof(user_id) - 1);
                    user_id[sizeof(user_id) - 1] = '\0';
                    return user_id;
                }
            }
        }
        
        CGroup grandparent;
        if (!cgroup_get_parent(&current, &grandparent)) {
            break;
        }
        current = grandparent;
    }
    
    return NULL;
}

static int try_activate_dmem_controller(CGroup *cgroup, int system) {
    /* There may be system-level cgroups we need to activate higher in the hierarchy */
    CGroup parent;
    if (cgroup_get_parent(cgroup, &parent)) {
        propagate_dmem_activation(&parent, system);
    }
    
    /* If we're operating on a system level, don't try messing with users' cgroups */
    if (user_slice_id(cgroup) != NULL && system) {
        return 0;
    }
    
    /* Check if we can activate the controller */
    char controllers[MAX_CONTROLLERS][256];
    int ctrl_count = cgroup_get_active_controllers(cgroup, controllers, MAX_CONTROLLERS);
    
    CGroup descendants[256];
    CGroup *desc_ptrs[256];
    int desc_count = cgroup_get_descendants(cgroup, desc_ptrs, 256);
    
    /* Free the allocated descendants */
    for (int i = 0; i < desc_count; i++) {
        free(desc_ptrs[i]);
    }
    
    int can_activate = (desc_count > 0) || (ctrl_count > 0);
    
    if (!can_activate) {
        return 0;
    }
    
    int retry = 0;
    while (1) {
        int result = cgroup_add_controller(cgroup, "dmem");
        
        if (result < 0) {
            if (result == -ENOENT && !retry) {
                if (cgroup_get_parent(cgroup, &parent)) {
                    propagate_dmem_activation(&parent, system);
                    retry = 1;
                    continue;
                }
            }
            return -1;
        } else {
            return 0;
        }
    }
}

static void propagate_dmem_activation(CGroup *cgroup, int system) {
    char controllers[MAX_CONTROLLERS][256];
    int ctrl_count = cgroup_get_active_controllers(cgroup, controllers, MAX_CONTROLLERS);
    
    if (ctrl_count < 0) {
        return;
    }
    
    int has_active_dmem = 0;
    for (int i = 0; i < ctrl_count; i++) {
        if (strcmp(controllers[i], "dmem") == 0) {
            has_active_dmem = 1;
            break;
        }
    }
    
    if (!has_active_dmem) {
        if (try_activate_dmem_controller(cgroup, system) < 0) {
            return;
        }
    }
    
    /* Check if we should set limits */
    CGroup parent;
    char *user_id = NULL;
    int should_set_limit = 1;
    
    if (cgroup_get_parent(cgroup, &parent)) {
        user_id = user_slice_id(&parent);
        if (user_id != NULL) {
            if (system) {
                return;
            }
            
            char name[256];
            cgroup_get_name(cgroup, name, sizeof(name));
            
            char user_service_name[512];
            snprintf(user_service_name, sizeof(user_service_name), "user@%s.service", user_id);
            
            /* Only protect app.slice and user@<id>.service */
            if (strcmp(name, "app.slice") != 0 && strcmp(name, user_service_name) != 0) {
                should_set_limit = 0;
            }
        }
    }
    
    if (!should_set_limit) {
        return;
    }
    
    DMemLimit limits;
    if (!cgroup_device_memory_capacity(cgroup, &limits)) {
        return;
    }
    
    cgroup_write_device_memory_low(cgroup, &limits);
}

static void activate_dmem_in_descendants(CGroup *cgroup, int system) {
    CGroup *descendants[256];
    int desc_count = cgroup_get_descendants(cgroup, descendants, 256);
    
    if (desc_count == 0) {
        propagate_dmem_activation(cgroup, system);
        return;
    }
    
    for (int i = 0; i < desc_count; i++) {
        activate_dmem_in_descendants(descendants[i], system);
        free(descendants[i]);
    }
}

static void handle_new_unit(DBusConnection *connection, const char *unit_path, int system) {
    const char *iface_names[] = {
        "org.freedesktop.systemd1.Service",
        "org.freedesktop.systemd1.Scope",
        "org.freedesktop.systemd1.Slice",
        "org.freedesktop.systemd1.Socket",
    };
    int num_ifaces = sizeof(iface_names) / sizeof(iface_names[0]);
    
    char cgroup_path[MAX_PATH_LEN] = {0};
    
    for (int i = 0; i < num_ifaces; i++) {
        DBusMessage *msg;
        DBusMessage *reply;
        DBusError error;
        
        dbus_error_init(&error);
        
        msg = dbus_message_new_method_call(
            "org.freedesktop.DBus.Properties",
            unit_path,
            "org.freedesktop.DBus.Properties",
            "Get"
        );
        
        if (!msg) {
            continue;
        }
        
        const char *iface = iface_names[i];
        const char *property = "ControlGroup";
        
        if (!dbus_message_append_args(msg,
            DBUS_TYPE_STRING, &iface,
            DBUS_TYPE_STRING, &property,
            DBUS_TYPE_INVALID)) {
            dbus_message_unref(msg);
            continue;
        }
        
        reply = dbus_connection_send_with_reply_and_block(connection, msg, 1000, &error);
        dbus_message_unref(msg);
        
        if (dbus_error_is_set(&error)) {
            dbus_error_free(&error);
            continue;
        }
        
        if (reply) {
            DBusMessageIter iter;
            DBusMessageIter variant_iter;
            const char *value;
            
            dbus_message_iter_init(reply, &iter);
            
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&iter, &variant_iter);
                
                if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&variant_iter, &value);
                    strncpy(cgroup_path, value, sizeof(cgroup_path) - 1);
                    cgroup_path[sizeof(cgroup_path) - 1] = '\0';
                    dbus_message_unref(reply);
                    break;
                }
            }
            
            dbus_message_unref(reply);
        }
    }
    
    if (strlen(cgroup_path) > 0) {
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s%s", CGROUP_BASE, cgroup_path);
        
        CGroup cgroup;
        cgroup_from_path(&cgroup, full_path);
        propagate_dmem_activation(&cgroup, system);
    }
}

static void free_unit_queue(UnitQueue *queue) {
    queue->count = 0;
}

int main(int argc, char *argv[]) {
    int system = 0;
    
    /* Check for --use-system-bus flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--use-system-bus") == 0) {
            system = 1;
            break;
        }
    }
    
    DBusError error;
    DBusConnection *connection;
    
    dbus_error_init(&error);
    
    if (system) {
        connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    } else {
        connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    }
    
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "Failed to connect to DBus: %s\n", error.message);
        dbus_error_free(&error);
        return 1;
    }
    
    if (!connection) {
        fprintf(stderr, "Failed to connect to DBus: unknown error\n");
        return 1;
    }
    
    /* Add match for UnitNew signal */
    dbus_bus_add_match(connection,
        "type='signal',"
        "interface='org.freedesktop.systemd1.Manager',"
        "member='UnitNew'",
        &error);
    
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "Failed to add UnitNew match: %s\n", error.message);
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 1;
    }
    
    /* Add match for UnitRemoved signal */
    dbus_bus_add_match(connection,
        "type='signal',"
        "interface='org.freedesktop.systemd1.Manager',"
        "member='UnitRemoved'",
        &error);
    
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "Failed to add UnitRemoved match: %s\n", error.message);
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 1;
    }
    
    /* Activate dmem in all existing descendants */
    CGroup root;
    cgroup_root(&root);
    activate_dmem_in_descendants(&root, system);
    
    UnitQueue unit_queue;
    unit_queue.count = 0;
    
    printf("Listening for systemd unit events...\n");
    
    while (1) {
        /* Process pending DBus messages */
        dbus_connection_read_write_dispatch(connection, 1000);
        
        /* Process signals */
        while (dbus_connection_dispatch(connection) == DBUS_DISPATCH_DATA_REMAINS) {
            /* Continue processing */
        }
        
        /* Read incoming messages manually to extract unit info */
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(connection)) != NULL) {
            if (dbus_message_is_signal(msg, "org.freedesktop.systemd1.Manager", "UnitNew")) {
                const char *unit_name;
                const char *unit_path;
                
                if (dbus_message_get_args(msg, NULL,
                    DBUS_TYPE_STRING, &unit_name,
                    DBUS_TYPE_OBJECT_PATH, &unit_path,
                    DBUS_TYPE_INVALID)) {
                    
                    /* Add to queue */
                    if (unit_queue.count < MAX_UNITS) {
                        strncpy(unit_queue.units[unit_queue.count], unit_path, 511);
                        unit_queue.units[unit_queue.count][511] = '\0';
                        unit_queue.count++;
                    }
                }
            } else if (dbus_message_is_signal(msg, "org.freedesktop.systemd1.Manager", "UnitRemoved")) {
                const char *unit_name;
                const char *unit_path;
                
                if (dbus_message_get_args(msg, NULL,
                    DBUS_TYPE_STRING, &unit_name,
                    DBUS_TYPE_OBJECT_PATH, &unit_path,
                    DBUS_TYPE_INVALID)) {
                    
                    /* Remove from queue */
                    for (int i = 0; i < unit_queue.count; i++) {
                        if (strcmp(unit_queue.units[i], unit_path) == 0) {
                            /* Shift remaining items */
                            for (int j = i; j < unit_queue.count - 1; j++) {
                                strcpy(unit_queue.units[j], unit_queue.units[j + 1]);
                            }
                            unit_queue.count--;
                            break;
                        }
                    }
                }
            }
            
            dbus_message_unref(msg);
        }
        
        /* Process queued units */
        for (int i = 0; i < unit_queue.count; i++) {
            handle_new_unit(connection, unit_queue.units[i], system);
        }
        
        /* Clear queue */
        free_unit_queue(&unit_queue);
    }
    
    dbus_connection_unref(connection);
    
    return 0;
}
