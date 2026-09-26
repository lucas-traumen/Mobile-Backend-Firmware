import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import type { Point } from "@influxdata/influxdb-client"

import {
  InfluxWriter,
  MAX_QUEUE_SIZE,
  MAX_RETRY_DELAY_MS,
  RETRY_INITIAL_DELAY_MS,
  type TelemetryPointData,
  type WritableWriteApi,
} from "./influx-writer.js"

type FlushMock = ReturnType<typeof vi.fn>
interface MockWriteApi {
  writePoint: FlushMock
  flush: FlushMock
  dispose: FlushMock
}

function makeWriteApi(): MockWriteApi {
  return {
    writePoint: vi.fn(),
    flush: vi.fn().mockResolvedValue(undefined),
    dispose: vi.fn(),
  }
}

function makePointData(
  overrides: Partial<Omit<TelemetryPointData, "measurement">> & {
    roomId?: string
    boardId?: string
  } = {},
): TelemetryPointData {
  return {
    measurement: "sensors",
    tags: { roomId: overrides.roomId ?? "room1", boardId: overrides.boardId ?? "dev1" },
    fields: overrides.fields ?? { temperature: 25, humidity: 50 },
    timestamp: overrides.timestamp ?? new Date(),
  }
}

// Serialize giống WriteApi thật với precision "ms" (Date → epoch millis).
function lineOf(point: Point): string {
  const line = point.toLineProtocol({
    convertTime: (time) => (time instanceof Date ? String(time.getTime()) : undefined),
  })
  expect(line).toBeDefined()
  return line as string
}

describe("InfluxWriter", () => {
  let warnSpy: ReturnType<typeof vi.spyOn>
  let errorSpy: ReturnType<typeof vi.spyOn>

  beforeEach(() => {
    vi.useFakeTimers()
    warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    errorSpy = vi.spyOn(console, "error").mockImplementation(() => {})
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.restoreAllMocks()
  })

  it("ghi thành công: writePoint + flush, queue rỗng, line protocol đúng", async () => {
    const writeApi = makeWriteApi()
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData())
    await vi.advanceTimersByTimeAsync(0)

    expect(writeApi.writePoint).toHaveBeenCalledTimes(1)
    expect(writeApi.flush).toHaveBeenCalledTimes(1)
    expect(writer.queuedCount).toBe(0)

    const point = writeApi.writePoint.mock.calls[0][0] as Point
    // toLineProtocol sort tags + fields theo alphabet: boardId, roomId | humidity, temperature
    // + timestamp epoch millis ở cuối.
    expect(lineOf(point)).toMatch(
      /^sensors,boardId=dev1,roomId=room1 humidity=50,temperature=25 \d{13}$/,
    )
  })

  it("point v2: tags {boardId, roomId: boardId} — quy ước 1:1 còn hiệu lực", async () => {
    const writeApi = makeWriteApi()
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData({ roomId: "0", boardId: "0" }))
    await vi.advanceTimersByTimeAsync(0)

    const point = writeApi.writePoint.mock.calls[0][0] as Point
    expect(lineOf(point)).toMatch(/^sensors,boardId=0,roomId=0 /)
  })

  it("timestamp là epoch millis (UTC) của point truyền vào", async () => {
    const writeApi = makeWriteApi()
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData({ timestamp: new Date("2026-09-05T10:00:00.123Z") }))
    await vi.advanceTimersByTimeAsync(0)

    const point = writeApi.writePoint.mock.calls[0][0] as Point
    expect(lineOf(point)).toMatch(/\s1788602400123$/)
  })

  it("Influx lỗi → giữ điểm trong queue, retry đúng backoff đầu tiên", async () => {
    const writeApi = makeWriteApi()
    writeApi.flush.mockRejectedValueOnce(new Error("connection refused"))
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData())
    await vi.advanceTimersByTimeAsync(0)
    expect(writeApi.flush).toHaveBeenCalledTimes(1)
    expect(writer.queuedCount).toBe(1)

    // chưa tới hạn retry (500ms)
    await vi.advanceTimersByTimeAsync(RETRY_INITIAL_DELAY_MS - 1)
    expect(writeApi.flush).toHaveBeenCalledTimes(1)

    // đúng 500ms → retry thành công, queue xả
    await vi.advanceTimersByTimeAsync(1)
    expect(writeApi.flush).toHaveBeenCalledTimes(2)
    expect(writer.queuedCount).toBe(0)
  })

  it("backoff nhân đôi và chặn ở 30s", async () => {
    const writeApi = makeWriteApi()
    writeApi.flush.mockRejectedValue(new Error("down"))
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData())
    await vi.advanceTimersByTimeAsync(0)
    expect(writer.nextRetryDelayMs).toBe(RETRY_INITIAL_DELAY_MS * 2) // scheduled 500 → stored 1000

    // Mỗi vòng bắn đúng MỘT timer retry → đếm flush được xác định trước.
    for (let i = 0; i < 10; i++) {
      await vi.advanceTimersToNextTimerAsync()
    }
    // 1000→2000→4000→8000→16000→30000→... neo ở 30s
    expect(writer.nextRetryDelayMs).toBe(MAX_RETRY_DELAY_MS)
    expect(writeApi.flush).toHaveBeenCalledTimes(11) // 1 lần đầu + 10 retry
  })

  it("queue đầy (1000) → drop điểm cũ nhất + WARN, ghi tiếp khi Influx sống lại", async () => {
    const writeApi = makeWriteApi()
    writeApi.flush.mockRejectedValue(new Error("down"))
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    for (let i = 0; i <= MAX_QUEUE_SIZE; i++) {
      writer.writePointData(makePointData({ roomId: `room${i}`, fields: { temperature: i } }))
    }
    await vi.advanceTimersByTimeAsync(0)

    expect(writer.queuedCount).toBe(MAX_QUEUE_SIZE)
    expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("[influx-writer] queue full, dropped oldest point"))
    expect(
      warnSpy.mock.calls.filter((call) => String(call[0]).includes("queue full, dropped oldest point")),
    ).toHaveLength(1)

    // Influx sống lại → retry xả queue; room0 đã bị drop nên chỉ ghi room1..room1000
    writeApi.flush.mockResolvedValue(undefined)
    await vi.advanceTimersByTimeAsync(MAX_RETRY_DELAY_MS)
    expect(writer.queuedCount).toBe(0)

    const lines = (writeApi.writePoint.mock.calls as Array<[Point]>).map(([point]) => lineOf(point))
    expect(lines).toHaveLength(MAX_QUEUE_SIZE + 1) // 1 lần fail đầu + 1000 điểm queue
    expect(lines[0]).toContain("roomId=room0") // lần attempt đầu (thất bại)
    expect(lines.slice(1).filter((line) => line.includes("roomId=room0"))).toHaveLength(0)
    expect(lines[1]).toContain("roomId=room1")
    expect(lines[MAX_QUEUE_SIZE]).toContain(`roomId=room${MAX_QUEUE_SIZE}`)
  })

  it("point mới khi đang chờ retry → flush được ngay (không đợi backoff)", async () => {
    const writeApi = makeWriteApi()
    writeApi.flush.mockRejectedValueOnce(new Error("down"))
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData({ boardId: "a" }))
    await vi.advanceTimersByTimeAsync(0)
    expect(writer.queuedCount).toBe(1)

    writer.writePointData(makePointData({ boardId: "b" })) // point mới → pump ngay lập tức
    await vi.advanceTimersByTimeAsync(0)
    expect(writeApi.flush).toHaveBeenCalledTimes(3) // a fail, b thành công, retry a thành công
    expect(writer.queuedCount).toBe(0)
  })

  it("close: xả hết queue (best-effort) rồi dispose", async () => {
    const writeApi = makeWriteApi()
    writeApi.flush.mockRejectedValueOnce(new Error("down"))
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData({ boardId: "a" }))
    writer.writePointData(makePointData({ boardId: "b" }))
    await vi.advanceTimersByTimeAsync(0)
    expect(writer.queuedCount).toBe(2)

    writeApi.flush.mockResolvedValue(undefined)
    await writer.close()

    expect(writeApi.flush).toHaveBeenCalledTimes(3) // 1 fail + 2 xả lúc close
    expect(writer.queuedCount).toBe(0)
    expect(writeApi.dispose).toHaveBeenCalledTimes(1)
    expect(vi.getTimerCount()).toBe(0) // không còn retry timer
  })

  it("close khi Influx vẫn lỗi: log số điểm còn lại + vẫn dispose", async () => {
    const writeApi = makeWriteApi()
    writeApi.flush.mockRejectedValue(new Error("down"))
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)

    writer.writePointData(makePointData())
    await vi.advanceTimersByTimeAsync(0)
    expect(writer.queuedCount).toBe(1)

    await writer.close()

    expect(writeApi.dispose).toHaveBeenCalledTimes(1)
    expect(writer.queuedCount).toBe(1) // không ghi được → giữ nguyên, đã log
    expect(errorSpy).toHaveBeenCalledWith(expect.stringContaining("[influx-writer] close"))
    expect(vi.getTimerCount()).toBe(0)
  })

  it("writePointData sau close → bỏ qua", async () => {
    const writeApi = makeWriteApi()
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)
    await writer.close()

    writer.writePointData(makePointData())
    await vi.advanceTimersByTimeAsync(0)
    expect(writeApi.writePoint).not.toHaveBeenCalled()
    expect(writer.queuedCount).toBe(0)
  })

  it("fields tùy ý (map theo descriptor) ghi đúng line protocol + cùng chính sách queue", async () => {
    const writeApi = makeWriteApi()
    const writer = new InfluxWriter(() => writeApi as unknown as WritableWriteApi)
    const receivedAt = new Date("2026-09-15T10:00:00.000Z")

    writer.writePointData({
      measurement: "sensors",
      tags: { roomId: "0", boardId: "0" },
      fields: { temperature: 28.5, humidity: 71 },
      timestamp: receivedAt,
    })
    await vi.advanceTimersByTimeAsync(0)

    expect(writer.queuedCount).toBe(0)
    const point = writeApi.writePoint.mock.calls[0][0] as Point
    expect(lineOf(point)).toMatch(/^sensors,boardId=0,roomId=0 humidity=71,temperature=28.5 1789466400000$/)

    // Influx lỗi → giữ trong queue (cùng chính sách retry).
    writeApi.flush.mockRejectedValueOnce(new Error("down"))
    writer.writePointData({
      measurement: "sensors",
      tags: { roomId: "b", boardId: "b" },
      fields: { lux: 120 },
      timestamp: receivedAt,
    })
    await vi.advanceTimersByTimeAsync(0)
    expect(writer.queuedCount).toBe(1)
    expect(warnSpy).not.toHaveBeenCalledWith(expect.stringContaining("writePointData() sau khi close"))
  })
})
