Cài đặt thư viện, build:

```bash
sudo apt install -y libvirt-dev
gcc -o balloon balloon.c -lvirt
```

Chạy:

```bash
sudo ./balloon
```