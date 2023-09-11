# VirtIO Balloon

## Cài đặt môi trường phát triển Kernel

Mở terminal và cài đặt các gói cần thiết cho việc phát triển thêm driver cho kernel

```bash
sudo apt update
sudo apt install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev
```

## Xây dựng mã nguồn cho driver và nạp vào kẻnal

```bash
cd ~
mkdir virtio_balloon
cd virtio_balloon
touch virtio_balloon.c
touch virtio_balloon.h
touch Makefile
```

Make file có nội dung như sau:

```make
obj-m += virtio_balloon.o

all:
    make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
    make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

Build và thêm vào kernel:

```bash
sudo make
sudo make install
modinfo /lib/modules/$(uname -r)/extra/virtio_balloon.ko
sudo insmod /lib/modules/$(uname -r)/extra/virtio_balloon.ko
```

Kiểm tra xem driver đã được thêm thành công chưa:

```bash
sudo lsmod | grep virtio_balloon
```

```bash
sudo dmesg
```

Gỡ bỏ driver khỏi kernel

```bash
sudo rmmod -f /lib/modules/$(uname -r)/extra/virtio_balloon.ko
```

## Build và cài đặt custom qemu

```bash
sudo apt install -y libglib2.0-dev libgcrypt20-dev zlib1g-dev autoconf make automake libtool bison flex libpixman-1-dev device-tree-compiler seabios ninja-build
./configure --target-list=x86_64-softmmu --enable-kvm --disable-werror --prefix=/usr
sudo make install -j
sudo apt install -y libvirt-daemon-system virtinst
sudo usermod -aG kvm $(whoami)
sudo usermod -aG libvirt $(whoami)
```

```bash
virt-install \
  --name=host \
  --vcpus=4 \
  --ram=4096 \
  --disk path=./vm1.img,size=8 \
  --location=./ubuntu-20.04.6-live-server-amd64.iso \
  --extra-args="console=ttyS0 textmode=1" \
  --graphics none \
  --host-device=pci_0000_05_00_0

kvm \
  -enable-kvm \
  -name vm1 \
  -smp 2 \
  -m 2048 \
  -drive file=./vm1.img \
  -kernel ./linux \
  -initrd ./initrd.gz \
  -cdrom ./ubuntu-20.04.6-live-server-amd64.iso \
  -boot d \
  -append "console=ttyS0 textmode=1" \
  -device virtio-balloon
```
