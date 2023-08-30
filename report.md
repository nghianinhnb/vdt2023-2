## Công việc đã hoàn thành

- Cài đặt và chạy thành công kvm
- Giải quyết được vấn đề hot-plug ram trên kvm
- Thực hiện được nested virtualization trên kvm
- Custom QEMU 1.7.50 hỗ trợ automatic balloon tuy nhiên cài đặt kernel
chưa thành công do phiên bản quá cũ đã không còn hỗ trợ.

## Công việc tiếp theo

- [ ] Xây dựng `virtIO balloon` driver. Là một module cung cấp các lời gọi hệ
thống thực hiện theo dõi tải, tăng giảm bong bóng bộ nhớ, lấy các thông tin
bộ nhớ phục vụ monitoring:

  - [ ] Tìm hiểu và xây dựng chức năng theo dõi tải máy khách

  - [ ] Tìm hiểu và xây dựng chức năng tăng giảm bong bóng bộ nhớ

  - [ ] Tìm hiểu và xây dựng chức năng cung cấp metric phục vụ monitoring

- [ ] Custom QEMU điều khiển máy chủ và máy khách gọi các lời gọi hệ thống
trên, thực hiện chức năng automatic ballooning, trả metric phục vụ monitoring

  - [ ] Tìm hiểu và xây dựng chức năng truyền nhận thông điệp báo tải máy khách,
  tăng giảm bong bóng bộ nhớ

  - [ ] Xây dựng các thủ tục tương ứng với từng thông điệp

  - [ ] Xây dựng các cli bật tắt, tùy chỉnh auto ballooning, lấy metric

- [ ] Benchmark khi chạy automatic balloon so với bình thường

- [ ] Xây dựng giải pháp monitoring

- [ ] Demo
