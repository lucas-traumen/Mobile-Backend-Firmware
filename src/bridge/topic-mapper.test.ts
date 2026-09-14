import { describe, expect, it, vi } from "vitest"

import {
  FIRMWARE_RELAY_STATE_FILTER,
  FIRMWARE_STATUS_FILTER,
  FIRMWARE_TELEMETRY_FILTER,
  classifyBridgeTopic,
  mapFrontendRelayCommand,
  mapRelayStateToFrontend,
  mapStatusToFrontend,
  mapTelemetryToFrontend,
  type BridgeLogger,
  type BridgePublish,
} from "./topic-mapper.js"

const PREFIX = "smarthome"

function makeLogger() {
  return {
    debug: vi.fn<(message: string) => void>(),
    info: vi.fn<(message: string) => void>(),
    warn: vi.fn<(message: string) => void>(),
    error: vi.fn<(message: string) => void>(),
  } satisfies BridgeLogger
}

function topics(publishes: BridgePublish[]): string[] {
  return publishes.map((publish) => publish.topic)
}

function telemetry(overrides: Record<string, unknown> = {}): string {
  return JSON.stringify({
    schemaVersion: 1,
    deviceId: "0",
    roomId: "phong-khach",
    temperature: 25.74,
    humidity: 58.33,
    ...overrides,
  })
}

describe("topic filter constants", () => {
  it("firmware filters cố định dù prefix frontend đổi", () => {
    expect(FIRMWARE_TELEMETRY_FILTER).toBe("smarthome/+/telemetry")
    expect(FIRMWARE_STATUS_FILTER).toBe("smarthome/+/status")
    expect(FIRMWARE_RELAY_STATE_FILTER).toBe("smarthome/+/relay/state")
  })
})

describe("mapTelemetryToFrontend", () => {
  it("tách temperature/humidity thành 2 topic số, retained, dùng roomId trong payload", () => {
    const logger = makeLogger()
    const publishes = mapTelemetryToFrontend("smarthome/0/telemetry", telemetry(), PREFIX, logger)

    expect(publishes).toEqual([
      {
        topic: "smarthome/room/phong-khach/sensor/temperature",
        payload: "25.74",
        qos: 1,
        retain: true,
      },
      {
        topic: "smarthome/room/phong-khach/sensor/humidity",
        payload: "58.33",
        qos: 1,
        retain: true,
      },
    ])
    expect(logger.debug).not.toHaveBeenCalled()
  })

  it("prefix tùy biến được áp cho topic frontend", () => {
    const publishes = mapTelemetryToFrontend("smarthome/0/telemetry", telemetry(), "nhaxanh")
    expect(topics(publishes)).toEqual([
      "nhaxanh/room/phong-khach/sensor/temperature",
      "nhaxanh/room/phong-khach/sensor/humidity",
    ])
  })

  it("nhận Buffer giống string", () => {
    const publishes = mapTelemetryToFrontend(
      "smarthome/0/telemetry",
      Buffer.from(telemetry()),
      PREFIX,
    )
    expect(publishes).toHaveLength(2)
  })

  it("payload sai shape (schemaVersion 2, thiếu field, temperature phi lý) → bỏ + log DEBUG", () => {
    const logger = makeLogger()
    const bad = [
      telemetry({ schemaVersion: 2 }),
      "{}",
      telemetry({ temperature: 999 }),
      telemetry({ humidity: Number.NaN }),
      telemetry({ roomId: "" }),
    ]
    for (const payload of bad) {
      expect(mapTelemetryToFrontend("smarthome/0/telemetry", payload, PREFIX, logger)).toEqual([])
    }
    expect(logger.debug).toHaveBeenCalledTimes(bad.length)
  })

  it("JSON hỏng / topic lạ → bỏ, không throw", () => {
    const logger = makeLogger()
    expect(() =>
      mapTelemetryToFrontend("smarthome/0/telemetry", "{broken", PREFIX, logger),
    ).not.toThrow()
    expect(mapTelemetryToFrontend("smarthome/0/telemetry", "{broken", PREFIX, logger)).toEqual([])
    expect(mapTelemetryToFrontend("other/0/telemetry", telemetry(), PREFIX, logger)).toEqual([])
    expect(mapTelemetryToFrontend("smarthome/0/status", telemetry(), PREFIX, logger)).toEqual([])
    expect(logger.debug).toHaveBeenCalled()
  })

  it("deviceId lệch kiểu (number) → bỏ", () => {
    const publishes = mapTelemetryToFrontend(
      "smarthome/0/telemetry",
      telemetry({ deviceId: 0 }),
      PREFIX,
    )
    expect(publishes).toEqual([])
  })
})

describe("mapStatusToFrontend", () => {
  it("online/offline → room status retained, roomId = deviceId", () => {
    const logger = makeLogger()
    expect(mapStatusToFrontend("smarthome/0/status", "online", PREFIX, logger)).toEqual([
      { topic: "smarthome/room/0/status", payload: "online", qos: 1, retain: true },
    ])
    expect(mapStatusToFrontend("smarthome/3/status", "offline", PREFIX, logger)).toEqual([
      { topic: "smarthome/room/3/status", payload: "offline", qos: 1, retain: true },
    ])
    expect(logger.debug).not.toHaveBeenCalled()
  })

  it("payload lạ / topic lạ → bỏ + log DEBUG", () => {
    const logger = makeLogger()
    expect(mapStatusToFrontend("smarthome/0/status", "connecting", PREFIX, logger)).toEqual([])
    expect(mapStatusToFrontend("smarthome/0/status", "", PREFIX, logger)).toEqual([])
    expect(mapStatusToFrontend("smarthome/0/telemetry", "online", PREFIX, logger)).toEqual([])
    expect(logger.debug).toHaveBeenCalledTimes(3)
  })
})

describe("mapRelayStateToFrontend", () => {
  it("tách từng kênh K1..K3 thành stat topic retained", () => {
    const logger = makeLogger()
    const payload = JSON.stringify({ schemaVersion: 1, K1: "ON", K2: "ON", K3: "OFF" })
    const publishes = mapRelayStateToFrontend("smarthome/0/relay/state", payload, PREFIX, logger)

    expect(publishes).toEqual([
      { topic: "smarthome/room/0/stat/relay/1", payload: "ON", qos: 1, retain: true },
      { topic: "smarthome/room/0/stat/relay/2", payload: "ON", qos: 1, retain: true },
      { topic: "smarthome/room/0/stat/relay/3", payload: "OFF", qos: 1, retain: true },
    ])
    expect(logger.debug).not.toHaveBeenCalled()
  })

  it("chỉ publish kênh có trong payload, giữ thứ tự tăng dần", () => {
    const payload = JSON.stringify({ schemaVersion: 1, K3: "ON", K1: "OFF" })
    expect(topics(mapRelayStateToFrontend("smarthome/7/relay/state", payload, PREFIX))).toEqual([
      "smarthome/room/7/stat/relay/1",
      "smarthome/room/7/stat/relay/3",
    ])
  })

  it("kênh K4..K10 (chưa có trên firmware) → bỏ + log DEBUG, kênh hợp lệ vẫn đi", () => {
    const logger = makeLogger()
    const payload = JSON.stringify({ schemaVersion: 1, K1: "ON", K4: "ON", K10: "OFF" })
    const publishes = mapRelayStateToFrontend("smarthome/0/relay/state", payload, PREFIX, logger)

    expect(topics(publishes)).toEqual(["smarthome/room/0/stat/relay/1"])
    expect(logger.debug).toHaveBeenCalledTimes(2)
  })

  it("payload sai shape / state lạ / JSON hỏng → bỏ, không throw", () => {
    const logger = makeLogger()
    const bad = [
      "{broken",
      JSON.stringify({ schemaVersion: 2, K1: "ON" }),
      JSON.stringify({ schemaVersion: 1, K1: "BLINK" }),
      JSON.stringify(["not", "object"]),
    ]
    for (const payload of bad) {
      expect(() =>
        mapRelayStateToFrontend("smarthome/0/relay/state", payload, PREFIX, logger),
      ).not.toThrow()
      expect(mapRelayStateToFrontend("smarthome/0/relay/state", payload, PREFIX, logger)).toEqual([])
    }
    expect(mapRelayStateToFrontend("smarthome/0/status", "online", PREFIX, logger)).toEqual([])
    expect(logger.debug).toHaveBeenCalled()
  })

  it("bỏ field lạ (schemaVersion, roomId) không tạo topic rác", () => {
    const payload = JSON.stringify({
      schemaVersion: 1,
      roomId: "phong-khach",
      K1: "ON",
      note: "x",
    })
    expect(topics(mapRelayStateToFrontend("smarthome/0/relay/state", payload, PREFIX))).toEqual([
      "smarthome/room/0/stat/relay/1",
    ])
  })
})

describe("mapFrontendRelayCommand", () => {
  it("slot 1..3 → firmware K{n}/set, payload thuần, không retained", () => {
    const logger = makeLogger()
    expect(
      mapFrontendRelayCommand("smarthome/room/0/cmnd/relay/1", "ON", PREFIX, logger),
    ).toEqual([{ topic: "smarthome/0/relay/K1/set", payload: "ON", qos: 1, retain: false }])
    expect(
      mapFrontendRelayCommand("smarthome/room/phong-khach/cmnd/relay/2", "OFF", PREFIX, logger),
    ).toEqual([
      { topic: "smarthome/phong-khach/relay/K2/set", payload: "OFF", qos: 1, retain: false },
    ])
    expect(
      mapFrontendRelayCommand("smarthome/room/9/cmnd/relay/3", "ON", PREFIX, logger),
    ).toEqual([{ topic: "smarthome/9/relay/K3/set", payload: "ON", qos: 1, retain: false }])
    expect(logger.debug).not.toHaveBeenCalled()
  })

  it("slot 4..10 → bỏ qua + log DEBUG (không publish)", () => {
    const logger = makeLogger()
    for (const slot of [4, 5, 6, 7, 8, 9, 10]) {
      const publishes = mapFrontendRelayCommand(
        `smarthome/room/0/cmnd/relay/${slot}`,
        "ON",
        PREFIX,
        logger,
      )
      expect(publishes).toEqual([])
    }
    expect(logger.debug).toHaveBeenCalledTimes(7)
  })

  it("slot 0 / không phải số / vượt 10 → bỏ", () => {
    const logger = makeLogger()
    for (const slot of ["0", "abc", "11", "-1"]) {
      expect(
        mapFrontendRelayCommand(`smarthome/room/0/cmnd/relay/${slot}`, "ON", PREFIX, logger),
      ).toEqual([])
    }
    expect(logger.debug).toHaveBeenCalledTimes(4)
  })

  it("payload không phải ON/OFF (JSON, chữ thường, rỗng) → bỏ + log", () => {
    const logger = makeLogger()
    for (const payload of ['{"state":"ON"}', "on", "", "OFF\n"]) {
      const publishes = mapFrontendRelayCommand(
        "smarthome/room/0/cmnd/relay/1",
        payload,
        PREFIX,
        logger,
      )
      if (payload === "OFF\n") {
        expect(publishes).toEqual([
          { topic: "smarthome/0/relay/K1/set", payload: "OFF", qos: 1, retain: false },
        ])
      } else {
        expect(publishes).toEqual([])
      }
    }
    expect(logger.debug).toHaveBeenCalledTimes(3)
  })

  it("accept Buffer và trim khoảng trắng", () => {
    expect(
      mapFrontendRelayCommand("smarthome/room/0/cmnd/relay/1", Buffer.from(" ON "), PREFIX),
    ).toEqual([{ topic: "smarthome/0/relay/K1/set", payload: "ON", qos: 1, retain: false }])
  })

  it("prefix khác → vẫn nhận đúng contract frontend", () => {
    expect(
      mapFrontendRelayCommand("nhaxanh/room/0/cmnd/relay/1", "ON", "nhaxanh"),
    ).toEqual([{ topic: "smarthome/0/relay/K1/set", payload: "ON", qos: 1, retain: false }])
    // Prefix sai → không khớp, bỏ.
    expect(mapFrontendRelayCommand("nhaxanh/room/0/cmnd/relay/1", "ON", PREFIX)).toEqual([])
  })
})

describe("classifyBridgeTopic", () => {
  it("nhận diện đúng 4 nhóm + topic lạ", () => {
    expect(classifyBridgeTopic("smarthome/0/telemetry", PREFIX)).toBe("telemetry")
    expect(classifyBridgeTopic("smarthome/0/status", PREFIX)).toBe("status")
    expect(classifyBridgeTopic("smarthome/0/relay/state", PREFIX)).toBe("relayState")
    expect(classifyBridgeTopic("smarthome/room/0/cmnd/relay/1", PREFIX)).toBe("relayCommand")
    expect(classifyBridgeTopic("smarthome/0/relay/K1/set", PREFIX)).toBeNull()
    expect(classifyBridgeTopic("smarthome/room/0/sensor/temperature", PREFIX)).toBeNull()
  })
})

describe("an toàn payload rác", () => {
  it("mọi mapper không throw và trả [] với input rác bất kỳ", () => {
    const logger = makeLogger()
    const garbage = ["", "{", "\u0000", "null", "undefined", "[]", "123", Buffer.from([0xff, 0xfe])]
    for (const payload of garbage) {
      expect(() => mapTelemetryToFrontend("smarthome/0/telemetry", payload, PREFIX, logger)).not.toThrow()
      expect(() => mapStatusToFrontend("smarthome/0/status", payload, PREFIX, logger)).not.toThrow()
      expect(() =>
        mapRelayStateToFrontend("smarthome/0/relay/state", payload, PREFIX, logger),
      ).not.toThrow()
      expect(() =>
        mapFrontendRelayCommand("smarthome/room/0/cmnd/relay/1", payload, PREFIX, logger),
      ).not.toThrow()
    }
    expect(logger.debug).toHaveBeenCalled()
    expect(logger.error).not.toHaveBeenCalled()
  })
})
