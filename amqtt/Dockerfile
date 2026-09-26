# amqtt 0.12.1 + dependencies cho SessionDBPlugin (persistence sqlite).
#
# Nguồn image: dùng image chính thức `amqtt/amqtt:0.12.1` (tag 0.12.x ổn định
# mới nhất trên DockerHub, pushed 2026-09-16) làm base thay vì build lại từ
# python-slim — giữ đúng version amqtt đã pin và tận dụng argon2-cffi, pwdlib,
# websockets đã có sẵn trong image. CLI `amqtt_pub`/`amqtt_sub` đi kèm package
# amqtt, có sẵn trong image (scripts/clear-legacy-retained.sh + ví dụ docs dùng).
#
# Lý do cần lớp này: image chính thức KHÔNG bundle sqlalchemy/aiosqlite/greenlet
# (chỉ cài khi `pip install amqtt[contrib]`) — `amqtt.contrib.persistence.
# SessionDBPlugin` import sqlalchemy lúc module load → broker fail khi khởi động
# nếu thiếu. Giới hạn đúng 3 gói của extra `contrib` cần cho sqlite persistence
# (theo Requires-Dist của amqtt-0.12.1.dist-info), không cài cả extra.
FROM amqtt/amqtt:0.12.1

RUN pip install --no-cache-dir \
    "sqlalchemy[asyncio]>=2.0.41,<3" \
    "aiosqlite>=0.21.0" \
    "greenlet>=3.2.3"

# Entrypoint/Cmd giữ nguyên của image (Cmd: amqtt -c /app/conf/broker.yaml);
# docker-compose.yml override command (sinh passwd rồi `exec amqtt`) và đặt
# `stop_signal: SIGINT` — amqtt chỉ graceful-shutdown trên SIGINT, giải thích
# đầy đủ trong comment của docker-compose.yml.
