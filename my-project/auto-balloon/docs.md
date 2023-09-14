Cài đặt thư viện, build:

```bash
sudo apt install libvirt-dev
gcc -o balloon balloon.c -lvirt
```

Chạy:

```bash
sudo ./balloon
```