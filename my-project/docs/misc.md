Nó là device hay driver? là driver

Nó gồm những thành phần gì?
gồm các queue để đọc ghi bởi device aka giao tiếp với device, 
gồm các queue đợi ack bởi host aka giao tiếp với host,
1 thread chạy tác vụ ballooning, 
list các mem page đã đưa cho host,
list balloon pfn đã đưa cho host

Máy nào sẽ handle pressure?
máy khách đương nhiên phải báo prs để máy chủ trả lại vậy máy khách nên theo dõi prs
đằng nào cũng phải monitoring vậy để máy khách phát message khi thu thập stat:
- nếu thừa ram gom lại và đưa máy chủ:
    - cần cơ chế báo nhận ram đã đưa: đặt lock và đợi ack. lần thu thập tiếp theo kiểm tra lock
    trước khi gom và gửi
    - nếu chạm ngưỡng trong lúc gom mà chưa ack có nên trả lại luôn? không khả thi vì đã báo rồi,
    muốn làm thế đồng bộ lại phức tạp tương đương xin lại ngay lượt sau. mà lượng đưa mỗi lần cũng nhỏ.
- nếu chạm ngưỡng thì không báo gì
- nếu vượt ngưỡng thì báo lấy lại ram. lượng lấy lại trong 1 lần có thể gấp nhiều lần lượng đưa.
việc thu thập stat có thể sẽ thưa mà lấy lại ram thì cần gấp. vậy lấy ram có thể gọi chủ động không
cần đợi collect stat. sẽ tìm cách chèn lệnh gọi vào virtio err handler.


Nếu máy chủ không phản hồi hoặc phản hồi quá chậm do tải khi cần lấy lại ram hoặc khi đang gửi đợi ack
thì sao?
với không phản hồi khi hoặc phản hồi chậm khi đang gửi thì sẽ gửi ram kèm thời gian gửi. máy chủ quá timeout
không nhận nữa. máy khách sau timeout bỏ lock nhả ram ra đợt lượt collect stat tiếp theo. máy chủ và máy khách
có chung đồng hồ k? có vẻ là chung đồng hồ
còn trường hợp không phản hồi hoặc phản hồi chậm khi cần lấy ram thì gửi warning, xử lý lỗi như lỗi thiếu
ram. nếu kéo dài, xử lý thủ công.
Driver rất nhẹ nên các trường hợp trên chỉ để dự trù chứ khó xảy ra

Trường hợp ram lấy ra đem ra dùng hết nên không trả lại được?
khi xây dựng xong phương án monitoring điều này sẽ thể hiện rõ ràng trong lượng ram còn lại trên máy chủ
nếu lượng này quá thấp sẽ được sử lý như tình trạng cloud thiếu ram. sẽ do bên khác xử lý.
có lẽ máy chủ mà đang quá thiếu ram cũng cần dừng việc lấy ram để đảm bảo các máy ảo đã có hoạt động
tốt. vậy lúc ack sẽ báo cả tình trạng máy chủ hoặc thậm chí thêm thao tác hỏi tình trạng máy chủ trước khi gom.
bởi việc gom ram cho máy chủ không cần ưu tiên, hơn nữa là không bắt buộc. thời gian cho mỗi lần gom hoàn
toàn cho phép tính bằng chục phút.


Có cần chú ý đến việc phân mảnh trên ram?
có thể chống phân mảnh trên đống ram được nhận để khi trả lại trả các vùng lớn nhất có thể

Máy chủ lấy stats thế nào?
khi máy chủ nhận được thông tin stats thì ghi ra stdout, việc còn lại để các component
khác thu thập và xử lý

[ ] Soát lại 1 lần nữa vb.c
[ ] Sửa pci những chỗ dùng virtio config
[ ] Build lại virtio driver xem nhận chưa
[ ] Cài quemu mới xem lỗi không
[ ] Build máy ảo lên để test