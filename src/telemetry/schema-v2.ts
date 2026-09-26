import { z } from "zod"

// M14a — telemetry v2 theo kênh (contract board-centric): firmware v2 publish lên
// `{prefix}/boards/{boardId}/telemetry` payload
// `{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}`.
//
// Kênh v2 là generic (S1, S2, S3… do descriptor từng
// board khai báo) và descriptor không mang khoảng giới hạn, nên schema chỉ đảm
// bảo cấu trúc + số finite; ý nghĩa từng kênh do descriptor quyết định lúc map
// (src/boards/boards-ingest.ts).
export const telemetryV2Schema = z.object({
  schemaVersion: z.literal(2),
  // Anchor đầu là đủ theo spec M14: segment topic MQTT không chứa "/" nên phần
  // đuôi vượt class ký tự vẫn là boardId hợp lệ về thực dụng (topic↔payload được
  // đối chiếu riêng ở validate-v2).
  boardId: z.string().regex(/^[a-zA-Z0-9_-]+/),
  values: z.record(z.string(), z.number().finite()),
})

export type TelemetryV2Payload = z.infer<typeof telemetryV2Schema>
