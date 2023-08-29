# Automatic Ballooning

**NOTE: This is an experimental development project from 2013 that was
never completed. The feature described does not exist in any currently
shipping version of QEMU**


## Thiết lập và khởi động sandbox chạy lab

Cài đặt kvm theo hướng dẫn [tại đây](../kvm/kvm.md). Việc này để giả lập một số bài test
ví dụ như: `hot plug ram`, ...

### Kích hoạt nested virtualization:

>**Kiểm tra máy chủ có hỗ trợ nested virtualization không:**
>
>Đối với cpu intel:
>```bash
>cat /sys/module/kvm_intel/parameters/nested
>```
>
>Đối với cpu amd:
>```bash
>cat /sys/module/kvm_amd/parameters/nested
>```
>
>Nếu giá trị trả về là Y hoặc 1, điều đó có nghĩa là nested
>virtualization được hỗ trợ. Nếu giá trị là N hoặc 0, nested
>virtualization không được hỗ trợ.
>
>Nếu chưa, thử chạy lệnh sau, sau đó reboot:
>```bash
>sed -i 's/GRUB_CMDLINE_LINUX=""/GRUB_CMDLINE_LINUX="kvm-intel.>nested=1"/g' /etc/default/grub
>update-grub
>```
>Tương tự với chip amd

>**Kích hoạt nested virtualization:**
>
>Tắt các VM đang chạy và unload kvm_probe module:
>
>```bash
>sudo modprobe -r kvm_intel
>```
>
>Kích hoạt tính năng nested:
>
>```bash
>sudo modprobe kvm_intel nested=1
>```
>
>Để giữ thay đổi này vào lần tới khởi động, thêm dòng sau vào file
>`/etc/modprobe.d/kvm.conf` :
>
>```
>options kvm_intel nested=1
>```
>
>***Tương tự đối với chip amd thay `'kvm_intel'` bằng `'kvm_amd'` trong các lệnh trên***

### Khởi chạy sandbox:

```bash
virt-install \
    --name=host \
    --vcpus=2 \
    --memory=4096 \
    --cdrom=/path-to/ubuntu-14.04.6-server-amd64.iso \
    --disk size=30
```

***Các thao tác bên dưới đây thực hiện trong máy ảo này!***

## Yêu cầu về phiên bản và cấu hình máy sandbox

Kiểm tra kernel phiên bản 3.10 trở lên:

```bash
uname -r

4.4.0-142-generic   # > 3.10.*
```

Kiểm tra để chắc chắn đã kích hoạt CONFIG_CGROUPS và CONFIG_MEMCG:

```bash
grep -E 'CONFIG_CGROUPS=|CONFIG_MEMCG=' /boot/config-$(uname -r)

CONFIG_CGROUPS=y
CONFIG_MEMCG=y
```

## Build và cài đặt QEMU hỗ trợ automatic balloon:

Cài đặt các dependencies dùng để build QEMU 

```bash
sudo apt install -y libglib2.0-dev libgcrypt20-dev zlib1g-dev autoconf make automake libtool bison flex libpixman-1-dev device-tree-compiler seabios
```

Tải source code QEMU và giải nén vào folder `'build'`:

```bash
mkdir -p build
wget --no-check-certificate -O qemu https://repo.or.cz/qemu/qmp-unstable.git/snapshot/e8d6ecee0845ac5e17bb5054850e602a7c67ee21.tar.gz
tar -xzf qemu -C build --strip-components=1
```


### Tùy chỉnh 

Cấu hình buid qemu để ảo hóa kiến trúc x64

```bash
cd build
./configure --target-list=x86_64-softmmu --enable-kvm --disable-werror --prefix=/usr
```

>Có thể dùng lệnh sau nếu muốn xem danh sách các target đưọc hỗ trợ:
>```bash
>./configure --help | awk '/--target-list=LIST/,/^$/'
>```
>Nếu không tùy chỉnh target list thì mặc định sẽ build toàn bộ trong danh
>sách này.

### Cài đặt

Trong folder `'build'`, chạy các lệnh sau để build và cài đặt:

```bash
make
sudo make install
```

Kiểm tra nếu đã cài đặt thành công:

```bash
sudo qemu-system-x86_64 -version
```

```console
QEMU emulator version 1.7.50, Copyright (c) 2003-2008 Fabrice Bellard
```

Sau khi cài đặt thành công, cấp quyền sử dụng kvm module cho group kvm
và thêm user hiện tại vào group này

```bash
sudo setfacl -m g::rw /dev/kvm
sudo chmod g+rw /dev/kvm
sudo chown :kvm /dev/kvm
sudo usermod -aG kvm $(whoami)
```

Tạo file `'/etc/udev/rules.d/01-kvm.rules'` với nội dung dưới đây để giữ
phân quyền khi khởi động lại:

```
KERNEL=="kvm", GROUP="kvm", MODE="0660"
```

## Cài libvirt để hỗ trợ quản lý máy ảo

```bash
sudo apt install -y libvirt-bin virtinst
```

Thêm user hiện tại vào group libvirtd để có thể sử dụng libvirt:

```bash
sudo usermod -aG libvirtd $(whoami)
```

Kiểm tra xem libvirt đã nhận qemu simulator chưa:

```bash
sudo virsh version 
```

```console
Compiled against library: libvirt 1.2.2
Using library: libvirt 1.2.2
Using API: QEMU 1.2.2
Running hypervisor: QEMU 1.7.50
```

### Cài đặt kernel có hỗ trợ auto-balloon

Cài đặt các gói cần thiết

```bash
sudo apt update
sudo apt install -y make gcc bc libncurses5-dev libncursesw5-dev
```

Tải và giải nén kernel source code vào `'/usr/src'`:

```bash
sudo mkdir -p /usr/src/linux-2.6
wget --no-check-certificate -O linux-2.6.tar.gz https://repo.or.cz/linux-2.6/luiz-linux-2.6.git/snapshot/96a1a83759f875185a879cd9963b8183dc0ced57.tar.gz
sudo tar -xvf linux-2.6.tar.gz -C /usr/src/linux-2.6 --strip-components=1
```

Tạo file cấu hình:

```bash
cd /usr/src/linux-2.6
sudo make mrproper
sudo make menuconfig
```

Bỏ bớt các module không dùng đến như ethernet, wifi, các driver,... để giảm thời gian build và tránh lỗi vì đây chỉ là máy ảo nên một số device không được giả lập gây lỗi khi cài Dùng
phím điều hướng chọn `'<Save>'` để ghi ra file `'.config'` sau đó chọn
`'<Exit>'`. Rồi tiến hành biên dịch:

```bash
sudo make
sudo make modules
sudo make modules_install
sudo make install
```


## Tạo môi trường lab

Tải ubuntu server 14.04:

```bash
cd ~ && wget https://releases.ubuntu.com/14.04/ubuntu-14.04.6-server-amd64.iso
```

Vì đều sử dụng bridge theo mặc định nên tên của network bên trong máy
sandbox sẽ trùng với của máy chủ. Do đó cần tạo một network khác:

```bash
sudo nano ./nested-network.xml
```

Với nội dung như sau:

```xml
<network>
  <name>nested-network</name>
  <forward mode="nat">
    <nat>
      <port start="1024" end="65535"/>
    </nat>
  </forward>
  <bridge name="virbr1" stp="on" delay="0"/>
  <mac address="52:54:00:xx:xx:xx"/> <!-- Choose a unique MAC address -->
  <ip address="192.168.123.1" netmask="255.255.255.0">
    <dhcp>
      <range start="192.168.123.2" end="192.168.123.254"/>
    </dhcp>
  </ip>
</network>
```

Tạo, khởi động và kiểm tra:

```bash
sudo virsh net-define ./nested-network.xml
sudo virsh net-autostart nested-network
sudo virsh net-start nested-network
sudo virsh net-list --all
```

```bash
sudo ln -s /usr/share/seabios/bios-256k.bin /usr/share/qemu/bios-256k.bin
```

Tạo máy ảo và tiến hành cài đặt như bình thường:

```bash
sudo virt-install --name=vm1 --vcpus=4 --ram=4096 --disk path=./vm1.img,size=8 --location=./ubuntu-14.04.6-server-amd64.iso --network network=nested-network --extra-args="console=ttyS0 textmode=1 -device virtio-balloon,automatic=true" --graphics none
```

Sử dụng tổ hợp phím `'Ctrl + ]'` để thoát khỏi console của máy ảo.


### Clone máy ảo đã cài đặt

Tới đây đã setup thành công. Tắt máy ảo này đi và clone ra một máy ảo nữa là vm2

```bash
sudo virsh shutdown vm1
sudo virt-clone --original vm1 --name vm2 --file vm2.img
```

Có thể tạo thêm các máy ảo bằng cách tương tự.


## Thực nghiệm

### Tổng quan kết quả đạt được

- Có thể tạo lượng máy ảo có tổng ram lớn hơn ram trên máy chủ
- 


### 1. Tổng ram trên các máy ảo nhiều hơn ram trên máy chủ

Ram trên máy chủ hiện tại là 4GB:

```bash
host@host:~$ free -mh
             total       used       free     shared    buffers     cached
Mem:          3.9G       732M       3.1G       444K        16M       283M
-/+ buffers/cache:       432M       3.4G
Swap:         4.0G         0B       4.0G
```

Cho 2 máy ảo vm1 và vm2 sử dụng 4GB ram và start:

```bash
virsh setmaxmem --domain vm1 --size 4G --config
virsh setmaxmem --domain vm2 --size 4G --config
virsh start vm1
virsh start vm2
```

Kết quả ở 2 máy ảo như dưới đây. Các máy đều có tên vm1 do đều clone từ máy vm1, có thể phân biệt qua ip

```c
vm1@vm1:~$ ip a | grep 'global eth0'
    inet 192.168.123.24/24 brd 192.168.123.255 scope global eth0
vm1@vm1:~$ free -mh
             total       used       free     shared    buffers     cached
Mem:          3.9G       2.1G       1.8G       276K        14M        37M
-/+ buffers/cache:       2.0G       1.8G
Swap:           0B         0B         0B
```

```c
vm1@vm1:~$ ip a | grep 'global eth0'
    inet 192.168.123.132/24 brd 192.168.123.255 scope global eth0
vm1@vm1:~$ free -mh
             total       used       free     shared    buffers     cached
Mem:          3.9G       2.1G       1.8G       272K        14M        37M
-/+ buffers/cache:       2.0G       1.8G
Swap:           0B         0B         0B
```

### 2. Khả năng lấy lại ram của máy ảo

Setup kịch bản

```bash
virsh shutdown vm1
virsh shutdown vm2
virsh setmaxmem --domain vm1 --size 3G --config
virsh setmaxmem --domain vm2 --size 3G --config
virsh setmem --domain vm1 --size 1G --config
virsh setmem --domain vm2 --size 1G --config
virsh start vm1
virsh start vm2
```

vm1 ban đầu với max ram là 3G, ram thực tế là 1G tức 2GB bong bóng, hiện tại đang sử dụng 0.4G ram:

```console
host@host:~$ ./meminfo vm1

actual: 1 GB
available: 2.93448 GB
rss: 0.436237 GB
```

Chạy test cấp phát 1500MB, tức sẽ sử dụng 2G:

```console

```