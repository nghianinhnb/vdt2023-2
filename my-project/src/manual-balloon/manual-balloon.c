#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_VM_NAME_LENGTH 100

#define CONFIG_FILE "/etc/balloon/default.conf"
#define ERROR_LOG_FILE "/var/log/balloon/error.log"

typedef struct {
    float low_threshold;
    float high_threshold;
    unsigned int interval;
    unsigned int speed;
} balloon_config;

typedef struct {
    char *name;
    unsigned int used;
    unsigned int actual;
    unsigned int available;
} vm_info;

void err_log(const char *format, ...) {
    FILE *f = fopen(ERROR_LOG_FILE, "a");
    if (f != NULL) {
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fclose(f);
    }
}

void setmem(vm_info *vm, int inc_ram_amount) {
    char command[256];
    snprintf(command, sizeof(command), "virsh setmem --domain %s --size %um", vm->name, vm->actual + inc_ram_amount);
    int status = system(command);

    if (status == -1) {
        err_log("[setmem] System error")
    } else if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) != 0) {
            err_log("[setmem] 'virsh setmem' with exit status %d\n", WEXITSTATUS(status))
        }
    } else {
        err_log( "[setmem] 'virsh setmem' didn't exit properly\n")
    }
}

char** get_running_vm_names() {
    FILE *fp;
    char buffer[MAX_VM_NAME_LENGTH];
    char **vmList = NULL;
    int vmCount = 0;

    fp = popen("virsh list --name", "r");
    if (fp == NULL) {
        perror("popen");
        return NULL;
    }

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (strlen(buffer) <= 1) {
            continue;
        }

        size_t length = strlen(buffer);
        if (buffer[length - 1] == '\n') {
            buffer[length - 1] = '\0';
        }

        char **newVmList = realloc(vmList, (vmCount + 1) * sizeof(char *));
        if (newVmList == NULL) {
            perror("realloc");
            free(vmList);
            pclose(fp);
            return NULL;
        }
        vmList = newVmList;

        vmList[vmCount] = strdup(buffer);
        if (vmList[vmCount] == NULL) {
            perror("strdup");
            free(vmList);
            pclose(fp);
            return NULL;
        }

        vmCount++;
    }

    pclose(fp);
    return vmList;
}

vm_info get_vm_mem_info(const char *vm_name) {
    vm_info vm;
    vm.name = strdup(vm_name);
    vm.used = 0;
    vm.actual = 0;
    vm.available = 0;

    char command[MAX_VM_NAME_LENGTH + 32];
    snprintf(command, sizeof(command), "virsh dommemstat %s", vm_name);

    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        perror("popen");
        return vm;
    }

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        char *token = strtok(buffer, " \t\n");
        if (token != NULL) {
            if (strcmp(token, "actual") == 0) {
                token = strtok(NULL, " \t\n");
                if (token != NULL) {
                    vm.actual = atoi(token) >> 10;
                }
            } else if (strcmp(token, "available") == 0) {
                token = strtok(NULL, " \t\n");
                if (token != NULL) {
                    vm.available = atoi(token) >> 10;
                }
            } else if (strcmp(token, "rss") == 0) {
                token = strtok(NULL, " \t\n");
                if (token != NULL) {
                    vm.used = atoi(token) >> 10;
                }
            }
        }
    }

    pclose(fp);
    return vm;
}

void read_config(balloon_config *config) {
    char config_buffer[256];
    FILE *file = fopen(CONFIG_FILE, "r");
    if (file == NULL) {
        goto out_default;
    }
    if (fgets(config_buffer, sizeof(config_buffer), file) == NULL) {
        goto out_close_file;
    }
    if (sscanf(config_buffer, "low_threshold=%f\nhigh_threshold=%f\ninterval=%u\nspeed=%u",
               &config->low_threshold, &config->high_threshold, &config->interval, &config->speed) != 4) {
        goto out_close_file;
    }
    fclose(file);
    return;

out_close_file:
    fclose(file);
out_default:
    err_log("[read_config] Error reading config file. Use default config")
    config->low_threshold = 0.7;
    config->high_threshold = 0.85;
    config->interval = 10;
    config->speed = 32 << 10;
}


void ballooning() {
    struct sysinfo info;
    balloon_config config;

    for (;;) {
        read_config(&config);
        char** vm_list = get_running_vm_names();

        for (int i = 0; vm_list[i] != NULL; i++) {
            vm_info vm = get_vm_mem_info(vm_list[i]);
            if (!vm.available) continue;

            float used_percent = (float) vm.used / vm.available;

            if (used_percent < config.low_threshold) {
                setmem(&vm, config.speed);
            } else if (used_percent >= config.high_threshold) {
                setmem(&vm, -config.speed);
            }

            free(vm.name);
        }

        free(vm_list);
        sleep(config.interval);
    }
}

int main() {
    ballooning();
    return 0;
}
