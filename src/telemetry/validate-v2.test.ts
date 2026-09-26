import { describe, expect, it, vi } from "vitest"

import {
  boardsTelemetryTopicPattern,
  extractBoardIdFromBoardsTelemetryTopic,
  validateTelemetryV2Message,
} from "./validate-v2.js"

const PREFIX = "smarthome"
const TOPIC = "smarthome/boards/0/telemetry"

function validPayload(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    schemaVersion: 2,
    boardId: "0",
    values: { S1: 28.5, S2: 71 },
    ...overrides,
  }
}

describe("boardsTelemetryTopicPattern / extractBoardIdFromBoardsTelemetryTopic", () => {
  it("lấy boardId từ topic đúng mẫu theo prefix", () => {
    expect(extractBoardIdFromBoardsTelemetryTopic(TOPIC, PREFIX)).toBe("0")
    expect(extractBoardIdFromBoardsTelemetryTopic("nhaxanh/boards/A-1/telemetry", "nhaxanh")).toBe(
      "A-1",
    )
  })

  it("prefix động — regex escape đúng ký tự đặc biệt", () => {
    expect(boardsTelemetryTopicPattern("my.house").test("my.house/boards/0/telemetry")).toBe(true)
    expect(boardsTelemetryTopicPattern("my.house").test("myXhouse/boards/0/telemetry")).toBe(false)
  })

  it("trả null với topic sai mẫu", () => {
    expect(extractBoardIdFromBoardsTelemetryTopic("smarthome/boards/0/descriptor", PREFIX)).toBeNull()
    expect(extractBoardIdFromBoardsTelemetryTopic("smarthome/0/telemetry", PREFIX)).toBeNull()
    expect(extractBoardIdFromBoardsTelemetryTopic("smarthome/boards/telemetry", PREFIX)).toBeNull()
    expect(extractBoardIdFromBoardsTelemetryTopic("other/boards/0/telemetry", PREFIX)).toBeNull()
    expect(extractBoardIdFromBoardsTelemetryTopic("smarthome/boards/0/telemetry/extra", PREFIX)).toBeNull()
    // prefix khác không khớp
    expect(extractBoardIdFromBoardsTelemetryTopic(TOPIC, "nhaxanh")).toBeNull()
  })
})

describe("validateTelemetryV2Message — payload hợp lệ", () => {
  it("payload đúng + boardId khớp topic → trả payload đã parse", () => {
    const raw = JSON.stringify(validPayload())
    const result = validateTelemetryV2Message(TOPIC, raw, PREFIX)
    expect(result).not.toBeNull()
    expect(result?.topic).toBe(TOPIC)
    expect(result?.boardId).toBe("0")
    expect(result?.payload).toEqual({ schemaVersion: 2, boardId: "0", values: { S1: 28.5, S2: 71 } })
  })

  it("nhận Buffer", () => {
    const result = validateTelemetryV2Message(TOPIC, Buffer.from(JSON.stringify(validPayload()), "utf8"), PREFIX)
    expect(result?.payload.values.S1).toBe(28.5)
  })
})

describe("validateTelemetryV2Message — sai → null + WARN, không throw", () => {
  it("JSON hỏng → null", () => {
    expect(validateTelemetryV2Message(TOPIC, "{not json", PREFIX)).toBeNull()
  })

  it("sai schema v2 (schemaVersion 1, values sai kiểu) → null", () => {
    expect(validateTelemetryV2Message(TOPIC, JSON.stringify(validPayload({ schemaVersion: 1 })), PREFIX)).toBeNull()
    expect(validateTelemetryV2Message(TOPIC, JSON.stringify(validPayload({ values: "x" })), PREFIX)).toBeNull()
  })

  it("boardId lệch segment topic → null", () => {
    const raw = JSON.stringify(validPayload({ boardId: "1" }))
    expect(validateTelemetryV2Message(TOPIC, raw, PREFIX)).toBeNull()
  })

  it("topic sai mẫu → null (kể cả payload hợp lệ)", () => {
    const raw = JSON.stringify(validPayload())
    expect(validateTelemetryV2Message("smarthome/boards/0/status", raw, PREFIX)).toBeNull()
  })

  it("mọi nhánh sai đều chỉ WARN, không throw", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    try {
      expect(() => validateTelemetryV2Message(TOPIC, "{broken", PREFIX)).not.toThrow()
      expect(() =>
        validateTelemetryV2Message(TOPIC, JSON.stringify(validPayload({ boardId: "1" })), PREFIX),
      ).not.toThrow()
      expect(() => validateTelemetryV2Message("lạ", "{}", PREFIX)).not.toThrow()
      expect(warnSpy).toHaveBeenCalled()
    } finally {
      warnSpy.mockRestore()
    }
  })
})
