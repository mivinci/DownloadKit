/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * wss_bench_server_gobwas.go - WSS echo server using gobwas/ws
 *
 * Usage: go run wss_bench_server_gobwas.go [port] [cert] [key]
 *   Default port: 9093
 *   Default cert: bench_cert.pem
 *   Default key:  bench_key.pem
 *
 * Behavior: Accepts WSS connections and echoes messages.
 * Uses gobwas/ws for zero-allocation upgrade and low-level frame I/O.
 */

package main

import (
	"crypto/tls"
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

	cert, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load TLS cert/key: %v\n", err)
		os.Exit(1)
	}

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{cert},
	}

	addr := "0.0.0.0:" + port
	ln, err := tls.Listen("tcp", addr, tlsConfig)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Go WSS bench server (gobwas/ws) listening on wss://%s/\n", addr)
	fmt.Printf("TLS cert: %s\n", certFile)
	fmt.Printf("TLS key:  %s\n", keyFile)
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
