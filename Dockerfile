FROM golang:1.22-alpine AS build
RUN apk add --no-cache git
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -ldflags="-s -w" -o drop .

FROM alpine:3.20
RUN apk add --no-cache bash
COPY --from=build /src/drop /usr/local/bin/drop
EXPOSE 9800
ENTRYPOINT ["drop"]
