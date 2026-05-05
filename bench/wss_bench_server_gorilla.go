/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * wss_bench_server_gorilla.go - WSS echo server using gorilla/websocket
 *
 * Usage: go run wss_bench_server_gorilla.go [port] [cert] [key]
 *   Default port: 9091
 *   Default cert: bench_cert.pem
 *   Default key:  bench_key.pem
 *
 * Behavior: Upgrades every HTTPS request to WebSocket and echoes messages.
 */

package main

import (
	"fmt"
	"net/http"
	"os"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
	CheckOrigin:     func(r *http.Request) bool { return true },
}

func wsHandler(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	for {
		msgType, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}
		if err := conn.WriteMessage(msgType, msg); err != nil {
			break
		}
	}
}

func main() {
	port := "9091"
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

	http.HandleFunc("/", wsHandler)

	addr := "0.0.0.0:" + port
	fmt.Printf("Go WSS bench server (gorilla/websocket) listening on wss://%s/\n", addr)
	fmt.Printf("TLS cert: %s\n", certFile)
	fmt.Printf("TLS key:  %s\n", keyFile)
	fmt.Println("Behavior: echo all messages back to sender")

	if err := http.ListenAndServeTLS(addr, certFile, keyFile, nil); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}
