copy custom virtio balloon device vào qemu source:

```bash
cp virtio-balloon.c /path-to-qemu-6.2/hw/virtio/virtio-balloon.c
```

command đoạn code sau trong /path-to-qemu/hw/virtio/virtio-ccw-balloon.c và /path-to-qemu/hw/virtio/virtio-balloon-pci.c:

```c
    object_property_add_alias(obj, "guest-stats", OBJECT(&dev->vdev),
                                  "guest-stats");
    object_property_add_alias(obj, "guest-stats-polling-interval",
                              OBJECT(&dev->vdev),
                              "guest-stats-polling-interval");
```

build and install

```bash
sudo apt install -y libglib2.0-dev libgcrypt20-dev zlib1g-dev autoconf make automake libtool bison flex libpixman-1-dev device-tree-compiler seabios ninja-build
./configure --target-list=x86_64-softmmu --enable-kvm --disable-werror --prefix=/usr
sudo make install -j
```

Chạy:

- Cấp phát disk:

```bash
qemu-img create -f qcow2 /var/lib/libvirt/images/vm1.img 10G
```

- Chạy máy ảo với virtio-balloon device

```bash
kvm \
  -enable-kvm \
  -name vm1 \
  -smp 4 \
  -m 4096 \
  -drive file=/var/lib/libvirt/images/vm1.img \
  -cdrom ~/Downloads/ubuntu-20.04.6-live-server-amd64.iso \
  -boot d \
  -nographic -serial stdio -display none -monitor null \
  -device virtio-balloon
```

reference: https://repo.or.cz/qemu/qmp-unstable.git/commit/7266e87f99b26490269370c853ac2087fe56f18a