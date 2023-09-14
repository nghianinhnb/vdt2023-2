![KVM Logo](https://agix.com.au/wp-content/uploads/2019/02/linux-kvm.png)

**Kernel-based Virtual Machine (KVM)** là một công nghệ ảo hóa nguồn mở được tích hợp
trong Linux. KVM biến Linux thành một trình ảo hóa cho phép máy chủ chạy nhiều môi trường
ảo biệt lập gọi là máy khách (GuestM) hoặc máy ảo (VM).

**VirtIO Memory Ballooning:** một kỹ thuật trong đó máy chủ có thể yêu cầu các máy ảo chạy trên nó trả lại
một phần bộ nhớ đã đăng ký. Phần bộ nhớ này máy chủ có thể phục vụ để chạy các tiến trình
hoặc chạy các máy ảo khác. Việc này được thực hiện bằng cách để máy ảo thổi một bong bóng 
trong bộ nhớ chiếm các phần bộ nhớ đang không sử dụng. Máy chủ tạm thời lấy về bong bóng 
bộ nhớ này. Khi cần, máy chủ trả lại các phần bộ nhớ và bong bóng xẹp xuống. Toàn bộ điều
này xảy ra trong khi máy ảo vẫn đang chạy.

## Môi trường

HĐH: Ubuntu Desktop 22.04

CPU: AMD Ryzen 5 4600H 6cores/12threads

RAM: 16GB

## Cài đặt

Kiểm tra cpu có hỗ trợ ảo hóa hay không:

```console
$ egrep -c '(vmx|svm)' /proc/cpuinfo
$ kvm-ok
```
- vmx (Virtual Machine eXtensions): còn được gọi là VT-x (Intel Virtualization Technology)
cung cấp ảo hóa mức phần cứng cho các vi xử lý Intel
- svm (Secure Virtual Machine): tương tự như VMX của Intel, SVM cung cấp hỗ trợ ảo hóa mức
phần cứng cho các bộ xử lý AMD


**Cài đặt các thư viện:**

```console
$ sudo apt install -y qemu-kvm virt-manager libvirt-daemon-system virtinst libvirt-clients bridge-utils
```
- qemu-kvm: Một trình giả lập và ảo hóa mã nguồn mở cung cấp giả lập phần cứng.
- virt-manager: Giao diện để quản lý các máy ảo thông qua libvirt daemon.
- libvirt-daemon-system: Gói cung cấp các tệp config để chạy libvirt daemon.
- virtinst: CLI để khởi tạo và tùy chỉnh các máy ảo.
- libvirt-clients: Bộ các thư viện và API để quản lý và kiểm soát các máy ảo và hypervisors.
- bridge-utils: Bộ các công cụ để tạo và quản lý các bridge.

Thêm user hiện tại vào nhóm được tạo khi cài đặt thư viện để có quyền sử dụng các
thư viện này
```console
$ sudo usermod -aG kvm $(whoami)
$ sudo usermod -aG libvirt $(whoami)
```

**Khởi động dịch vụ ảo hóa:**

```console
$ sudo systemctl enable --now libvirtd
$ sudo systemctl start libvirtd
```

```console
● libvirtd.service - Virtualization daemon
     Loaded: loaded (/lib/systemd/system/libvirtd.service; enabled; vendor pres>
     Active: active (running) since Thu 2023-07-20 16:39:02 +07; 6h left
TriggeredBy: ● libvirtd-admin.socket
             ● libvirtd.socket
             ● libvirtd-ro.socket
       Docs: man:libvirtd(8)
             https://libvirt.org
   Main PID: 884 (libvirtd)
      Tasks: 21 (limit: 32768)
     Memory: 19.6M
        CPU: 2.896s
     CGroup: /system.slice/libvirtd.service
             ├─ 884 /usr/sbin/libvirtd
             ├─1088 /usr/sbin/dnsmasq --conf-file=/var/lib/libvirt/dnsmasq/defa>
             └─1089 /usr/sbin/dnsmasq --conf-file=/var/lib/libvirt/dnsmasq/defa>

Thg 7 20 16:39:03 VTS-NGHIANV24 dnsmasq-dhcp[1088]: DHCP, sockets bound exclusi>
Thg 7 20 16:39:03 VTS-NGHIANV24 dnsmasq[1088]: reading /etc/resolv.conf
```

## Thực nghiệm

Tạo máy ảo chạy ubuntu server 22.04. Máy được cấp phát 1 core cpu và 2GB
ram.

```console
nghia@VTS-NGHIANV24:~$ sudo virt-install \
--name=vm2 \
--vcpus=1 \
--memory=2048 \
--cdrom=/home/nghia/Downloads/ubuntu-20.04.6-live-server-amd64.iso \
--disk size=8

Starting install...
Allocating 'vm1.qcow2'                                      |    0 B  00:00 ... 
Creating domain...                                          |    0 B  00:00     
Running graphical console command: virt-viewer --connect qemu:///system --wait vm1
```

```console
nghia@VTS-NGHIANV24:~$ sudo virsh list --all

 Id   Name   State
----------------------
 1    vm1    running
 ```
 
Cấu hình lại máy ảo này với 2GB ram tối đa và 1GB tức thời (tức hiện tại
bong bóng sẽ chiếm 1GB):

> **Cách 1:**
>
> ```console
> $ virsh edit vm1
> ```
>
> sửa
>
> ```xml
>   <memory unit='KiB'>2097152</memory>
>   <currentMemory unit='KiB'>2097152</currentMemory>
> ```
>
> thành
>
> ```xml
>   <memory unit='GiB'>2</memory>
>   <currentMemory unit='GiB'>1</currentMemory>
> ```
>
> và shutdown sau đó start lại. vì bản config trên chỉ được đọc khi khởi tạo
> máy ảo và shutdown sẽ `destroy` máy ảo rồi cấp phát lại khi start
> 
> ```console
> $ virsh shutdown vm1
> $ virsh start vm1
> ```

> **Cách 2:**
>
> Sử dụng lệnh sau:
>
> ```console
> $ virsh setmaxmem --domain vm1 --size 2G --config
> $ virsh setmem --domain vm1 --size 1G
> ```

> **Lưu ý**
> - Tùy chọn `--config` sẽ chỉ sửa libvir XML, có tác dụng vào lần tới khởi tạo
> - `setmaxmem` chỉnh bộ nhớ cấp phát, chỉ có thể sửa với tùy chọn `--config`


Trạng máy ảo trước:

```console
nghia@VTS-NGHIANV24:~$ virsh dominfo vm1
Id:             8
Name:           vm1
UUID:           7d8a79cf-7c6e-4288-8f31-7729620268f5
OS Type:        hvm
State:          running
CPU(s):         1
CPU time:       16,4s
Max memory:     2097152 KiB
Used memory:    2097152 KiB   // Before
Persistent:     yes
Autostart:      disable
Managed save:   no
Security model: apparmor
Security DOI:   0
Security label: libvirt-7d8a79cf-7c6e-4288-8f31-7729620268f5 (enforcing)
```

```console
vm1@vm1:~$ free -m
               total        used        free      shared  buff/cache   available
Mem:            1963         191        1431           1         340        1627
Swap:              0           0           0
```

và sau khi điều chỉnh:

```console
nghia@VTS-NGHIANV24:~$ virsh dominfo vm1
Id:             8
Name:           vm1
UUID:           7d8a79cf-7c6e-4288-8f31-7729620268f5
OS Type:        hvm
State:          running
CPU(s):         1
CPU time:       18,0s
Max memory:     2097152 KiB
Used memory:    1048576 KiB   // After
Persistent:     yes
Autostart:      disable
Managed save:   no
Security model: apparmor
Security DOI:   0
Security label: libvirt-7d8a79cf-7c6e-4288-8f31-7729620268f5 (enforcing)
```

```console
vm1@vm1:~$ free -m
               total        used        free      shared  buff/cache   available
Mem:             939         165         428           1         346         629
Swap:              0           0           0
```

```console
nghia@VTS-NGHIANV24:~$ virsh dommemstat vm1
actual 1048576
swap_in 0
swap_out 0
major_fault 1117
minor_fault 225880
unused 515280
available 962504
usable 647168
last_update 1689845856
disk_caches 256344
hugetlb_pgalloc 0
hugetlb_pgfail 0
rss 1014220
```

Bây giờ ta thực hiện kịch bản kiểm thử: Trong máy ảo ta chạy một chương
trình cấp phát nhiều hơn số bộ nhớ còn lại khi bong bóng nở nhưng chưa 
chạm đến bộ nhớ tối đa khởi tạo.  
Chương trình bị kill:

```console
vm1@vm1:~$ ./test 
Killed
```

Có vẻ như máy ảo không tự động phát hiện và lấy lại bộ nhớ đã bỏ ra.

- - - 

Chương trình sử dụng để kiểm thử `test.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEMORY_TO_CONSUME 800 * 1024 * 1024 // 800MB

void consume_ram() {
    // Allocate memory
    char *memory = (char *)malloc(MEMORY_TO_CONSUME);

    if (memory != NULL) {
        // Fill the allocated memory with data
        memset(memory, 1, MEMORY_TO_CONSUME);

        // Sleep for 1 minute
        printf("Consuming 800MB RAM...\n");
        sleep(60);

        // Free the allocated memory before exiting
        free(memory);
    } else {
        printf("Memory allocation failed. Exiting...\n");
    }
}

int main() {
    consume_ram();
    printf("Process completed!\n");
    return 0;
}
```

Biên dịch:

```console
$ gcc -o test test.c
```

https://repo.or.cz/linux-2.6/luiz-linux-2.6.git/shortlog/refs/heads/virtio-balloon/pressure-notification/rfc/v1

https://repo.or.cz/qemu/qmp-unstable.git/shortlog/refs/heads/balloon-automatic/handle-all-events/rfc/v1

https://repo.or.cz/qemu/qmp-unstable.git/shortlog/refs/heads/balloon-automatic/rfc/v2