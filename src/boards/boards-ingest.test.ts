import { describe, expect, it, vi } from "vitest"

import { BoardsTelemetryIngest, mapValuesToFields } from "./boards-ingest.js"
import type { BoardDescriptor } from "./descriptor.js"
import type { TelemetryPointData } from "../influx/influx-writer.js"
import { DescriptorRegistry } from "./descriptor-registry.js"

const PREFIX = "smarthome"
const TOPIC = "smarthome/boards/0/telemetry"
const RECEIVED_AT = new Date("2026-09-15T10:00:00.000Z")

function descriptor(overrides: Partial<BoardDescriptor> = {}): BoardDescriptor {
  return {
    schemaVersion: 1,
    boardId: "0",
    boardType: "A",
    sensors: [
      { channel: "S1", field: "temperature", unit: "°C" },
      { channel: "S2", field: "humidity", unit: "%" },
    ],
    relays: [{ channel: "K1" }, { channel: "K2" }, { channel: "K3" }],
    ...overrides,
  }
}

interface Harness {
  ingest: BoardsTelemetryIngest
  registry: DescriptorRegistry
  points: TelemetryPointData[]
  prefix: string
}

function makeHarness(options: { prefix?: string } = {}): Harness {
  const prefix = options.prefix ?? PREFIX
  const registry = new DescriptorRegistry({ prefix })
  const points: TelemetryPointData[] = []
  const ingest = new BoardsTelemetryIngest({
    prefix,
    registry,
    onPoint: (point) => points.push(point),
  })
  return { ingest, registry, points, prefix }
}

function loadDescriptor(harness: Harness, overrides: Partial<BoardDescriptor> = {}): void {
  harness.registry.handleMessage(
    `${harness.prefix}/boards/0/descriptor`,
    Buffer.from(JSON.stringify(descriptor(overrides))),
  )
}

describe("mapValuesToFields — map data-driven theo descriptor", () => {
  it("channel → field đúng theo descriptor.sensors (không nhánh if theo loại cảm biến)", () => {
    const { fields, unknownChannels } = mapValuesToFields(descriptor(), { S1: 28.5, S2: 71 })
    expect(fields).toEqual({ temperature: 28.5, humidity: 71 })
    expect(unknownChannels).toEqual([])
  })

  it("descriptor tùy ý (kênh S9 → lux) map theo đúng khai báo", () => {
    const luxDescriptor = descriptor({
      sensors: [{ channel: "S9", field: "lux", unit: "lx" }],
      relays: [],
    })
    const { fields, unknownChannels } = mapValuesToFields(luxDescriptor, { S9: 120 })
    expect(fields).toEqual({ lux: 120 })
    expect(unknownChannels).toEqual([])
  })

  it("kênh lạ bị lọc + báo danh sách; descriptor khai báo trùng field → giá trị sau thắng", () => {
    const { fields, unknownChannels } = mapValuesToFields(descriptor(), { S1: 28.5, S9: 1, S8: 2 })
    expect(fields).toEqual({ temperature: 28.5 })
    expect(unknownChannels).toEqual(["S9", "S8"])
  })

  it("descriptor khai báo trùng kênh → entry sau ghi đè", () => {
    const dup = descriptor({
      sensors: [
        { channel: "S1", field: "temperature", unit: "°C" },
        { channel: "S1", field: "temperature2", unit: "°C" },
      ],
    })
    const { fields } = mapValuesToFields(dup, { S1: 5 })
    expect(fields).toEqual({ temperature2: 5 })
  })
})

describe("BoardsTelemetryIngest — đường v2", () => {
  it("topicFilter đúng theo prefix", () => {
    expect(makeHarness().ingest.topicFilter).toBe("smarthome/boards/+/telemetry")
  })

  it("map theo descriptor → 1 point tags {boardId, roomId: boardId} + fields ngữ nghĩa + timestamp truyền vào", () => {
    const harness = makeHarness()
    loadDescriptor(harness)

    harness.ingest.ingest(TOPIC, Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "0", values: { S1: 28.5, S2: 71 } })), RECEIVED_AT)

    expect(harness.points).toHaveLength(1)
    const point = harness.points[0]
    expect(point.measurement).toBe("sensors")
    expect(point.tags).toEqual({ roomId: "0", boardId: "0" })
    expect(point.fields).toEqual({ temperature: 28.5, humidity: 71 })
    expect(point.timestamp).toBe(RECEIVED_AT)
  })

  it("kênh lạ → WARN + bỏ kênh đó, point vẫn ghi với kênh hợp lệ còn lại", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const harness = makeHarness()
    loadDescriptor(harness)
    try {
      harness.ingest.ingest(
        TOPIC,
        Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "0", values: { S1: 28.5, S9: 99 } })),
        RECEIVED_AT,
      )
      expect(harness.points).toHaveLength(1)
      expect(harness.points[0].fields).toEqual({ temperature: 28.5 })
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining('kênh "S9" không có trong descriptor'))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("hết kênh hợp lệ sau lọc → bỏ point + WARN, không gọi onPoint", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const harness = makeHarness()
    loadDescriptor(harness)
    try {
      harness.ingest.ingest(
        TOPIC,
        Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "0", values: { S9: 99 } })),
        RECEIVED_AT,
      )
      expect(harness.points).toHaveLength(0)
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("không còn kênh hợp lệ"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("cold-start: descriptor chưa có → WARN + bỏ point (KHÔNG queue), không crash", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const harness = makeHarness() // registry trống
    try {
      harness.ingest.ingest(
        TOPIC,
        Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "0", values: { S1: 28.5 } })),
        RECEIVED_AT,
      )
      expect(harness.points).toHaveLength(0)
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("chưa có descriptor"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("boardId lệch segment topic → bỏ + WARN (qua validate)", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const harness = makeHarness()
    loadDescriptor(harness)
    try {
      harness.ingest.ingest(
        TOPIC,
        Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "5", values: { S1: 28.5 } })),
        RECEIVED_AT,
      )
      expect(harness.points).toHaveLength(0)
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("boardId lệch topic"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("payload sai schema v2 → bỏ + WARN", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const harness = makeHarness()
    loadDescriptor(harness)
    try {
      harness.ingest.ingest(TOPIC, Buffer.from('{"schemaVersion":1,"deviceId":"0"}'), RECEIVED_AT)
      expect(harness.points).toHaveLength(0)
      expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("sai schema v2"))
    } finally {
      warnSpy.mockRestore()
    }
  })

  it("handleMessage: topic thuộc v2 → xử lý + trả true; topic khác → false", () => {
    const harness = makeHarness()
    loadDescriptor(harness)

    expect(harness.ingest.handleMessage(TOPIC, Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "0", values: { S1: 1 } })))).toBe(true)
    expect(harness.points).toHaveLength(1)
    // timestamp tự sinh là Date
    expect(harness.points[0].timestamp).toBeInstanceOf(Date)

    expect(harness.ingest.handleMessage("smarthome/0/telemetry", Buffer.from("{}"))).toBe(false)
    expect(harness.ingest.handleMessage("smarthome/boards/0/descriptor", Buffer.from("{}"))).toBe(false)
  })

  it("descriptor từ prefix khác không được dùng (prefix động theo env)", () => {
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    const harness = makeHarness({ prefix: "nhaxanh" })
    harness.registry.handleMessage(
      "nhaxanh/boards/0/descriptor",
      Buffer.from(JSON.stringify(descriptor())),
    )
    try {
      // topic prefix smarthome → không thuộc contract nhaxanh → handleMessage false
      expect(harness.ingest.handleMessage(TOPIC, Buffer.from("{}"))).toBe(false)
      expect(harness.points).toHaveLength(0)
    } finally {
      warnSpy.mockRestore()
    }
  })
})
