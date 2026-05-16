/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * https_bench_server.go - Go HTTPS server for comparison benchmarking
 *
 * Usage: go run https_bench_server.go [port] [cert] [key]
 *   Default port: 8444
 *   Default cert: bench_cert.pem
 *   Default key:  bench_key.pem
 *
 * Routes:
 *   GET  /ping          → 200 "pong"  (minimal response)
 *   GET  /echo?size=N   → 200 with N bytes of 'x' (payload test)
 *   POST /echo          → 200 echoing the request body
 *
 * Benchmark with wrk (HTTP/1.1 over TLS):
 *   wrk -t4 -c100 -d10s https://127.0.0.1:8444/ping
 *
 * Benchmark with h2load (HTTP/2 over TLS with ALPN):
 *   h2load -t4 -c100 -m10 -D 10 https://127.0.0.1:8444/ping
 */

package main

import (
	"bytes"
	"crypto/tls"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"

	"golang.org/x/net/http2"
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
	port := "8444"
	certFile := "bench_cert.pem"
	keyFile := "bench_key.pem"

	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	if len(os.Args) > 2 {
		certFile = os.Args[2]
	}
	if len(os.Args) > 3 {
		keyFile = os.Args[3]
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/ping", handlePing)
	mux.HandleFunc("/echo", handleEcho)

	// Load TLS certificate
	cert, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load TLS cert: %v\n", err)
		os.Exit(1)
	}

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{cert},
		NextProtos:   []string{"h2", "http/1.1"},
	}

	server := &http.Server{
		Addr:      "0.0.0.0:" + port,
		Handler:   mux,
		TLSConfig: tlsConfig,
	}

	// Configure HTTP/2
	http2.ConfigureServer(server, &http2.Server{})

	addr := "0.0.0.0:" + port
	fmt.Printf("Go HTTPS bench server listening on %s\n", addr)
	fmt.Println("Routes:")
	fmt.Println("  GET  /ping        → 200 \"pong\"")
	fmt.Println("  GET  /echo?size=N → 200 with N bytes")
	fmt.Println("  POST /echo        → 200 echo body")
	fmt.Printf("TLS cert: %s\n", certFile)
	fmt.Printf("TLS key:  %s\n", keyFile)
	fmt.Println("Protocol: HTTPS (TLS + ALPN h2/http1.1)")

	if err := server.ListenAndServeTLS(certFile, keyFile); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}
