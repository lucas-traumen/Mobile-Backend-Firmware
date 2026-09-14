# syntax=docker/dockerfile:1

# ---- Stage build: typecheck + biên dịch TypeScript ra dist/ ----
FROM node:24-alpine AS build
WORKDIR /app

COPY package.json package-lock.json ./
RUN npm ci

COPY tsconfig.json tsconfig.build.json ./
COPY src ./src
# src/orchestrator-state.test.ts import plugin ngoài src — typecheck cần nó trong context
COPY .opencode/plugins ./.opencode/plugins

# typecheck trên tsconfig gốc (noEmit), rồi emit dist/ qua tsconfig.build.json
RUN npm run typecheck && npx tsc -p tsconfig.build.json

# ---- Stage runtime: chỉ dist/ + dependencies production ----
FROM node:24-alpine
WORKDIR /app
ENV NODE_ENV=production

COPY --from=build /app/dist ./dist
COPY package.json package-lock.json ./
RUN npm ci --omit=dev

USER node

CMD ["node", "dist/src/main.js"]
