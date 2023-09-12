Build và thêm vào kernel:

```bash
sudo make install -j
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

Gỡ bỏ driver khỏi kernel:

```bash
sudo rmmod -f /lib/modules/$(uname -r)/extra/virtio_balloon.ko
```

reference: https://repo.or.cz/linux-2.6/luiz-linux-2.6.git/commit/96a1a83759f875185a879cd9963b8183dc0ced57