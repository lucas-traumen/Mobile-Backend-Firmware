import { z } from "zod"

// Khoảng vật lý của SHT30/SHT31 (xem PLAN.md — quyết định firmware):
// nhiệt độ −40…125 °C, độ ẩm 0…100 %RH. z.number() loại NaN ở tầng type,
// .finite() loại Infinity/-Infinity trước khi vào min/max.
export const telemetrySchema = z.object({
  schemaVersion: z.literal(1),
  deviceId: z.string().min(1),
  roomId: z.string().min(1),
  temperature: z.number().finite().min(-40).max(125),
  humidity: z.number().finite().min(0).max(100),
})

export type TelemetryPayload = z.infer<typeof telemetrySchema>
