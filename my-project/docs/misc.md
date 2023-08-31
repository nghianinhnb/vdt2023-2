Nó là device hay driver? là driver

Nó gồm những thành phần gì?
gồm các queue để đọc ghi bởi device aka giao tiếp với device, 
gồm các queue đợi ack bởi host aka giao tiếp với host,
1 thread chạy tác vụ ballooning, 
list các mem page đã đưa cho host,
list balloon pfn đã đưa cho host

Có nên tách ra các utils không? có. tạo header cho nó là đc
