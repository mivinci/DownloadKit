/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_bench_server_gobwas.go - WebSocket echo server using gobwas/ws
 *
 * Usage: go run ws_bench_server_gobwas.go [port]
 *   Default port: 9093
 *
 * Behavior: Accepts WebSocket connections and echoes messages.
 * Uses gobwas/ws for zero-allocation upgrade and low-level frame I/O.
 */

package main

import (
	"fmt"
	"io"
	"net"
	"os"

	"github.com/gobwas/ws"
	"github.com/gobwas/ws/wsutil"
)

func handleConn(conn net.Conn) {
	defer conn.Close()

	for {
		msg, op, err := wsutil.ReadClientData(conn)
		if err != nil {
			break
		}
		if err := wsutil.WriteServerMessage(conn, op, msg); err != nil {
			break
		}
	}
}

func main() {
	port := "9093"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	addr := "0.0.0.0:" + port
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Go WebSocket bench server (gobwas/ws) listening on ws://%s/\n", addr)
	fmt.Println("Behavior: echo all messages back to sender")

	upgrader := ws.Upgrader{}

	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go func() {
			_, err := upgrader.Upgrade(conn)
			if err != nil {
				conn.Close()
				return
			}
			handleConn(conn)
		}()
	}
}

// Suppress unused import warning
var _ = io.EOF
