export const APP_NAME = "mobile-backend"

export interface HealthReport {
  status: "ok"
  app: string
  time: string
}

export function health(): HealthReport {
  return { status: "ok", app: APP_NAME, time: new Date().toISOString() }
}
