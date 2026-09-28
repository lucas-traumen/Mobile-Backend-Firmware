# Smart Home — theo dõi nhiệt độ, độ ẩm ngay trên điện thoại

Hệ thống gồm ba phần cùng hoạt động trong mạng Wi-Fi nhà bạn:

- **Thiết bị đo** — board ESP32 gắn cảm biến nhiệt độ/độ ẩm, đặt trong từng phòng.
- **Server** — một máy tính/máy chủ nhỏ trong nhà, nhận số đo từ các board và lưu lại.
- **App điện thoại** — xem số đo theo thời gian thực, xem lịch sử, bật/tắt công tắc (relay) gắn với board.

```
Board ESP32 (cảm biến)  →  Server (nhận + lưu dữ liệu)  →  App điện thoại (xem + điều khiển)

              Cả ba thiết bị phải cùng một mạng Wi-Fi / LAN trong nhà
```

Bạn không cần biết Docker, MQTT hay InfluxDB: server cài bằng **một file script duy nhất**, app tự tìm thấy server trong mạng. Người phát triển muốn xem bên trong hệ thống: đọc [tài liệu riêng](docs/developer-guide.md) ở cuối trang.

## Bắt đầu nhanh

```bash
# 1. Trên server (x86-64, cùng Wi-Fi với điện thoại) — cài mọi thứ, tự sinh mật khẩu:
bash smarthome-deploy.sh init

# 2. Kiểm tra mọi thứ đã chạy:
bash smarthome-deploy.sh status

# 3. Hiện mã QR cho app (chứa mật khẩu — cẩn thận khi hiển thị):
bash smarthome-deploy.sh credentials-qr

# 4. Trên điện thoại: nối cùng Wi-Fi, mở app, quét QR ở bước 3.
```

Server phải cài Docker trước khi chạy bước 1 — chi tiết từng bước ở [Cài server lần đầu](#cài-server-lần-đầu) và [Kết nối ứng dụng điện thoại](#kết-nối-ứng-dụng-điện-thoại). Server đã cài sẵn? Xem [Dùng hằng ngày](#dùng-hằng-ngày).

## Bạn cần chuẩn bị gì

- **Máy server**: chạy Linux (Ubuntu Server/Debian là dễ nhất), CPU **x86-64** (loại thông thường của PC/server). Raspberry Pi và các máy **ARM chưa dùng được** — phần mềm server hiện chỉ đóng gói cho x86-64, cài lên ARM sẽ lỗi ngay khi tải về.
- **Mạng**: server, điện thoại và các board nối **cùng một mạng** (LAN/Wi-Fi). Server không được bật chế độ ngủ (sleep) — ngủ là "biến mất" với cả mạng.
- **Trên server**: Docker Engine + Docker Compose v2 cùng vài công cụ nhỏ (hướng dẫn cài ở bước 1 bên dưới).
- **Board ESP32** đã có firmware Smart Home do người kỹ thuật nạp. Tự làm board mới từ đầu cần công cụ chuyên dụng — xem [tài liệu cho người phát triển](docs/developer-guide.md).
- **Điện thoại** đã cài app Smart Home.

## Cài server lần đầu

### Bước 1 — Cài Docker và các công cụ nhỏ

SSH vào server rồi dán lần lượt (**Ubuntu** — theo hướng dẫn chính thức của Docker):

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
  | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo usermod -aG docker "$USER"   # đăng xuất/đăng nhập lại sau lệnh này

# Các công cụ nhỏ còn lại:
sudo apt-get install -y openssl python3 avahi-daemon avahi-utils qrencode
```

Dùng **Debian**? Block trên dành cho Ubuntu — làm theo [hướng dẫn Docker chính thức cho Debian](https://docs.docker.com/engine/install/debian/) (tương tự, nhưng repo apt là `linux/debian` thay vì `linux/ubuntu`).

Thiếu `docker compose`, `openssl` hoặc `python3` thì cài đặt **dừng ngay từ đầu**; thiếu `avahi-daemon` thì dừng ở bước bật dịch vụ dò tìm — script đều in đúng lệnh cần cài. Chỉ thiếu `avahi-utils` hoặc `qrencode` thì hệ thống vẫn chạy — mất phần kiểm tra dò tìm hoặc mã QR.

Trên server thật **không chạy `docker compose build`** — mọi thứ đã được đóng gói sẵn, script chỉ tải về và khởi động.

### Bước 2 — Đưa file cài đặt vào server

Chỉ cần đúng một file `smarthome-deploy.sh` — không cần tải cả repo. Lấy file từ nguồn đáng tin cậy (bản phát hành chính thức khi có, hoặc do người quản trị cung cấp) rồi chọn một trong hai cách:

```bash
# Cách A — chép từ máy tính của bạn sang server (thay tên đăng nhập + địa chỉ server):
scp smarthome-deploy.sh <tên-đăng-nhập>@<địa-chỉ-server>:

# Cách B — tải trực tiếp trên server từ nơi phát hành chính thức (nếu đã có):
curl -fsSL <URL_BẢN_PHÁT_HÀNH>/smarthome-deploy.sh -o smarthome-deploy.sh
```

Khi nơi phát hành công bố mã kiểm tra (checksum), nên đối chiếu trước khi chạy: `sha256sum smarthome-deploy.sh`.

### Bước 3 — Chạy cài đặt

```bash
bash smarthome-deploy.sh init
```

Script tự làm toàn bộ: kiểm tra môi trường, tạo thư mục `~/smarthome` chứa các file cần chạy, **tự sinh mật khẩu an toàn** (không bao giờ hiện ra màn hình), tải ba phần mềm đã đóng gói sẵn về rồi khởi động. Có thể hỏi mật khẩu `sudo` một lần (để bật dịch vụ dò tìm tự động). Lần đầu tải về mất vài phút là bình thường.

Chạy lại `init` vô hại: mật khẩu và dữ liệu không bao giờ bị ghi đè.

### Bước 4 — Kiểm tra server đã chạy

```bash
bash smarthome-deploy.sh status
```

Ba dịch vụ phải chạy: `amqtt` và `influxdb` ở trạng thái `Up (healthy)`, còn `backend` chỉ cần `Up` (service này không có kiểm tra sức khỏe). Phần cuối của kết quả là kiểm tra "server có tự quảng bá trong mạng không" (`_smarthome._tcp`) — app dựa vào đó tự tìm thấy server mà không cần gõ địa chỉ.

Server khởi động lại (reboot, mất điện) thì hệ thống **tự chạy lại**, không cần gõ lệnh nào.

## Kết nối ứng dụng điện thoại

1. Điện thoại nối **cùng Wi-Fi** với server.
2. Trên server, hiện thông tin đăng nhập cho app dạng mã QR:

   ```bash
   bash smarthome-deploy.sh credentials-qr
   ```

   Mã này chứa **mật khẩu của app** — coi như chìa khóa nhà: chỉ hiện ra lúc ghép nối, không chụp màn hình hay gửi đi đâu.    Mã còn kèm sẵn phần không nhạy cảm (địa chỉ máy chủ, MQTT WebSocket port, org/bucket InfluxDB — khi server nhận ra địa chỉ LAN của mình) để bản app mới tự điền hết sau khi quét; bản app hiện tại chỉ dùng phần mật khẩu, phần còn lại tự bỏ qua.

3. Mở app trên điện thoại → quét mã QR ở bước 2 (hoặc dùng dò tìm tự động — app tự thấy server trong mạng).
4. Xong. Các board đang hoạt động sẽ hiện dưới dạng thẻ trong app.

Không quét được QR (terminal không hỗ trợ)? Chạy `bash smarthome-deploy.sh pairing-code` — in khối thông tin dạng chữ kèm QR, có thể nhập tay vào app. Khối này cũng chứa mật khẩu, đừng chia sẻ.

**Token chỉ-đọc (tùy chọn).** App đọc lịch sử trực tiếp từ nơi lưu dữ liệu bằng một token chỉ cho **đọc**, không cho sửa. Hai lệnh, chạy trên server:

```bash
cd ~/smarthome

# 1) Tạo user "app-mobile" để gắn token (chạy lại báo "already exists" — vô hại):
docker exec "$(docker compose ps -q influxdb)" influx user create \
  --name app-mobile --org smarthome

# 2) Tạo token chỉ-đọc (in token ra màn hình):
docker exec "$(docker compose ps -q influxdb)" sh -c \
  'influx auth create --user app-mobile --read-bucket "$(influx bucket list --name telemetry --hide-headers | cut -f1)" --description "mobile app read-only"'
```

Cảnh báo "initial password not set" ở lệnh 1 là bình thường — user này chỉ để gắn token, không đăng nhập. Chép token in ra ở lệnh 2, mở `~/smarthome/.env` và thêm dòng `INFLUX_APP_TOKEN=<token-vừa-chép>`, rồi chạy lại `bash smarthome-deploy.sh credentials-qr` — từ đó mã QR cho app kèm luôn token. Chỉ đổi chữ `smarthome` (org) và `telemetry` (bucket) trong lệnh nếu khi cài bạn đã đặt tên khác mặc định.

## Thêm board (thiết bị đo) mới

1. Board phải đã có firmware Smart Home (việc nạp firmware cần công cụ chuyên dụng — nhờ người kỹ thuật, xem [developer guide](docs/developer-guide.md)).
2. Mỗi board có một mã định danh riêng (`boardId`) sinh từ địa chỉ phần cứng (MAC) in trong log khi board khởi động. Tạo nhãn QR cho board:

   ```bash
   bash smarthome-deploy.sh board-qr -m 5c:01:3b:6b:af:6c      # theo MAC
   bash smarthome-deploy.sh board-qr --board-id 3b6baf6c        # hoặc theo boardId đã biết
   ```

   In/ghi nhãn QR này dán lên vỏ board — quét là khỏi nhớ mã.

3. Trong app: quét QR board → app nối **Bluetooth** với board (tên Bluetooth dạng `IoTBoard-xxxxxxxx`) → nhập tên Wi-Fi + mật khẩu nhà → app đẩy cấu hình sang board.
4. Sau khoảng nửa phút board vào mạng và tự hiện thành thẻ mới trong app.

**Board cần làm lại từ đầu?** Board chưa từng cấu hình tự vào chế độ Bluetooth khi bật nguồn. Board đã cấu hình mà cần làm lại (đổi Wi-Fi, sai mật khẩu): **giữ nút BOOT trên board ≥ 5 giây** khi board đang chạy — board xóa cấu hình cũ rồi tự quay lại chế độ cấu hình.

Khi cấu hình, giữ điện thoại **gần board** (Bluetooth chỉ bắt được ở tầm ngắn). QR board chỉ chứa định danh thiết bị, không chứa mật khẩu, nhưng cũng không cần thiết chia sẻ ra ngoài.

## Dùng hằng ngày

| Việc cần làm | Lệnh trên server |
|---|---|
| Kiểm tra mọi thứ còn chạy tốt | `bash smarthome-deploy.sh status` |
| Xem 100 dòng log gần nhất | `bash smarthome-deploy.sh logs` |
| Xem log liên tục (thoát: Ctrl-C) | `bash smarthome-deploy.sh logs -f` |
| Cập nhật lên phiên bản mới | `bash smarthome-deploy.sh update` |
| Tạm dừng hệ thống | `bash smarthome-deploy.sh stop` |
| Chạy lại sau khi dừng | `bash smarthome-deploy.sh start` |
| Mở menu chọn việc bằng phím | `bash smarthome-deploy.sh` (không gõ lệnh gì) |

- `update` chỉ thay phiên bản phần mềm: dữ liệu đo được và mật khẩu **giữ nguyên**.
- Server reboot/mất điện: hệ thống tự sống lại, kiểm tra bằng `status`.
- Trước một nâng cấp lớn (đổi đời database…), nên sao lưu dữ liệu trước — nhờ người kỹ thuật làm theo phần [Sao lưu và phục hồi dữ liệu](docs/developer-guide.md#sao-lưu-và-phục-hồi-dữ-liệu).

## Xử lý lỗi thường gặp

| Hiện tượng | Nguyên nhân thường gặp | Cách xử lý |
|---|---|---|
| App không thấy server | Điện thoại và server khác Wi-Fi; server đang ngủ | Nối cùng Wi-Fi; tắt ngủ server; chạy `status` kiểm tra |
| Wi-Fi chặn dò tìm tự động (Wi-Fi khách sạn/văn phòng) | Mạng kiểu này chặn các thiết bị thấy nhau | Dùng mạng riêng của nhà, hoặc nhập địa chỉ server thủ công trong app |
| Container chưa `Up (healthy)` ngay sau khi cài | Lần đầu khởi động chậm (tải về, sắp xếp dữ liệu lần đầu) | Chờ 1–2 phút rồi chạy `status` lại; xem chi tiết `bash smarthome-deploy.sh logs -f` |
| Cài/cập nhật lỗi tải về (platform, architecture) | Server là ARM/Raspberry Pi, hoặc kho phần mềm đã chuyển chế độ riêng tư | Chỉ dùng máy x86-64; lỗi "denied/401" thì nhờ người kỹ thuật đăng nhập kho (`docker login`) rồi chạy lại |
| `init` dừng ngay ở đầu | Thiếu Docker/compose/openssl/python3 | Script in đúng lệnh cần cài — cài xong chạy lại `init` |
| `credentials-qr` báo `MQTT_APP_PASSWORD` trống | `.env` thiếu/trống giá trị này (file từ phiên bản cũ, hoặc bị xóa/sửa tay) — broker đã tự sinh mật khẩu random và in ra log | Lấy lại từ log broker theo ghi chú dưới bảng, điền vào `.env` rồi chạy lại `credentials-qr` |
| Board không hiện trong app | Board chưa được cấu hình, hoặc cấu hình cũ không còn dùng được | Chưa cấu hình: quét QR board và làm theo app. Đã cấu hình mà lỗi: giữ nút BOOT ≥ 5 giây để làm lại |
| Có địa chỉ server mà app vẫn không nối được | Tường lửa trên server chặn | Mở 3 cổng: `sudo ufw allow 1883/tcp`, `sudo ufw allow 9001/tcp`, `sudo ufw allow 8086/tcp` |

**Lấy lại mật khẩu app từ log broker** (khi `credentials-qr` báo `MQTT_APP_PASSWORD` trống — chạy trong `~/smarthome`):

```bash
docker compose logs amqtt | grep -A1 "MQTT_APP_PASSWORD trống"
```

Dòng thứ hai của kết quả là mật khẩu (`amqtt-setup:     <mật khẩu>`). Điền vào `~/smarthome/.env` (dòng `MQTT_APP_PASSWORD=...`) rồi chạy lại `bash smarthome-deploy.sh credentials-qr`.

## An toàn dữ liệu

- **Thư mục `~/smarthome` trên server là khu nhạy cảm.** File `.env` trong đó là "chìa khóa" của cả hệ thống (mật khẩu, token) — không sao chép đi nơi khác, không gửi qua chat/email, không dán vào ảnh hay commit lên Git.
- **Mã QR credentials và mã ghép nối hiển thị mật khẩu/token nguyên văn** — chỉ hiện khi đang ghép nối app/thiết bị, xong là đóng cửa sổ terminal.
- **Token cho app luôn là token chỉ-đọc.** Đừng bao giờ dùng token quản trị cho app.
- **Trên server thật không chạy `docker compose build`** (đó là việc của người phát triển) và **tuyệt đối không chạy `docker compose down -v`** — `-v` xóa sạch dữ liệu đã đo. Chỉ dùng các lệnh qua script ở trên.
- Dữ liệu đo nằm trong "ổ dữ liệu" riêng của Docker; `update` thường không đụng tới. Dữ liệu quan trọng thì sao lưu định kỳ — xem [developer guide](docs/developer-guide.md).

## Tài liệu cho người phát triển

Bạn muốn xem cách hệ thống hoạt động bên trong, tự build từ mã nguồn, nạp firmware cho board, hay truy vấn dữ liệu trực tiếp? Xem:

- **[docs/developer-guide.md](docs/developer-guide.md)** — kiến trúc, build từ source, firmware ESP32, contract dữ liệu, truy vấn, vận hành InfluxDB, sao lưu.
- [docs/frontend-setup.md](docs/frontend-setup.md) — cấu hình kết nối cho app/web frontend.
