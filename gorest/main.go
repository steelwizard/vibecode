package main

import (
	"encoding/json"
	"log"
	"net/http"
	"sync"
	"time"
)

type Todo struct {
	ID        int       `json:"id"`
	Title     string    `json:"title"`
	Completed bool      `json:"completed"`
	CreatedAt time.Time `json:"created_at"`
}

type createTodoRequest struct {
	Title string `json:"title"`
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

func main() {
	store := newTodoStore()
	mux := http.NewServeMux()

	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{
			"status": "ok",
		})
	})

	mux.HandleFunc("GET /todos", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, store.list())
	})

	mux.HandleFunc("POST /todos", func(w http.ResponseWriter, r *http.Request) {
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

	server := &http.Server{
		Addr:         ":8080",
		Handler:      mux,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  30 * time.Second,
	}

	log.Println("gorest listening on http://localhost:8080")
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("server failed: %v", err)
	}
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(payload); err != nil {
		http.Error(w, "failed to encode response", http.StatusInternalServerError)
	}
}
