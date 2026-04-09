/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_bench_server_gorilla.go - WebSocket echo server using gorilla/websocket
 *
 * Usage: go run ws_bench_server_gorilla.go [port]
 *   Default port: 9091
 *
 * Behavior: Upgrades every HTTP request to WebSocket and echoes messages.
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
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	http.HandleFunc("/", wsHandler)

	addr := "0.0.0.0:" + port
	fmt.Printf("Go WebSocket bench server (gorilla/websocket) listening on ws://%s/\n", addr)
	fmt.Println("Behavior: echo all messages back to sender")

	if err := http.ListenAndServe(addr, nil); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}
}
