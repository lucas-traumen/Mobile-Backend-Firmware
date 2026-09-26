# amqtt — MQTT broker của stack (M21b)

Broker MQTT 3.1.1 pure Python asyncio, là service `amqtt` trong
`docker-compose.yml` — port host giữ nguyên như trước: **1883** (TCP) và
**9001** (WebSocket), ESP32/app không đổi cổng. PoC M21a (port phụ
21883/29001) đã gỡ bỏ; config production là `amqtt/broker.yaml`.

## Thành phần

- **Image** (`amqtt/Dockerfile`): `amqtt/amqtt:0.12.1` (image chính thức, tag
  0.12.x ổn định mới nhất trên DockerHub) + 3 gói `sqlalchemy[asyncio]`,
  `aiosqlite`, `greenlet` cho persistence — image gốc không bundle extras
  `amqtt[contrib]`, thiếu là `SessionDBPlugin` fail lúc start. CLI
  `amqtt_pub`/`amqtt_sub` đi kèm package amqtt, có sẵn trong image (scripts và
  ví dụ docs dùng chúng trong container).
- **Config** (`amqtt/broker.yaml`, mount ro vào `/app/conf/broker.yaml`):
  listener `default` TCP `0.0.0.0:1883` + `ws-mobile` WS `0.0.0.0:9001`.
- **Auth** (`FileAuthPlugin`): file `username:hash` argon2id tại
  `/app/data/passwd`, sinh lúc start từ env bởi `scripts/amqtt-setup.sh`
  (hash qua `scripts/amqtt-passwd.py`, password đi qua stdin, không qua argv).
  Hai user dùng chung cả 2 listener: `MQTT_USER` (mặc định `esp32`) cho
  ESP32 + backend, `MQTT_APP_USER` (mặc định `app`) cho app mobile. Không khai
  `AnonymousAuthPlugin` ⇒ cấm anonymous. Đổi password trong `.env` rồi
  `docker compose up -d amqtt` là hiệu lực; `MQTT_APP_PASSWORD` trống → sinh
  random và in ra log container.
- **Persistence** (`SessionDBPlugin`): session + retained lưu sqlite tại
  `/app/data/amqtt.db` trên volume `amqtt-data`, `clear_on_shutdown: false`
  ⇒ retained sống qua restart bất kể client publish với clean_session nào.
- **Signal** (`docker-compose.yml`): amqtt 0.12 chỉ graceful-shutdown trên
  SIGINT — service đặt `stop_signal: SIGINT` để `docker stop/restart/down`
  gửi thẳng đúng signal broker cần; command dùng `exec amqtt` để broker là
  PID 1 với SIGINT ở default (không dùng wrapper `... &` + trap: dash với
  async list luôn set SIGINT=SIG_IGN cho process con và Python tôn trọng
  SIG_IGN kế thừa → KeyboardInterrupt không bao giờ fire — chi tiết trong
  comment của docker-compose.yml).
- **Healthcheck**: python socket tới `127.0.0.1:1883` (image không có `nc`);
  backend `depends_on: amqtt: service_healthy`.

## Lệnh thường dùng

```sh
docker compose up -d --build amqtt   # build + start broker
docker compose logs -f amqtt         # log connect/disconnect (+ password random nếu có)
docker compose restart amqtt         # passwd sinh lại từ .env hiện tại
```

Ví dụ publish/subscribe bằng `amqtt_pub`/`amqtt_sub` trong container:
`README.md` mục 8/9 và `docs/frontend-setup.md` mục 5.

## Điểm cần biết về config 0.12.x (đọc từ source image)

- Schema config là dataclass (`BrokerConfig`); section `plugins` dùng key là
  **đường dẫn class đầy đủ**, value là dict **snake_case** khớp field của
  `Config` dataclass từng plugin (hydrate bằng dacite strict — key kebab-case
  như `password-file` làm broker crash lúc start). Cú pháp `auth: plugins: [...]`
  của hbmqtt/amqtt 0.10 đã deprecated.
- Listener bắt buộc phải có tên `default`; listener khác kế thừa trường chưa
  đặt từ default. WS listener phải đặt cả `type: ws` lẫn `bind`.
- WS client phải offer subprotocol `mqtt` (`Sec-WebSocket-Protocol: mqtt`) —
  broker (websockets 15) trả 400 "missing subprotocol" nếu thiếu. Client MQTT
  chuẩn (mqtt.js/paho theo spec MQTT-over-WS) đều gửi header này.
- Retained lưu global (bảng `stored_messages`) tại publish-time nên sống qua
  kill/restart bất kể clean_session của client publish.
