/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http_bench_server.go - Go net/http server for comparison benchmarking
 *
 * Usage: go run http_bench_server.go [port]
 *   Default port: 8080
 *
 * Routes:
 *   GET  /ping          → 200 "pong"  (minimal response)
 *   GET  /echo?size=N   → 200 with N bytes of 'x' (payload test)
 *   POST /echo          → 200 echoing the request body
 *
 * Designed to be benchmarked with wrk, ab, or hey:
 *   wrk -t4 -c100 -d10s http://127.0.0.1:8080/ping
 *   hey -n 100000 -c 50 http://127.0.0.1:8080/ping
 */

package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
)

func handlePing(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	w.Write([]byte("pong"))
}

func handleEcho(w http.ResponseWriter, r *http.Request) {
	if r.Method == "POST" {
		// Echo back the request body
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "read body failed", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Write(body)
		return
	}

	// GET: parse ?size=N from URL
	size := 64 // default
	if s := r.URL.Query().Get("size"); s != "" {
		if n, err := strconv.Atoi(s); err == nil && n > 0 {
			size = n
			if size > 1048576 { // cap at 1MB
				size = 1048576
			}
		}
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Write(bytes.Repeat([]byte("x"), size))
}

func main() {
	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/ping", handlePing)
	mux.HandleFunc("/echo", handleEcho)

	addr := "0.0.0.0:" + port
	fmt.Printf("Go HTTP bench server listening on %s\n", addr)
	fmt.Println("Routes:")
	fmt.Println("  GET  /ping        → 200 \"pong\"")
	fmt.Println("  GET  /echo?size=N → 200 with N bytes")
	fmt.Println("  POST /echo        → 200 echo body")

	if err := http.ListenAndServe(addr, mux); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}
