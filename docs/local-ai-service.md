# Local AI service

`components/local_ai_service` replaces the Gemini backend. The recording and summary
services retain their asynchronous task/event lifecycle. Audio is sent directly to
the configured transcription server; text prompts are sent directly to Ollama.

## Setup

Connect the ESP32 to the same LAN as the servers using the device setup portal.
Under **Local AI**, configure:

| Setting | Example |
| --- | --- |
| Transcription endpoint | `https://your-diction-host/v1/audio/transcriptions` |
| Transcription model | `parakeet-v3` |
| Pairing token | A token from `docker exec diction-gateway gateway auth` |
| Ollama server | `https://your-diction-host/ollama` |
| Ollama model | `llama3.1:8b-instruct-q4_K_M` |

For roaming, use public HTTPS addresses. This deployment proxies only Ollama
`/api/tags` and `/api/generate` through Diction at `/ollama`, validates the Diction
pairing token before forwarding, and does not expose Ollama administration. The
firmware reuses the pairing token only when both URLs have the same origin.
A direct LAN Ollama address still works without forwarding the Diction token.
The selected model must already be installed. mDNS queries support `.local` names. A compatible
Whisper server can be used by changing the transcription URL/model and clearing the
Diction token if that server requires no authentication.

An empty token field preserves the saved token; **Clear token** explicitly removes
it. The portal never returns the token. NVS settings override build defaults,
including an explicitly cleared token. Device-specific build values belong in the
ignored `sdkconfig`, never in `sdkconfig.defaults`.

## Server requests

- Transcription: `POST /v1/audio/transcriptions`, multipart fields `model`,
  `response_format=json`, and `file=recording.wav`. WAV data is 16-bit mono PCM at
  the recording's sample rate, streamed from the clip's PSRAM chunks through
  bounded writes. The response is `{ "text": "..." }`.
- Generation: Ollama `POST /api/generate` with `stream=false`, a configured model,
  prompt, 8192-token context, and 1024-token output allowance. The response's
  `response` field supplies the summary.
- Readiness: transcription `GET /v1/models`; Diction also verifies the token with
  `GET /v1/auth/key`. Ollama `GET /api/tags` verifies the installed model.

HTTPS uses ESP-IDF's certificate bundle and PSRAM-backed TLS allocations. The initial
server check waits up to 15 seconds for SNTP on a cold boot, and rejects HTTPS
requests until the clock is set. Certificate verification remains enabled. Redirects are disabled so bearer tokens
are not forwarded to another endpoint. Responses are limited to 64 KiB. Requests
have a 180-second timeout to allow cold local model loads; readiness probes use
15 seconds per request. Only one readiness worker runs at a time, including across
Wi-Fi reconnects or settings changes. Audio and text readiness are independent.

Summary input uses UTF-8 byte length as a conservative token upper bound, with
6000-token input/rollup and 4000-token chunk budgets to fit the local context.

## Device portal API

- `GET /api/settings/ai`: settings and readiness, with token presence only.
- `POST /api/settings/ai`: update any of `transcription_url`,
  `transcription_model`, `api_key`, `ollama_url`, `model_name`.
- `GET /api/runtime/ai`: readiness and the last server error.
- `POST /api/settings/ai/check`: start a server readiness check.

## USB diagnostics

The USB Serial/JTAG console provides `help`, `status`, `ai-check`, `ai-test`,
`capture-test`, `wifi-join`, `sd-format` and `log`. `ai-test` sends a short Ollama
prompt. `capture-test` records five seconds, saves the clip, transcribes it and
saves its transcript in the archive. It should be run only when no user recording
is active. `sd-format ERASE` explicitly erases the mounted SD filesystem; it does
not repartition a card. Entering USB mass-storage mode temporarily disconnects
the serial console because both modes share the board's USB connection.
