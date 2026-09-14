import { EventEmitter } from "node:events"
import { beforeEach, describe, expect, it, vi } from "vitest"
import mqtt, { type MqttClient } from "mqtt"

import { MqttService, TELEMETRY_TOPIC_FILTER, type TelemetrySink } from "./mqtt-service.js"
import type { TelemetryPayload } from "../telemetry/schema.js"

vi.mock("mqtt", () => ({
  default: { connect: vi.fn() },
}))

const connectMock = vi.mocked(mqtt.connect)

class FakeMqttClient extends EventEmitter {
  subscribe = vi.fn(
    (topic: string, _opts: { qos: number }, cb?: (err: Error | null) => void): unknown => {
      cb?.(null)
      return this
    },
  )
  end = vi.fn((_force: boolean, _opts: object, cb?: () => void): unknown => {
    cb?.()
    return this
  })
}

function makePayload(overrides: Partial<TelemetryPayload> = {}): TelemetryPayload {
  return {
    schemaVersion: 1,
    deviceId: "dev1",
    roomId: "room1",
    temperature: 25,
    humidity: 50,
    ...overrides,
  }
}

function makeService(sink: TelemetrySink): MqttService {
  return new MqttService({
    url: "mqtt://broker.test:1883",
    username: "user1",
    password: "pass1",
    sink,
  })
}

describe("MqttService", () => {
  let client: FakeMqttClient
  let sink: TelemetrySink & { write: ReturnType<typeof vi.fn> }
  let warnSpy: ReturnType<typeof vi.spyOn>
  let errorSpy: ReturnType<typeof vi.spyOn>

  beforeEach(() => {
    vi.clearAllMocks()
    client = new FakeMqttClient()
    connectMock.mockReturnValue(client as unknown as MqttClient)
    sink = { write: vi.fn() }
    warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    errorSpy = vi.spyOn(console, "error").mockImplementation(() => {})
  })

  it("connect đúng url + credentials + tự reconnect (reconnectPeriod > 0)", async () => {
    const service = makeService(sink)
    await service.start()

    expect(connectMock).toHaveBeenCalledWith(
      "mqtt://broker.test:1883",
      expect.objectContaining({
        username: "user1",
        password: "pass1",
        reconnectPeriod: expect.any(Number),
      }),
    )
    const options = connectMock.mock.calls[0][1]
    expect(options?.reconnectPeriod).toBeGreaterThan(0)
  })

  it("subscribe đúng topic filter khi connected", async () => {
    const service = makeService(sink)
    await service.start()
    client.emit("connect")

    expect(client.subscribe).toHaveBeenCalledWith(
      TELEMETRY_TOPIC_FILTER,
      expect.objectContaining({ qos: 1 }),
      expect.any(Function),
    )
  })

  it("message hợp lệ → validate xong gọi sink.write với payload đã parse", async () => {
    const service = makeService(sink)
    await service.start()
    client.emit("connect")

    client.emit("message", "smarthome/dev1/telemetry", Buffer.from(JSON.stringify(makePayload())))
    expect(sink.write).toHaveBeenCalledTimes(1)
    expect(sink.write).toHaveBeenCalledWith(makePayload())
  })

  it("message sai (JSON hỏng / deviceId lệch / topic lạ) → log + bỏ, không gọi sink, không crash", async () => {
    const service = makeService(sink)
    await service.start()
    client.emit("connect")

    expect(() => client.emit("message", "smarthome/dev1/telemetry", Buffer.from("{broken"))).not.toThrow()
    expect(() =>
      client.emit(
        "message",
        "smarthome/other-device/telemetry",
        Buffer.from(JSON.stringify(makePayload())),
      ),
    ).not.toThrow()
    expect(() =>
      client.emit("message", "smarthome/dev1/status", Buffer.from(JSON.stringify(makePayload()))),
    ).not.toThrow()

    expect(sink.write).not.toHaveBeenCalled()
    expect(warnSpy).toHaveBeenCalled()
  })

  it("sink.write ném lỗi → bắt lại, không crash event loop", async () => {
    sink.write.mockImplementation(() => {
      throw new Error("sink exploded")
    })
    const service = makeService(sink)
    await service.start()
    client.emit("connect")

    expect(() =>
      client.emit("message", "smarthome/dev1/telemetry", Buffer.from(JSON.stringify(makePayload()))),
    ).not.toThrow()
    expect(errorSpy).toHaveBeenCalledWith(expect.stringContaining("[mqtt-service]"))
  })

  it("event 'error' → chỉ log, không crash", async () => {
    const service = makeService(sink)
    await service.start()

    expect(() => client.emit("error", new Error("socket hang up"))).not.toThrow()
    expect(errorSpy).toHaveBeenCalledWith(expect.stringContaining("socket hang up"))
  })

  it("reconnect: 'connect' bắn lại → resubscribe", async () => {
    const service = makeService(sink)
    await service.start()

    client.emit("connect") // lần nối đầu
    client.emit("close") // mất kết nối
    client.emit("connect") // reconnect thành công

    expect(client.subscribe).toHaveBeenCalledTimes(2)
    expect(client.subscribe).toHaveBeenLastCalledWith(
      TELEMETRY_TOPIC_FILTER,
      expect.objectContaining({ qos: 1 }),
      expect.any(Function),
    )
  })

  it("start hai lần → không connect thêm client thứ hai", async () => {
    const service = makeService(sink)
    await service.start()
    await service.start()
    expect(connectMock).toHaveBeenCalledTimes(1)
  })

  it("stop: end client, lần sau start nối lại client mới", async () => {
    const service = makeService(sink)
    await service.start()
    await service.stop()
    expect(client.end).toHaveBeenCalledWith(false, {}, expect.any(Function))

    const second = new FakeMqttClient()
    connectMock.mockReturnValueOnce(second as unknown as MqttClient)
    await service.start()
    expect(connectMock).toHaveBeenCalledTimes(2)
  })
})
