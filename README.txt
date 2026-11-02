VN_LunarCalendar - Lịch âm Việt Nam cho đồng hồ taskbar (Windows 10 và Windows 11)
===============================================================================

LÀ GÌ
  Một file exe duy nhất (C++ thuần, Win32 + GDI). Khi bạn bấm vào đồng hồ ở taskbar, lịch này hiện ra
  thay cho khay lịch của Windows: mỗi ô có số dương lịch và ngày âm lịch nhỏ bên dưới (10/9, 1/6n = tháng nhuận).
  Không nhúng DLL, không hook chuột, không Windhawk/WinRT/MinHook, không thư viện C++ nào.
  Chỉ dùng thiết lập hệ thống: định dạng ngày giờ, tên tháng/thứ, ngày đầu tuần, giao diện sáng/tối, màu accent.

CÁCH DÙNG
  - Bấm đồng hồ taskbar: mở lịch. Bấm lại (hoặc bấm ra ngoài, Esc): đóng.
  - Muốn dùng lịch gốc của Windows: bỏ tick "Thay lịch Windows khi bấm đồng hồ" trong menu chuột phải của icon khay.
    Lịch gốc cũng vẫn mở bình thường khi bạn gọi bằng phím tắt (Win+Alt+D, Win+N) vì lúc đó con trỏ không nằm trên đồng hồ.
  - Ctrl+Alt+L hoặc click icon khay (rê chuột vào icon để xem ngày âm hôm nay): mở lịch.
  - Lăn chuột / mũi tên / PageUp / PageDown: đổi tháng. Home: về hôm nay.
  - Rê chuột vào một ngày: dòng thông tin phía dưới hiện ngày âm, Can Chi ngày/tháng/năm và tên ngày lễ.
  - Màu đỏ = ngày lễ âm lịch bạn chọn (Tết mùng 1-4 và Giỗ Tổ đỏ cả số dương; các ngày khác chỉ nhãn âm).

THÔNG TIN SẢN PHẨM (nhúng trong exe)
  Icon: vn_lunar.ico (exe, cửa sổ và icon khay). Tác giả/Công ty: bbkim. Sản phẩm: VN Lunar Calendar, phiên bản 1.0.0.0.
  Xem bằng chuột phải exe > Properties > Details, hoặc icon khay > "Giới thiệu".
  Muốn sửa chữ/phiên bản: mở resource.rc. Muốn đổi icon: thay file vn_lunar.ico (giữ tên). Rồi chạy lại build.bat.
  app.manifest: khai báo hỗ trợ Windows 7-11, chạy quyền thường (không hỏi UAC), DPI từng màn hình.

BUILD
  Cần g++ của MSYS2 (có kèm windres):  pacman -S mingw-w64-ucrt-x86_64-gcc   rồi chạy build.bat.
  Lỗi build: gửi nội dung build_error.log.

TÀI NGUYÊN
  - Khi rảnh: 1 cửa sổ ẩn + 1 WinEvent hook chỉ chạy khi cửa sổ foreground đổi (không chạy theo chuột).
  - 1 timer duy nhất, kêu 1 lần mỗi nửa đêm. Timer 1 giây chỉ chạy khi lịch đang mở.
  - Font và bộ đệm vẽ chỉ được tạo khi lịch mở, giải phóng khi đóng; RAM được thu gọn (working set) mỗi lần đóng.

XUNG ĐỘT VÀ GIỚI HẠN
  - Phát hiện lịch Windows: khi cửa sổ foreground đổi sang flyout của ShellExperienceHost/ShellHost VÀ con trỏ
    đang nằm trên đồng hồ. Vì vậy lịch gốc có thể nháy một thoáng (vài chục ms) trước khi lịch này hiện.
  - Windows 11: bấm đồng hồ cũng mở Trung tâm thông báo. Khi bật "Thay lịch Windows", lịch này thay luôn nó
    (thông báo vẫn mở được bằng Win+N hoặc tắt tùy chọn). Vùng đồng hồ được ước lượng là ~130 DIP bên phải taskbar.
  - Ctrl+Alt+L có thể trùng phím tắt của phần mềm khác (card đồ họa, bộ gõ...): nếu bị trùng thì phím này không hoạt
    động, các cách mở khác vẫn dùng được.
  - Không xung đột với Windhawk hay các mod khác vì không chèn gì vào tiến trình khác.
  - Âm lịch tính theo múi giờ UTC+7 (UTC+8 cho năm trước 1968), đã kiểm tra các ngày Tết 1968-2028, tháng nhuận
    và tính nhất quán 1950-2150.
