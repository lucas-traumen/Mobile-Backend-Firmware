import { describe, expect, it } from "vitest"

import { telemetryV2Schema } from "./schema-v2.js"

function validPayload(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    schemaVersion: 2,
    boardId: "0",
    values: { S1: 28.5, S2: 71 },
    ...overrides,
  }
}

describe("telemetryV2Schema — payload hợp lệ", () => {
  it("payload đúng shape → parse thành công", () => {
    const parsed = telemetryV2Schema.safeParse(validPayload())
    expect(parsed.success).toBe(true)
    if (parsed.success) {
      expect(parsed.data).toEqual({ schemaVersion: 2, boardId: "0", values: { S1: 28.5, S2: 71 } })
    }
  })

  it("values nhận kênh tùy ý (generic — không giới hạn tên kênh)", () => {
    const parsed = telemetryV2Schema.safeParse(
      validPayload({ values: { S1: 1, "S2-S3": 2, lux_01: 0, "S4.x": -3.25 } }),
    )
    expect(parsed.success).toBe(true)
  })

  it("boardId nhận chữ/số/gạch ngang/gạch dưới", () => {
    for (const boardId of ["0", "A-0011", "phong_khach", "x9"]) {
      expect(telemetryV2Schema.safeParse(validPayload({ boardId })).success).toBe(true)
    }
  })

  it("KHÔNG range-check vật lý: giá trị ngoài khoảng SHT3x vẫn hợp lệ (khác v1)", () => {
    // Kênh v2 generic, descriptor không mang khoảng giới hạn → schema chỉ đảm bảo finite.
    expect(telemetryV2Schema.safeParse(validPayload({ values: { S1: -999, S2: 5000 } })).success).toBe(
      true,
    )
  })

  it("values rỗng vẫn đúng shape (hợp lệ ở tầng schema; ingest tự bỏ point khi hết kênh)", () => {
    expect(telemetryV2Schema.safeParse(validPayload({ values: {} })).success).toBe(true)
  })
})

describe("telemetryV2Schema — payload sai → fail", () => {
  it("schemaVersion sai (1) → fail", () => {
    expect(telemetryV2Schema.safeParse(validPayload({ schemaVersion: 1 })).success).toBe(false)
  })

  it("thiếu schemaVersion → fail", () => {
    const { schemaVersion: _schemaVersion, ...missing } = validPayload()
    expect(telemetryV2Schema.safeParse(missing).success).toBe(false)
  })

  it("thiếu boardId → fail", () => {
    const { boardId: _boardId, ...missing } = validPayload()
    expect(telemetryV2Schema.safeParse(missing).success).toBe(false)
  })

  it("thiếu values → fail", () => {
    const { values: _values, ...missing } = validPayload()
    expect(telemetryV2Schema.safeParse(missing).success).toBe(false)
  })

  it("boardId rỗng → fail", () => {
    expect(telemetryV2Schema.safeParse(validPayload({ boardId: "" })).success).toBe(false)
  })

  it("boardId bắt đầu ký tự lạ → fail", () => {
    expect(telemetryV2Schema.safeParse(validPayload({ boardId: "!x" })).success).toBe(false)
    expect(telemetryV2Schema.safeParse(validPayload({ boardId: " " })).success).toBe(false)
  })

  it("values value sai kiểu (string / boolean) → fail", () => {
    expect(
      telemetryV2Schema.safeParse(validPayload({ values: { S1: "28.5" } })).success,
    ).toBe(false)
    expect(telemetryV2Schema.safeParse(validPayload({ values: { S1: true } })).success).toBe(false)
  })

  it("values sai kiểu (mảng) → fail", () => {
    expect(telemetryV2Schema.safeParse(validPayload({ values: [1, 2] })).success).toBe(false)
  })

  it("số ngoài double range (1e400 → Infinity) → fail (.finite chặn)", () => {
    expect(telemetryV2Schema.safeParse(validPayload({ values: { S1: 1e400 } })).success).toBe(false)
    expect(telemetryV2Schema.safeParse(validPayload({ values: { S1: -1e400 } })).success).toBe(false)
  })

  it("payload không phải object → fail", () => {
    expect(telemetryV2Schema.safeParse([1]).success).toBe(false)
    expect(telemetryV2Schema.safeParse("x").success).toBe(false)
  })
})
