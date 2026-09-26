#!/usr/bin/env python3
"""Sinh một dòng `username:argon2hash` cho FileAuthPlugin của amqtt.

- hash là argon2id format PHC ($argon2id$v=19$...) — FileAuthPlugin của amqtt
  >= 0.12 xác minh argon2/bcrypt qua pwdlib; sha512-crypt đã deprecated
  (Python 3.13 bỏ module `crypt`);
- password đọc từ STDIN, KHÔNG truyền qua argv → không lộ vào process list/log.

Chạy trong container amqtt (có sẵn argon2-cffi) hoặc môi trường host có
argon2-cffi. Script bọc shell scripts/amqtt-setup.sh gọi helper này.

Ví dụ:
    printf '%s\n' "$MQTT_PASSWORD" | amqtt-passwd.py "$MQTT_USER" >> "$PASSWD_FILE"
"""

import sys

try:
    from argon2 import PasswordHasher
except ImportError:
    print("amqtt-passwd: thiếu argon2-cffi (pip install argon2-cffi)", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: amqtt-passwd.py USERNAME < password", file=sys.stderr)
        return 2

    username = sys.argv[1]
    if not username:
        print("amqtt-passwd: username không được rỗng", file=sys.stderr)
        return 2
    if ":" in username:
        # File passwd định dạng `username:hash` — username chứa ':' phá vỡ format
        # (FileAuthPlugin split lần đầu gặp ':' maxsplit=1).
        print("amqtt-passwd: username không được chứa ':'", file=sys.stderr)
        return 2

    # readline: lấy đúng một dòng stdin; rstrip chỉ bỏ \n/\r cuối dòng,
    # giữ nguyên mọi ký tự khác của password (kể cả dấu cách đầu/cuối).
    password = sys.stdin.readline().rstrip("\r\n")
    if not password:
        print("amqtt-passwd: password rỗng (truyền qua stdin)", file=sys.stderr)
        return 1

    # Default argon2-cffi: argon2id, time_cost=3, memory_cost=64MiB,
    # parallelism=4, hash_len=32 — đúng format pwdlib.Argon2Hasher của
    # FileAuthPlugin xác minh được.
    print(f"{username}:{PasswordHasher().hash(password)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
