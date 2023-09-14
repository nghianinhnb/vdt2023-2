#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysinfo.h>

#include <libvirt/libvirt.h>

#define MAX_VM_NAME_LENGTH 64
#define MAX_NUM_OF_VM 1024

#define CONFIG_LOW_THRESHOLD_DEFAULT 0.7
#define CONFIG_HIGH_THRESHOLD_DEFAULT 0.85
#define CONFIG_INTERVAL_DEFAULT (long int) 5
#define CONFIG_SPEED_DEFAULT (long int) (32 << 10)

#define CONFIG_FILE "/etc/balloon/default.conf"
#define ERROR_LOG_FILE "/var/log/balloon/error.log"

typedef struct {
    float low_threshold;
    float high_threshold;
    long int interval;
    long int speed;
} balloon_config;

typedef struct {    // in KB
    long int actual;
    long int available;
    long int max;
} vm_info;

virConnectPtr connection = NULL;

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

int create_file_if_not_exist() {
    int status = 0;
    status = system("mkdir -p /etc/balloon");
    status = system("mkdir -p /var/log/balloon");
    generate_default_config_file();
    return status;
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

vm_info get_vm_info(virDomainPtr dom) {
    vm_info vm;
    virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];

    vm.actual = 0;
    vm.available = 0;
    vm.max = virDomainGetMaxMemory(dom);

    int numStats = virDomainMemoryStats(dom, stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
    for (int i = 0; i < numStats; i++) {
        if (stats[i].tag == VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON)
            vm.actual = stats[i].val;
        else if (stats[i].tag == VIR_DOMAIN_MEMORY_STAT_USABLE) {
            vm.available = stats[i].val;
        }
    }
    return vm;
}

void ballooning() {
    balloon_config config;
    int vm_ids[MAX_NUM_OF_VM];
    int num_VMs, i;
    char time_stamp[20];

    for (;;) {
        read_config(&config);
        num_VMs = virConnectListDomains(connection, vm_ids, MAX_NUM_OF_VM);

        for (i = 0; i < num_VMs; i++) {
            virDomainPtr dom = virDomainLookupByID(connection, vm_ids[i]);
            if (!dom) continue;

            virDomainSetMemoryStatsPeriod(dom, config.interval, 0);
            vm_info vm = get_vm_info(dom);
            if (!vm.actual || !vm.available || !vm.max ) {
                virDomainFree(dom);
                continue;
            }

            float pressure = (float)(vm.max - vm.available) / vm.max;
            if (pressure < config.low_threshold) {
                virDomainSetMemory(dom, vm.actual - config.speed);
            } 
            else if (pressure >= config.high_threshold) {
                virDomainSetMemory(dom, vm.actual + 2*config.speed);
            }

            fprintf(stdout, "[%s]: used:%ldMB | free: %ldMB | current: %ldMB | max: %ldMB | pressure: %.2f%%\n",
                virDomainGetName(dom), (vm.actual - vm.available) >> 10, vm.available >> 10, vm.actual >> 10, vm.max >> 10, pressure * 100);

            // strftime(time_stamp, sizeof(time_stamp), "%Y-%m-%d %H:%M:%S", localtime(&time(NULL)));

            // fprintf(stdout, "timestamp=\"%s\" domain=\"%s\" free_memory_bytes=%ld current_memory_bytes=%ld max_memory_bytes=%ld memory_pressure=%.2f%%\n",
            //     time_stamp, virDomainGetName(dom), vm.available << 10, vm.actual << 10, vm.max << 10, pressure);

            virDomainFree(dom);
        }
        sleep(config.interval);
    }
}

int main() {
    create_file_if_not_exist();

    connection = virConnectOpen("qemu:///system");
    if (connection == NULL) {
        fprintf(stderr, "Failed to open connection to the hypervisor\n");
        return 1;
    }

    ballooning();

    virConnectClose(connection);

    return 0;
}
