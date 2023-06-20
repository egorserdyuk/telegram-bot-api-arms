# Unofficial Docker image of Telegram Bot API for ARM systems

Here is Docker image for [Telegram bot API](https://github.com/tdlib/telegram-bot-api)

The source code was taken from the repository [Aiogram Telegram bot's API for 386/amd64](https://github.com/aiogram/telegram-bot-api)

The Telegram Bot API provides an HTTP API for creating [Telegram Bots](https://core.telegram.org/bots).

If you've got any questions about bots or would like to report an issue with your bot, kindly contact us at [@BotSupport](https://t.me/BotSupport) in Telegram.

To build yourself using `Docker BuildX` use this command: `docker buildx build --platform "linux/arm64,linux/arm/v7,linux/arm/v8" -t telegram-bot-api-arms . -f Dockerfile.txt`

## Quick reference

Before start, you will need to obtain `api-id` and `api-hash` as described in https://core.telegram.org/api/obtaining_api_id and specify them using the `TELEGRAM_API_ID` and `TELEGRAM_API_HASH` environment variables.

And then to start the Telegram Bot API all you need to do is
`docker run -d -p 8081:8081 --name=telegram-bot-api-arms --restart=always -v telegram-bot-api-arms-data:/var/lib/telegram-bot-api-arms -e TELEGRAM_API_ID=<api_id> -e TELEGRAM_API_HASH=<api-hash> thecooldump/telegram-bot-api-arms:latest`

## Configuration

Container can be configured via environment variables

### `TELEGRAM_API_ID`, `TELEGRAM_API_HASH`

Application identifiers for Telegram API access, which can be obtained at https://my.telegram.org as described in https://core.telegram.org/api/obtaining_api_id

### `TELEGRAM_STAT`

Enable statistics HTTP endpoint.

Usage: `-e TELEGRAM_STAT=1 -p 8082:8082` and then check that `curl http://<host>:8082` returns server statistic


### `TELEGRAM_FILTER`

"<remainder>/<modulo>". Allow only bots with 'bot_user_id % modulo == remainder'


### `TELEGRAM_MAX_WEBHOOK_CONNECTIONS`

default value of the maximum webhook connections per bot

### `TELEGRAM_VERBOSITY`

log verbosity level

### `TELEGRAM_LOG_FILE`

Filename where logs will be redirected (By default logs will be written to stdout/stderr streams)

### `TELEGRAM_MAX_CONNECTIONS`

maximum number of open file descriptors

### `TELEGRAM_PROXY`

HTTP proxy server for outgoing webhook requests in the format http://host:port

### `TELEGRAM_LOCAL`

allow the Bot API server to serve local requests

### `TELEGRAM_HTTP_IP_ADDRESS`

Use the `TELEGRAM_HTTP_IP_ADDRESS: "[::]"` parameter to listen on the ipv6 intranet

## Start with persistent storage

Server working directory is `/var/lib/telegram-bot-api-arms` so if you want to persist the server data you can mount this folder as volume:

`-v telegram-bot-api-arms-data:/etc/telegram/bot/api`

Note that all files in this directory will be owned by user `telegram-bot-api-arms` and group `telegram-bot-api-arms` (uid: `101`, gid: `101`, compatible with [nginx](https://hub.docker.com/_/nginx) image)

## Usage via docker stack deploy or docker-compose

```yaml
version: '3.7'

services:
  telegram-bot-api-arms:
    image: thecooldump/telegram-bot-api-arms:latest
    environment:
      TELEGRAM_API_ID: "<api-id>"
      TELEGRAM_API_HASH: "<api-hash>"
    volumes:
      - telegram-bot-api-arms-data:/var/lib/telegram-bot-api-arms
    ports:
      - "8081:8081"

volumes:
  telegram-bot-api-arms-data:
```
