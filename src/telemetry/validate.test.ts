import { describe, expect, it } from "vitest"

import { extractDeviceIdFromTopic, validateTelemetryMessage } from "./validate.js"
import type { TelemetryPayload } from "./schema.js"

const TOPIC = "smarthome/esp32-living-room/telemetry"

function validPayload(overrides: Partial<TelemetryPayload> = {}): TelemetryPayload {
  return {
    schemaVersion: 1,
    deviceId: "esp32-living-room",
    roomId: "living-room",
    temperature: 25.4,
    humidity: 60,
    ...overrides,
  }
}

describe("extractDeviceIdFromTopic", () => {
  it("lấy deviceId từ topic đúng mẫu", () => {
    expect(extractDeviceIdFromTopic(TOPIC)).toBe("esp32-living-room")
  })

  it("trả null với topic sai mẫu", () => {
    expect(extractDeviceIdFromTopic("smarthome/telemetry")).toBeNull()
    expect(extractDeviceIdFromTopic("smarthome/dev1/status")).toBeNull()
    expect(extractDeviceIdFromTopic("smarthome/dev1/telemetry/extra")).toBeNull()
    expect(extractDeviceIdFromTopic("other/dev1/telemetry")).toBeNull()
    expect(extractDeviceIdFromTopic("smarthome//telemetry")).toBeNull()
  })
})

describe("validateTelemetryMessage — payload hợp lệ", () => {
  it("payload đúng + deviceId khớp topic → trả payload đã parse", () => {
    const raw = JSON.stringify(validPayload())
    const result = validateTelemetryMessage(TOPIC, raw)
    expect(result).not.toBeNull()
    expect(result?.topic).toBe(TOPIC)
    expect(result?.deviceId).toBe("esp32-living-room")
    expect(result?.payload).toEqual(validPayload())
  })

  it("nhận Buffer", () => {
    const result = validateTelemetryMessage(TOPIC, Buffer.from(JSON.stringify(validPayload()), "utf8"))
    expect(result?.payload.humidity).toBe(60)
  })

  it("biên SHT3x hợp lệ: temperature −40/125, humidity 0/100", () => {
    expect(
      validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ temperature: -40, humidity: 0 }))),
    ).not.toBeNull()
    expect(
      validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ temperature: 125, humidity: 100 }))),
    ).not.toBeNull()
  })
})

describe("validateTelemetryMessage — payload sai → null + không throw", () => {
  it("JSON hỏng → null", () => {
    expect(validateTelemetryMessage(TOPIC, "{not json")).toBeNull()
  })

  it("NaN trong payload → JSON.parse fail → null", () => {
    // JSON không biểu diễn được NaN — chuỗi như vậy là "JSON hỏng" và bị loại.
    const raw = `{"schemaVersion":1,"deviceId":"esp32-living-room","roomId":"living-room","temperature":NaN,"humidity":60}`
    expect(validateTelemetryMessage(TOPIC, raw)).toBeNull()
  })

  it("Infinity qua số ngoài double range (1e400) → null (.finite chặn)", () => {
    const raw = `{"schemaVersion":1,"deviceId":"esp32-living-room","roomId":"living-room","temperature":1e400,"humidity":60}`
    expect(validateTelemetryMessage(TOPIC, raw)).toBeNull()
  })

  it("-Infinity (−1e400) → null", () => {
    const raw = `{"schemaVersion":1,"deviceId":"esp32-living-room","roomId":"living-room","temperature":-1e400,"humidity":60}`
    expect(validateTelemetryMessage(TOPIC, raw)).toBeNull()
  })

  it("thiếu trường (humidity) → null", () => {
    const { humidity: _humidity, ...missing } = validPayload()
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(missing))).toBeNull()
  })

  it("thiếu schemaVersion → null", () => {
    const { schemaVersion: _schemaVersion, ...missing } = validPayload()
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(missing))).toBeNull()
  })

  it("schemaVersion sai (2) → null", () => {
    const bad = { ...validPayload(), schemaVersion: 2 } as unknown as TelemetryPayload
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(bad))).toBeNull()
  })

  it("temperature ngoài khoảng SHT3x (−41 và 126) → null", () => {
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ temperature: -41 })))).toBeNull()
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ temperature: 126 })))).toBeNull()
  })

  it("humidity ngoài khoảng (−1 và 101) → null", () => {
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ humidity: -1 })))).toBeNull()
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ humidity: 101 })))).toBeNull()
  })

  it("temperature sai kiểu (string) → null", () => {
    expect(
      validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ temperature: "25" as unknown as number }))),
    ).toBeNull()
  })

  it("roomId rỗng → null", () => {
    expect(validateTelemetryMessage(TOPIC, JSON.stringify(validPayload({ roomId: "" })))).toBeNull()
  })

  it("deviceId lệch topic → null", () => {
    const raw = JSON.stringify(validPayload({ deviceId: "esp32-bedroom" }))
    expect(validateTelemetryMessage(TOPIC, raw)).toBeNull()
  })

  it("topic sai mẫu → null (kể cả payload hợp lệ)", () => {
    const raw = JSON.stringify(validPayload())
    expect(validateTelemetryMessage("smarthome/dev1/status", raw)).toBeNull()
  })
})
