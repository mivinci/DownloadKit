/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * h2c_bench_server.go - Go h2c (cleartext HTTP/2) server for comparison benchmarking
 *
 * Usage: go run h2c_bench_server.go [port]
 *   Default port: 8081
 *
 * Routes:
 *   GET  /ping          → 200 "pong"  (minimal response)
 *   GET  /echo?size=N   → 200 with N bytes of 'x' (payload test)
 *   POST /echo          → 200 echoing the request body
 *
 * This server supports cleartext HTTP/2 (h2c) via Prior Knowledge,
 * matching moo's h2c support for fair comparison.
 *
 * Benchmark with h2load:
 *   h2load -n 100000 -c 100 -t 4 http://127.0.0.1:8081/ping
 */

package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"

	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
)

func handlePing(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	w.Write([]byte("pong"))
}

func handleEcho(w http.ResponseWriter, r *http.Request) {
	if r.Method == "POST" {
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
	port := "8081"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/ping", handlePing)
	mux.HandleFunc("/echo", handleEcho)

	// Wrap with h2c handler to support cleartext HTTP/2 (Prior Knowledge)
	h2s := &http2.Server{}
	handler := h2c.NewHandler(mux, h2s)

	addr := "0.0.0.0:" + port
	fmt.Printf("Go h2c bench server listening on %s\n", addr)
	fmt.Println("Routes:")
	fmt.Println("  GET  /ping        → 200 \"pong\"")
	fmt.Println("  GET  /echo?size=N → 200 with N bytes")
	fmt.Println("  POST /echo        → 200 echo body")
	fmt.Println("Protocol: h2c (cleartext HTTP/2 via Prior Knowledge)")

	if err := http.ListenAndServe(addr, handler); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}
