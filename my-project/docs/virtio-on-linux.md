# VirtIO on Linux

VirtIO devices là một lớp ảo hóa đưuọc tạo ra bởi hypervisor cung cấp cho các máy khách.
Các máy khách giao tiếp với các thiết bị này như với thiết bị vào ra vật lý bằng các giao
thức tương ứng (PCI, MMIO, CCW,... do virtio cung cấp) như các thiết bị vật lý bình 
thường. 

Giao tiếp giữa driver ở máy khách và các device trên được thực hiện thông qua bộ nhớ 
dùng chung sử dụng cấu trúc dữ liệu đặc biệt gọi là `'virtqueue'` thực chất là một bộ 
đệm vòng mà mỗi phần tử của nó là một linked-list các `'vring_desc'` - là struct mô tả 
một vùng nhớ/vùng đệm.

[writing_virtio_drivers](https://docs.kernel.org/driver-api/virtio/writing_virtio_drivers.html) 

## <span style="color: #0070B3">struct</span> vring_desc

Virtio ring descriptors, 16 bytes long. These can chain together via next.

**Definition:**

```c
struct vring_desc {
    __virtio64 addr;
    __virtio32 len;
    __virtio16 flags;
    __virtio16 next;
};
```

**Members:**

```
addr: 
    buffer address (guest-physical)

len: 
    buffer length

flags: 
    descriptor flags

next: 
    index of the next descriptor in the chain, if the VRING_DESC_F_NEXT 
    flag is set. We chain unused descriptors via this, too.
```

All the buffers the descriptors point to are allocated by the guest and 
used by the host either for reading or for writing but not for both.

## <span style="color: #0070B3">struct</span> virtqueue

a queue to register buffers for sending or receiving.

**Definition:**

```c
struct virtqueue {
    struct list_head list;
    void (*callback)(struct virtqueue *vq);
    const char *name;
    struct virtio_device *vdev;
    unsigned int index;
    unsigned int num_free;
    unsigned int num_max;
    bool reset;
    void *priv;
};
```

**Members:**
```
list:
    the chain of virtqueues for this device

callback:
    the function to call when buffers are consumed (can be NULL).

name:
    the name of this virtqueue (mainly for debugging)

vdev:
    the virtio device this queue was created for.

index:
    the zero-based ordinal number for this queue.

num_free:
    number of elements we expect to be able to fit.

num_max:
    the maximum number of elements supported by the device.

reset:
    vq is in reset state or not.

priv:
    a pointer for the virtqueue implementation to use.
```

**Description:**

A note on num_free: with indirect buffers, each buffer needs one 
element in the queue, otherwise a buffer will need one element 
per sg element.

The callback function pointed by this struct is triggered when 
the device has consumed the buffers provided by the driver. More 
specifically, the trigger will be an interrupt issued by the 
hypervisor (see vring_interrupt()). Interrupt request handlers 
are registered for a virtqueue during the virtqueue setup process 
(transport-specific).
