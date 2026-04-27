# gorest

A minimal HTTP JSON API in Go for managing in-memory todos. It uses the standard library only (`net/http`, `encoding/json`, `sync`) and stores data in process memory (no database).

## Requirements

- [Go](https://go.dev/dl/) 1.22 or newer

## Run

```bash
cd gorest
go run .
```

Two HTTP servers start in one process:

| Port | Role |
|------|------|
| **8090** | Web UI (static `frontend/`) |
| **9080** | JSON API (`/health`, `/todos`) |

Logs look like:

```text
gorest API listening on http://localhost:9080
gorest web UI listening on http://localhost:8090
```

Open **http://localhost:8090/** for the UI. The UI calls the API on port 9080; the API sends CORS headers so the browser allows cross-origin requests. Use curl against **http://localhost:9080** for scripting.

## Configuration

There is no external configuration file. Timeouts are fixed in code:

- Read: 10s  
- Write: 10s  
- Idle: 30s  

## API

All successful JSON responses use `Content-Type: application/json`.

### `GET /health`

Liveness check.

**Response** `200 OK`:

```json
{ "status": "ok" }
```

### `GET /todos`

Returns every todo, newest appended order (slice order).

**Response** `200 OK`: JSON array of todo objects:

```json
[
  {
    "id": 1,
    "title": "Buy milk",
    "completed": false,
    "created_at": "2026-04-21T12:00:00Z"
  }
]
```

Fields:

| Field        | Type    | Description                          |
|-------------|---------|--------------------------------------|
| `id`        | integer | Monotonic ID assigned by the server  |
| `title`     | string  | Todo title                           |
| `completed` | boolean | Always `false` on create (not updated by API) |
| `created_at`| string  | RFC3339 UTC timestamp                |

### `POST /todos`

Creates a todo.

**Request body** (JSON):

```json
{ "title": "My task" }
```

**Responses**

- `201 Created` — body is the new todo object (same shape as array elements above).
- `400 Bad Request` — invalid JSON or empty `title`:

```json
{ "error": "invalid JSON body" }
```

or

```json
{ "error": "title is required" }
```

**Example**

```bash
curl -s -X POST http://localhost:9080/todos \
  -H 'Content-Type: application/json' \
  -d '{"title":"Learn Go"}'
```

### `DELETE /todos/{id}`

Removes the todo with the given numeric `id`.

**Responses**

- `204 No Content` — todo was deleted.
- `400 Bad Request` — `id` is not a positive integer: `{ "error": "invalid id" }`
- `404 Not Found` — no todo with that id: `{ "error": "todo not found" }`

**Example**

```bash
curl -s -o /dev/null -w "%{http_code}" -X DELETE http://localhost:9080/todos/1
```

## Implementation notes

- **Concurrency**: A `sync.Mutex` protects the in-memory store for list, create, and delete.
- **Persistence**: Data is lost when the process exits. There is no update endpoint.
- **Routing**: Uses Go 1.22+ `ServeMux` patterns (`"GET /path"`, `"POST /path"`, `"DELETE /todos/{id}"`).

## Web UI

Static files live in `frontend/` and are **embedded** and served on **port 8090**. The UI lists todos, adds new ones via `POST` to `http://localhost:9080/todos`, removes them with `DELETE`, and checks `http://localhost:9080/health` for a small status pill (`API_BASE` in `app.js` must match where the API listens).

## Project layout

- `main.go` — API on `:9080` (with CORS), web on `:8090`, embedded `frontend/`
- `frontend/` — `index.html`, `styles.css`, `app.js`
- `go.mod` — module `gorest`, Go 1.22
