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
make
sudo insmod my_driver.ko
```

Kiểm tra xem driver đã được thêm thành công chưa:

```bash
dmesg
```

Gỡ bỏ driver khỏi kernel

```bash
sudo rmmod my_driver
```