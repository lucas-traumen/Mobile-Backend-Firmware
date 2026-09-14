import { describe, expect, it } from "vitest"
import { APP_NAME, health } from "./index.js"

describe("health", () => {
  it("reports ok with app name and timestamp", () => {
    const report = health()
    expect(report.status).toBe("ok")
    expect(report.app).toBe(APP_NAME)
    expect(Number.isNaN(Date.parse(report.time))).toBe(false)
  })
})
