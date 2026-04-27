package main

import (
	"embed"
	"encoding/json"
	"io/fs"
	"log"
	"net/http"
	"strconv"
	"sync"
	"time"
)

//go:embed frontend
var frontendFS embed.FS

type Todo struct {
	ID        int       `json:"id"`
	Title     string    `json:"title"`
	Completed bool      `json:"completed"`
	CreatedAt time.Time `json:"created_at"`
}

type createTodoRequest struct {
	Title string `json:"title"`
}

type patchTodoRequest struct {
	Completed *bool `json:"completed"`
}

type todoStore struct {
	mu     sync.Mutex
	nextID int
	items  []Todo
}

func newTodoStore() *todoStore {
	return &todoStore{
		nextID: 1,
		items:  make([]Todo, 0),
	}
}

func (s *todoStore) list() []Todo {
	s.mu.Lock()
	defer s.mu.Unlock()

	out := make([]Todo, len(s.items))
	copy(out, s.items)
	return out
}

func (s *todoStore) create(title string) Todo {
	s.mu.Lock()
	defer s.mu.Unlock()

	todo := Todo{
		ID:        s.nextID,
		Title:     title,
		Completed: false,
		CreatedAt: time.Now().UTC(),
	}

	s.nextID++
	s.items = append(s.items, todo)

	return todo
}

func (s *todoStore) updateCompleted(id int, completed bool) (Todo, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i := range s.items {
		if s.items[i].ID == id {
			s.items[i].Completed = completed
			return s.items[i], true
		}
	}
	return Todo{}, false
}

func (s *todoStore) deleteByID(id int) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i, item := range s.items {
		if item.ID == id {
			s.items = append(s.items[:i], s.items[i+1:]...)
			return true
		}
	}
	return false
}

func withCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Echo Origin so responses work with credentialed / private-network checks;
		// browsers may reject "*" for some cross-origin localhost requests.
		if o := r.Header.Get("Origin"); o != "" {
			w.Header().Set("Access-Control-Allow-Origin", o)
			w.Header().Add("Vary", "Origin")
			// Private Network Access: Chromium may require this on preflight and on the actual response
			// for cross-port localhost (e.g. UI :8090 → API :9080).
			w.Header().Set("Access-Control-Allow-Private-Network", "true")
		} else {
			w.Header().Set("Access-Control-Allow-Origin", "*")
		}
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS")
		if h := r.Header.Get("Access-Control-Request-Headers"); h != "" {
			w.Header().Set("Access-Control-Allow-Headers", h)
			w.Header().Add("Vary", "Access-Control-Request-Headers")
		} else {
			w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Accept")
		}
		if r.Header.Get("Access-Control-Request-Private-Network") == "true" {
			w.Header().Set("Access-Control-Allow-Private-Network", "true")
		}
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func main() {
	store := newTodoStore()
	apiMux := http.NewServeMux()

	apiMux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{
			"status": "ok",
		})
	})

	apiMux.HandleFunc("GET /todos", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, store.list())
	})

	apiMux.HandleFunc("POST /todos", func(w http.ResponseWriter, r *http.Request) {
		defer r.Body.Close()

		var req createTodoRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON body",
			})
			return
		}

		if req.Title == "" {
			writeJSON(w, http.StatusBadRequest, map[string]string{
				"error": "title is required",
			})
			return
		}

		writeJSON(w, http.StatusCreated, store.create(req.Title))
	})

	apiMux.HandleFunc("PATCH /todos/{id}", func(w http.ResponseWriter, r *http.Request) {
		defer r.Body.Close()

		id, err := strconv.Atoi(r.PathValue("id"))
		if err != nil || id < 1 {
			writeJSON(w, http.StatusBadRequest, map[string]string{
				"error": "invalid id",
			})
			return
		}

		var req patchTodoRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON body",
			})
			return
		}
		if req.Completed == nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{
				"error": "completed is required",
			})
			return
		}

		updated, ok := store.updateCompleted(id, *req.Completed)
		if !ok {
			writeJSON(w, http.StatusNotFound, map[string]string{
				"error": "todo not found",
			})
			return
		}
		writeJSON(w, http.StatusOK, updated)
	})

	apiMux.HandleFunc("DELETE /todos/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.Atoi(r.PathValue("id"))
		if err != nil || id < 1 {
			writeJSON(w, http.StatusBadRequest, map[string]string{
				"error": "invalid id",
			})
			return
		}
		if !store.deleteByID(id) {
			writeJSON(w, http.StatusNotFound, map[string]string{
				"error": "todo not found",
			})
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})

	apiServer := &http.Server{
		Addr:         ":9080",
		Handler:      withCORS(apiMux),
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  30 * time.Second,
	}

	web, err := fs.Sub(frontendFS, "frontend")
	if err != nil {
		log.Fatalf("frontend fs: %v", err)
	}
	webMux := http.NewServeMux()
	webMux.Handle("GET /", http.FileServer(http.FS(web)))
	webServer := &http.Server{
		Addr:         ":8090",
		Handler:      webMux,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  30 * time.Second,
	}

	go func() {
		log.Printf("gorest API listening on http://localhost%s\n", apiServer.Addr)
		if err := apiServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("API server failed: %v", err)
		}
	}()

	log.Printf("gorest web UI listening on http://localhost%s\n", webServer.Addr)
	if err := webServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("web server failed: %v", err)
	}
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		http.Error(w, "failed to encode response", http.StatusInternalServerError)
	}
}
