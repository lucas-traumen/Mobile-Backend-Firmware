# AGENTS.md — Mobile_Backend

## Lệnh phát triển

- TypeScript ESM strict, Node ≥ 24, npm. Chạy `npm install` trước khi làm gì.
- Kiểm tra chuẩn: `npm run typecheck` (tsc --noEmit) rồi `npm test` (vitest run). Test 1 file: `npx vitest run src/index.test.ts`.
- Chưa có lint/formatter riêng — tuân theo TypeScript strict và style các file hiện có.

## Cấu trúc

- `src/` — mã backend (đang là stub `health()`; framework HTTP chưa chọn — xem `PLAN.md`).
- `.opencode/agents/` — 4 agent v3; model + permission + prompt nằm trong frontmatter từng file. `opencode.json` chỉ pin `default_agent` và `subagent_depth: 1` — KHÔNG khai báo agent ở hai nơi.
- `.opencode/plugins/orchestrator-state.ts` — plugin checkpoint, ghi `.opencode/state/<run_id>.json`.
- Reasoning effort do provider xkiro quản ở cấu hình global (sol/terra/opus = `max`, glm-5.3-flash không override) — agent không set `reasoningEffort` trong repo này; muốn đổi thì sửa provider config global.

## OpenCode v3 — phân vai

| Agent | Model | Quyền chính |
|---|---|---|
| orchestrator (primary) | `xkiro/openai/gpt-5.6-sol`, fallback `xkiro/anthropic/claude-opus-5` | chỉ dispatch coder/reviewer/tester; bash git read-only được allow, còn lại ask |
| coder (subagent) | `xkiro/z-ai/glm-5.3-flash` | edit allow, task deny |
| reviewer (subagent) | `xkiro/openai/gpt-5.6-terra` | edit/bash deny — chỉ đọc |
| tester (subagent) | `xkiro/openai/gpt-5.6-terra` | edit deny; bash chỉ lệnh test/build được hỏi duyệt |

Lưu ý quyền: `edit: deny` KHÔNG chặn ghi file qua shell (echo >, tee). Reviewer an toàn vì `bash: deny`; tester lệch vào gateway duyệt lệnh — đọc kỹ output lệnh tester trước khi tin "đã sửa gì đó".

## Luật điều phối

1. Đầu phiên: đọc `PLAN.md`, `PROJECT_MEMORY.md`, `.opencode/state/*.json`, `git status`, `git diff`. Chờ người dùng duyệt plan trong `PLAN.md` trước khi giao coder.
2. Mỗi việc có `task_key` ổn định (`M1:coder`, `M1:review`, `M1:test`). Khi gọi Task luôn nhúng marker `[<run_id>/<task_key>]` vào `description`; tiếp tục cùng việc thì truyền `task_id` = `session_id` đã lưu.
3. Chỉ MỘT coder ghi trên worktree tại một thời điểm. Coder xong mới review/test (song song được nếu không đụng artifact).
4. Cùng việc → tiếp tục session cũ; việc khác → session mới. Worker không tự spawn worker (`subagent_depth: 1` chặn sẵn).
5. Prompt giao việc tự chứa: mục tiêu, file được sửa, tiêu chí đạt, lệnh kiểm tra. Worker không thấy hội thoại orchestrator.
6. Kết quả worker bắt buộc: trạng thái, file thay đổi, bằng chứng (lệnh + output), lỗi còn lại, bước tiếp. Quá 3 vòng sửa → `blocked`.
7. Chỉ chốt done sau khi tự chạy lại lệnh kiểm tra. `session.idle` ≠ đạt yêu cầu.
8. Orchestrator chỉ sửa `phase`/`next_action`/task mới trong state file; `session_id`/`call_id`/`history` do plugin quản.

## ID và checkpoint

- `run_id` UUID cho một workflow — giữ nguyên khi resume; plugin tự cấp ở dispatch đầu nếu description thiếu marker.
- `main_session_id` = phiên orchestrator; `session_id` worker = `task_id` của Task tool (một khái niệm); `call_id` chỉ để đối soát lần gọi, không dùng thay session.
- State `.opencode/state/<run_id>.json` (schema v3) là con trỏ tới phiên OpenCode — copy sang máy khác không mang theo hội thoại; không thay thế `PLAN.md`/`PROJECT_MEMORY.md`.
- Plugin capture ID session con qua `session.created`/`message.part.updated` (metadata `sessionId`) ngay khi có, không đợi worker xong; marker nằm cả trong title session con (Task tool đặt title = description).
- Restart: plugin đối chiếu state với sessions; khớp duy nhất → recovered, nhiều ứng viên → `blocked` (không đoán). Run đang chạy ở process khác (lock PID sống) → không đụng.
- Không commit `.opencode/state/` (đã gitignore — chứa ID phiên) và không bao giờ commit secret/`.env`.

## Fallback và resume

- Lỗi provider tạm thời: retry tối đa 2 lần (backoff). Sol vẫn lỗi → resume main với Opus:
  `opencode --session <MAIN_SESSION_ID> --agent orchestrator --model xkiro/anthropic/claude-opus-5`
  Nếu lịch sử không tương thích model: handoff bằng checkpoint (`PLAN.md` + state), tạo main session mới, lưu ID cũ vào history.
- Người dùng hủy / deny quyền / lỗi auth: `blocked`/`cancelled`, không tự retry/fallback để vượt.
- Phiên worker cũ mất hẳn: xác minh phần đã làm (git diff + messages), lưu ID cũ vào history, rồi mới tạo phiên mới kèm checkpoint.
- Resume thủ công: `opencode session list --format json` → `opencode --session <MAIN_SESSION_ID> --agent orchestrator`. Không dùng `--continue` — có thể chọn nhầm phiên khi chạy nhiều workflow.
