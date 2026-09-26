import { describe, expect, it, vi } from "vitest"

import { DescriptorRegistry } from "./descriptor-registry.js"

const PREFIX = "smarthome"

function descriptorJson(overrides: Record<string, unknown> = {}): string {
  return JSON.stringify({
    schemaVersion: 1,
    boardId: "0",
    boardType: "A",
    sensors: [
      { channel: "S1", field: "temperature", unit: "°C" },
      { channel: "S2", field: "humidity", unit: "%" },
    ],
    relays: [{ channel: "K1" }, { channel: "K2" }, { channel: "K3" }],
    ...overrides,
  })
}

function makeRegistry(prefix = PREFIX): DescriptorRegistry {
  return new DescriptorRegistry({ prefix })
}

describe("DescriptorRegistry", () => {
  it("topicFilter đúng theo prefix", () => {
    expect(makeRegistry().topicFilter).toBe("smarthome/boards/+/descriptor")
    expect(makeRegistry("nhaxanh").topicFilter).toBe("nhaxanh/boards/+/descriptor")
  })

  it("descriptor hợp lệ → cache + lookup trả descriptor đã parse", () => {
    const registry = makeRegistry()
    const handled = registry.handleMessage("smarthome/boards/0/descriptor", Buffer.from(descriptorJson()))
    expect(handled).toBe(true)
    expect(registry.size).toBe(1)

    const descriptor = registry.lookup("0")
    expect(descriptor).not.toBeNull()
    expect(descriptor?.boardId).toBe("0")
    expect(descriptor?.boardType).toBe("A")
    expect(descriptor?.sensors).toEqual([
      { channel: "S1", field: "temperature", unit: "°C" },
      { channel: "S2", field: "humidity", unit: "%" },
    ])
    expect(descriptor?.relays).toEqual([{ channel: "K1" }, { channel: "K2" }, { channel: "K3" }])
    expect(descriptor?.displayName).toBeUndefined()
  })

  it("displayName optional: nhận khi có, vẫn hợp lệ khi thiếu (shape M13 + M14)", () => {
    const registry = makeRegistry()
    registry.handleMessage(
      "smarthome/boards/0/descriptor",
      Buffer.from(descriptorJson({ displayName: "Phòng khách" })),
    )
    expect(registry.lookup("0")?.displayName).toBe("Phòng khách")

    registry.handleMessage("smarthome/boards/1/descriptor", Buffer.from(descriptorJson({ boardId: "1" })))
    expect(registry.lookup("1")?.displayName).toBeUndefined()
  })

  it("message mới hơn thay thế bản cũ trong cache (theo message retained mới nhất)", () => {
    const registry = makeRegistry()
    registry.handleMessage("smarthome/boards/0/descriptor", Buffer.from(descriptorJson()))
    registry.handleMessage(
      "smarthome/boards/0/descriptor",
      Buffer.from(descriptorJson({ boardType: "B", sensors: [{ channel: "S1", field: "lux", unit: "lx" }] })),
    )
    expect(registry.size).toBe(1)
    const descriptor = registry.lookup("0")
    expect(descriptor?.boardType).toBe("B")
    expect(descriptor?.sensors).toEqual([{ channel: "S1", field: "lux", unit: "lx" }])
  })

  it("boardId lệch segment topic → bỏ + WARN, không cache", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const registry = makeRegistry()
    try {
      const handled = registry.handleMessage(
        "smarthome/boards/0/descriptor",
        Buffer.from(descriptorJson({ boardId: "9" })),
      )
      expect(handled).toBe(true) // topic thuộc contract descriptor → đã xử lý (bỏ)
      expect(registry.size).toBe(0)
      expect(registry.lookup("0")).toBeNull()
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("lệch segment topic"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("descriptor sai shape → bỏ + WARN", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const registry = makeRegistry()
    try {
      // thiếu sensors
      const missing = JSON.parse(descriptorJson())
      delete missing.sensors
      registry.handleMessage("smarthome/boards/0/descriptor", Buffer.from(JSON.stringify(missing)))
      // displayName rỗng
      registry.handleMessage("smarthome/boards/0/descriptor", Buffer.from(descriptorJson({ displayName: "" })))
      // schemaVersion sai
      registry.handleMessage("smarthome/boards/0/descriptor", Buffer.from(descriptorJson({ schemaVersion: 2 })))

      expect(registry.size).toBe(0)
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("descriptor sai shape"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("JSON hỏng → bỏ + WARN, không crash", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const registry = makeRegistry()
    try {
      expect(() => registry.handleMessage("smarthome/boards/0/descriptor", Buffer.from("{broken"))).not.toThrow()
      expect(registry.size).toBe(0)
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("JSON hỏng"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("topic không thuộc contract descriptor → trả false (nhường path khác)", () => {
    const registry = makeRegistry()
    expect(registry.handleMessage("smarthome/boards/0/telemetry", Buffer.from("{}"))).toBe(false)
    expect(registry.handleMessage("smarthome/0/descriptor", Buffer.from("{}"))).toBe(false)
    expect(registry.handleMessage("nhaxanh/boards/0/descriptor", Buffer.from(descriptorJson()))).toBe(false)
    expect(registry.size).toBe(0)
  })
})
