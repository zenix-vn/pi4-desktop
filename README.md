# Pi4-Display: màn OSPTEK 10.1" 800×1280 (JD9366) trên Raspberry Pi 4

Driver panel DRM, driver cảm ứng và device-tree overlay cho đúng tấm màn đang dùng với P4-Desktop
(OSPTEK YDP1010BT006-V1, IC JD9366 vừa hiển thị vừa cảm ứng, MIPI-DSI 2 lane).

Dựa trên ví dụ Raspberry Pi 5 của OSPTEK, nhưng mang sang những gì đã kiểm chứng trên ESP32-P4:
- chuỗi khởi tạo lấy từ `P4-Desktop/main/panel_init.h` (chạy thật trên panel này),
- VFP = 140 dòng (TDDI: VFP nhỏ thì cảm ứng trả toàn 0xFF),
- cảm ứng: đọc ID rồi gửi lệnh "backdoor", mỗi lần chỉ đọc ngắn (8 byte), I2C 400 kHz,
  bỏ qua khung hỏng 0xFF, tùy chọn đồng bộ theo chân TE như bản ESP32 đồng bộ theo vsync.

> Chưa chạy thử trên Pi 4 thật. Phần dưới có hướng dẫn kiểm tra và xử lý từng bước.

## 1. Đấu nối

Adapter đi kèm panel có cổng **J2 15 pin, bước 1 mm**, chân giống hệt cổng DISPLAY (DSI) của Pi:

| Chân | J2 adapter | Pi 4 DISPLAY |
|---|---|---|
| 1, 4, 7, 10, 13 | GND | GND |
| 2 / 3 | DSI D1− / D1+ | DSI1 D1− / D1+ |
| 5 / 6 | DSI CLK− / CLK+ | DSI1 CLK− / CLK+ |
| 8 / 9 | DSI D0− / D0+ | DSI1 D0− / D0+ |
| 11 / 12 | I2C SCL / SDA (qua bộ chuyển mức 3V3↔1V8) | SCL0 / SDA0 (GPIO45/44) |
| 14, 15 | VCC 3V3 | 3V3 |

- Dùng cáp FPC 15 pin 1 mm (loại cáp màn 7" chính hãng của Pi), cắm vào cổng **DISPLAY**,
  **không phải cổng CAMERA**.
- Trước khi cắm, dùng đồng hồ kiểm tra: chân 14/15 của Pi phải thông sang chân 14/15 của J2, và
  chân 1 sang chân 1. Nếu cáp đảo mặt (chân 1 ↔ chân 15), 3V3 sẽ chạm GND.
- Nguồn: đèn nền (mạch boost SY7200, LED 80 mA) và nguồn panel đều lấy từ 3V3 của Pi qua cáp,
  ước khoảng 0.3–0.5 A. Dùng nguồn 5 V/3 A chính hãng. **Không** cấp thêm 3V3 vào J4 trong lúc cắm
  Pi, vì J4 nối thẳng vào đường 3V3 của Pi.
- Đèn nền và RESET được kéo lên sẵn trên adapter, nên không cần dây gì thêm.

**Tùy chọn: dây TE** (cảm ứng mượt hơn, ít nhấp nháy hơn): nối J3 chân 4 (TE) → GPIO17 (chân 11
của header 40 pin), rồi thêm `,te` vào dòng overlay. TE là mức 1V8: hầu hết Pi 4 vẫn đọc được,
nếu không thấy ngắt thì cần thêm một con chuyển mức.

## 2. Cài đặt

Chép cả thư mục này sang Pi (Raspberry Pi OS Bookworm 64-bit, kernel 6.6 trở lên), rồi chạy:

```sh
cd Pi4-Display
sudo ./install.sh
sudo reboot
```

Script sẽ cài kernel headers và dtc, build 2 module, chép vào `/lib/modules/$(uname -r)/extra`,
cài `jd9366-pi4.dtbo` và thêm vào `/boot/firmware/config.txt`:

```ini
[all]
display_auto_detect=0
dtoverlay=vc4-kms-v3d
ignore_lcd=1
dtoverlay=jd9366-pi4
```

Module tự nạp nhờ `compatible` trong device tree, không cần sửa `/etc/modules`.
**Sau mỗi lần nâng cấp kernel phải chạy lại `sudo ./install.sh`.**

### Tham số overlay

Viết trên cùng một dòng, ví dụ `dtoverlay=jd9366-pi4,rotation=90,te`:

| Tham số | Ý nghĩa |
|---|---|
| `rotation=90/180/270` | Xoay hình (báo cho DRM/desktop qua "panel orientation") |
| `swapxy`, `invx`, `invy` | Đảo trục cảm ứng khi chạm lệch hướng |
| `te` | Đã nối TE vào GPIO17: chỉ đọc cảm ứng lúc màn đang trong khoảng trống dọc |
| `i2c_hz=100000` | Hạ tốc độ I2C nếu đọc cảm ứng bị lỗi (mặc định 400000) |

### Tham số module

Đặt trong `/etc/modprobe.d/jd9366.conf`:

```
options panel_jd9366 pclk_khz=62500
options jd9366_touch max_points=2
```

- `pclk_khz`: mặc định 76000. Trên Pi 4, xung lane bị làm tròn lên 1000 Mb/s/lane (sát giới hạn của
  DSI). Nếu hình chập chờn, nhiễu hoặc đen, thử `62500`: chạy đúng 750 Mb/s/lane, màn ~49 Hz.
- `max_points`: mặc định 1 điểm chạm. Tăng lên thì có đa điểm nhưng mỗi lần đọc lâu hơn và dễ
  nhấp nháy hơn (nên dùng kèm `te`).

## 3. Kiểm tra

```sh
dmesg | grep -iE 'jd9366|dsi'
#  panel-jd9366 ...: JD9366 800x1280, pclk 76000 kHz
#  jd9366-touch 10-0068: chip cam ung ID 0x9032 da san sang
#  jd9366-touch 10-0068: san sang: hoi vong (khong co TE), 1 diem

kmsprint                        # phải có connector DSI-1, mode 800x1280
sudo apt install evtest && sudo evtest   # chọn "JD9366 Touchscreen", chạm thử
```

| Hiện tượng | Kiểm tra |
|---|---|
| Không có dòng `panel-jd9366` trong dmesg | `ls /boot/firmware/overlays/jd9366-pi4.dtbo`, chạy `sudo vclog -m` / `dtoverlay -l`, xem `lsmod \| grep jd9366` |
| Có DSI-1 nhưng màn đen, đèn nền sáng | Thử `pclk_khz=62500`; kiểm tra cáp (lane D0/D1/CLK) |
| Màn không sáng đèn nền | 3V3 trên J2 chân 14/15, chiều cáp |
| Cảm ứng báo "chua doc duoc ID" | `ls /dev/i2c-10`; cảm ứng chỉ sống khi màn đã hiển thị; thử `i2c_hz=100000` |
| Chạm lệch trục | Dùng `swapxy` / `invx` / `invy`. Trên ESP32 với xoay 90° thì đúng là x = py, y = 799 − px |
| Màn nhấp nháy khi chạm | Nối dây TE và bật `te`; giữ `max_points=1` |

## 4. Xoay ngang 1280×800 cho desktop

Pi OS desktop (labwc): mở Screen Configuration, xoay DSI-1 sang 90°, ở mục Touchscreen chọn
"JD9366 Touchscreen" → DSI-1 để cảm ứng xoay theo. App LVGL chạy thẳng trên DRM (port của
P4-Desktop) thì tự xoay trong app.

## File

| File | Nội dung |
|---|---|
| `panel-jd9366.c` | Driver DRM panel (DSI) |
| `jd9366_init_seq.h` | Chuỗi khởi tạo, sinh từ `P4-Desktop/main/panel_init.h` |
| `jd9366-touch.c` | Driver cảm ứng I2C → input (evdev, đa điểm) |
| `jd9366-pi4-overlay.dts` | Overlay cho Pi 4 (DSI1 + I2C0 mux) |
| `Makefile`, `install.sh` | Build và cài |

Dữ liệu thanh ghi và giao thức cảm ứng: OSPTEK, CC-BY-4.0,
<https://github.com/osptek/display-tft-10.1inch-800x1280>.
