import { z } from "zod"

// M14a — descriptor board (contract board-centric). Firmware v2 tự publish retained
// `{prefix}/boards/{boardId}/descriptor`; registry (descriptor-registry.ts) cache
// message mới nhất để ingest telemetry v2 map kênh → field Influx.
//
// `displayName` optional do firmware v2 thêm khi Kconfig `BOARD_DISPLAY_NAME`
// được đặt (M14 — thêm vào CUỐI shape theo quyết định 5 M13).
export const boardDescriptorSchema = z.object({
  schemaVersion: z.literal(1),
  boardId: z.string().regex(/^[a-zA-Z0-9_-]+/),
  boardType: z.string().min(1),
  sensors: z.array(
    z.object({
      channel: z.string().min(1),
      field: z.string().min(1),
      unit: z.string(),
    }),
  ),
  relays: z.array(z.object({ channel: z.string().min(1) })),
  displayName: z.string().min(1).optional(),
})

export type BoardDescriptor = z.infer<typeof boardDescriptorSchema>
