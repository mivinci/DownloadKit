/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * wss_bench_server_nhooyr.go - WSS echo server using coder/websocket (nhooyr)
 *
 * Usage: go run wss_bench_server_nhooyr.go [port] [cert] [key]
 *   Default port: 9092
 *   Default cert: bench_cert.pem
 *   Default key:  bench_key.pem
 *
 * Behavior: Upgrades every HTTPS request to WebSocket and echoes messages.
 */

package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"os"

	"nhooyr.io/websocket"
)

func wsHandler(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		InsecureSkipVerify: true,
	})
	if err != nil {
		return
	}
	defer conn.CloseNow()

	ctx := r.Context()
	for {
		msgType, reader, err := conn.Reader(ctx)
		if err != nil {
			break
		}
		writer, err := conn.Writer(ctx, msgType)
		if err != nil {
			break
		}
		if _, err := io.Copy(writer, reader); err != nil {
			break
		}
		if err := writer.Close(); err != nil {
			break
		}
	}
}

func main() {
	port := "9092"
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
	mux.HandleFunc("/", wsHandler)

	addr := "0.0.0.0:" + port
	fmt.Printf("Go WSS bench server (coder/websocket) listening on wss://%s/\n", addr)
	fmt.Printf("TLS cert: %s\n", certFile)
	fmt.Printf("TLS key:  %s\n", keyFile)
	fmt.Println("Behavior: echo all messages back to sender")

	server := &http.Server{
		Addr:    addr,
		Handler: mux,
	}

	if err := server.ListenAndServeTLS(certFile, keyFile); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}

// Ensure context is used
var _ = context.Background
