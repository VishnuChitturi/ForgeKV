# 05 — HTTP Server

> **Stage:** 6  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## The Problem: Local-Only Access

Through Stage 5, ForgeKV is a library — a C++ object that can only be used by code running in the same process. There is no way for an external program, a different machine, or a web service to interact with it.

For ForgeKV to be genuinely useful as a storage engine, it needs to be accessible over a network. An HTTP server is the most practical choice for a first networking layer.

---

## Why HTTP?

HTTP is the most widely understood application protocol. Choosing it means:

- Clients can be written in any language (Python, JavaScript, Go, another C++ program)
- Tools like `curl` and Postman can be used to interact with ForgeKV directly
- The request/response model maps naturally to the key-value operations (one request, one response)
- No custom protocol or binary client is needed

HTTP is not the most efficient protocol for a storage engine — dedicated binary protocols (like Redis RESP or gRPC) are faster. But HTTP is the right choice at this stage: it is simple, observable, and universally understood.

If a binary protocol is added later, it is added alongside HTTP, not instead of it.

---

## Client/Server Architecture

After Stage 6, ForgeKV operates as a server:

```
┌─────────────────────────┐
│        Client           │
│  (curl, Python, Go...)  │
└────────────┬────────────┘
             │  HTTP Request
             │  (e.g. GET /get?key=name)
             ▼
┌─────────────────────────┐
│      HTTP Server        │
│   (ForgeKV listener)    │
│   port: configurable    │
└────────────┬────────────┘
             │  Calls
             ▼
┌─────────────────────────┐
│       KV Engine         │
│  (set / get / del / ...) │
└────────────┬────────────┘
             │  Returns result
             ▼
┌─────────────────────────┐
│      HTTP Server        │
│   Formats response      │
└────────────┬────────────┘
             │  HTTP Response
             │  (JSON body, status code)
             ▼
┌─────────────────────────┐
│        Client           │
│  Receives result        │
└─────────────────────────┘
```

---

## Planned REST Endpoints

The following endpoints are planned. Exact request and response schemas will be defined during implementation.

### `POST /set`

Store or update a key-value pair.

Conceptual request body:
```json
{
  "key": "name",
  "value": "Vishnu"
}
```

Conceptual success response:
```json
{ "status": "ok" }
```

### `GET /get`

Retrieve the value for a key.

Conceptual query: `GET /get?key=name`

Conceptual success response:
```json
{
  "key": "name",
  "value": "Vishnu"
}
```

Conceptual not-found response (HTTP 404):
```json
{ "error": "key not found" }
```

### `POST /delete`

Remove a key from the store.

Conceptual request body:
```json
{ "key": "name" }
```

Conceptual success response:
```json
{ "status": "ok" }
```

### `GET /exists`

Check whether a key is present.

Conceptual query: `GET /exists?key=name`

Conceptual response:
```json
{ "key": "name", "exists": true }
```

### `GET /health`

Confirm the server is running and the engine is operational.

Conceptual response:
```json
{ "status": "ok" }
```

### `GET /stats`

Return current operational statistics (added properly in Stage 11).

Conceptual response:
```json
{
  "key_count": 3,
  "uptime_seconds": 142
}
```

---

## HTTP Status Codes

| Scenario                     | HTTP Status |
|------------------------------|-------------|
| Success                      | 200 OK      |
| Key not found                | 404 Not Found |
| Bad or missing request fields | 400 Bad Request |
| Internal engine error        | 500 Internal Server Error |

The exact mapping will be finalized during implementation.

---

## Library Selection

The HTTP server implementation will use either:

- **cpp-httplib** — a single-header C++ HTTP library. Simple, well-maintained, no dependencies.
- **From-scratch** — a minimal TCP listener + HTTP/1.1 parser written in C++.

This decision will be made at implementation time. The from-scratch option is educationally valuable; cpp-httplib avoids reinventing HTTP parsing, which is not a learning objective for this project.

---

## Request/Response Flow

```
Client sends: POST /set  {"key":"name","value":"Vishnu"}
                    │
                    ▼
HTTP server receives request
Parses method, path, body
                    │
                    ▼
Dispatch: route "/set" → call engine.set("name", "Vishnu")
                    │
                    ▼
Engine writes WAL record (Stage 3–4)
Engine updates in-memory store (Stage 1)
                    │
                    ▼
HTTP server serializes response: {"status":"ok"}
                    │
                    ▼
HTTP 200 response sent to client
```

---

## Concurrency Note

At Stage 6, the HTTP server handles one request at a time, or uses the library's default threading model. Proper concurrent request handling is introduced in Stage 7. The two stages are designed to be layered: the HTTP API is defined in Stage 6, and thread safety is added on top in Stage 7 without changing the API.

---

*Previous: [04-crash-recovery.md](04-crash-recovery.md)*  
*Next: [06-concurrency.md](06-concurrency.md)*
