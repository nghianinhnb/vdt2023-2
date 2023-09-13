#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysinfo.h>

#define MAX_VM_NAME_LENGTH 64

#define CONFIG_LOW_THRESHOLD_DEFAULT 0.7
#define CONFIG_HIGH_THRESHOLD_DEFAULT 0.85
#define CONFIG_INTERVAL_DEFAULT (long int) 5
#define CONFIG_SPEED_DEFAULT (long int) (64 << 10)

#define CONFIG_FILE "/etc/balloon/default.conf"
#define ERROR_LOG_FILE "/var/log/balloon/error.log"

typedef struct {
    float low_threshold;
    float high_threshold;
    long int interval;
    long int speed;
} balloon_config;

typedef struct {
    char *name;
    long int used;
    long int actual;
    long int available;
} vm_info;

void err_log(const char *format, ...) {
    FILE *f = fopen(ERROR_LOG_FILE, "a+");
    if (f != NULL) {
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fclose(f);
    }
}

void setmem(vm_info *vm, long int inc_ram_amount) {
    char command[256];
    snprintf(command, sizeof(command), "virsh setmem --domain %s --size %ldm", vm->name, vm->actual + inc_ram_amount);
    int status = system(command);

    if (status == -1) {
        err_log("[%s] System error\n", __func__);
    } else if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) != 0) {
            err_log("[%s] 'virsh setmem' with exit status %d\n", __func__, WEXITSTATUS(status));
        }
    } else {
        err_log( "[%s] 'virsh setmem' didn't exit properly\n", __func__);
    }
}

char** get_running_vm_names() {
    FILE *fp;
    char buffer[MAX_VM_NAME_LENGTH];
    char **vmList = NULL;
    unsigned int vmCount = 0;

    fp = popen("virsh list --name", "r");
    if (fp == NULL) {
        goto out;
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
            goto out_free_list;
        }
        vmList = newVmList;

        vmList[vmCount] = strdup(buffer);
        if (vmList[vmCount] == NULL) {
            goto out_free_list;
        }

        vmCount++;
    }

    pclose(fp);
    return vmList;

out_free_list:
    free(vmList);
    pclose(fp);
out:
    err_log( "[%s] collect vm's names fail\n", __func__);
    return NULL;
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
        err_log( "[%s] get %s's memory statistic fail\n", __func__, vm_name);
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

void generate_default_config_file() {
    FILE *file = fopen(CONFIG_FILE, "w");
    if (file == NULL) {
        err_log("[%s] Error creating config file\n", __func__);
        return;
    }
    fprintf(file, "low_threshold=%f\n", CONFIG_LOW_THRESHOLD_DEFAULT);
    fprintf(file, "high_threshold=%f\n", CONFIG_HIGH_THRESHOLD_DEFAULT);
    fprintf(file, "interval=%ld\n", CONFIG_INTERVAL_DEFAULT);
    fprintf(file, "speed=%ld", CONFIG_SPEED_DEFAULT);
    fclose(file);
}

void read_config(balloon_config *config) {
    char config_buffer[256];
    FILE *file = fopen(CONFIG_FILE, "r");
    if (file == NULL) {
        generate_default_config_file();
        goto out_default;
    }
    if (!fread(config_buffer, 1, sizeof(config_buffer), file)) {
        goto out_close_file;
    }
    if (sscanf(config_buffer, "low_threshold=%f high_threshold=%f interval=%ld speed=%ld",
            &config->low_threshold, &config->high_threshold, &config->interval, &config->speed) != 4) {
        goto out_close_file;
    }
    fclose(file);
    return;

out_close_file:
    fclose(file);
out_default:
    err_log("[%s] Error read config file. Use default config\n", __func__);
    config->low_threshold = CONFIG_LOW_THRESHOLD_DEFAULT;
    config->high_threshold = CONFIG_HIGH_THRESHOLD_DEFAULT;
    config->interval = CONFIG_INTERVAL_DEFAULT;
    config->speed = CONFIG_SPEED_DEFAULT;
}

void ballooning() {
    struct sysinfo info;
    balloon_config config;
    char** vm_list;

    for (;;) {
        fprintf(stdout, "ballooning...\n");
        read_config(&config);
        vm_list = get_running_vm_names();

        if (vm_list != NULL) {
            for (unsigned int i = 0; vm_list[i] != NULL; i++) {
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
        }
        sleep(config.interval);
    }
}

int create_file_if_not_exist() {
    int status = 0;
    if (status = system("mkdir -p /etc/balloon")) goto out;
    generate_default_config_file();
    if (status = system("mkdir -p /var/log/balloon")) goto out;
out:
    return status;
}

int main() {
    create_file_if_not_exist();
    ballooning();
    return 0;
}
