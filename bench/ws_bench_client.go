/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_bench_client.go - WebSocket benchmark client
 *
 * Usage: go run ws_bench_client.go [options]
 *
 * Options:
 *   -url string       WebSocket server URL (default "ws://127.0.0.1:9090/")
 *   -c int            Number of concurrent connections (default 100)
 *   -d duration       Test duration (default 10s)
 *   -size int         Message payload size in bytes (default 64)
 *   -text             Use text messages instead of binary
 *
 * Each connection sends a message, waits for the echo, then sends the next.
 * Reports total messages, throughput (msg/s), and average latency.
 */

package main

import (
	"flag"
	"fmt"
	"math"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

func main() {
	url := flag.String("url", "ws://127.0.0.1:9090/", "WebSocket server URL")
	conns := flag.Int("c", 100, "Number of concurrent connections")
	duration := flag.Duration("d", 10*time.Second, "Test duration")
	msgSize := flag.Int("size", 64, "Message payload size in bytes")
	useText := flag.Bool("text", false, "Use text messages instead of binary")
	flag.Parse()

	// Prepare payload
	payload := make([]byte, *msgSize)
	for i := range payload {
		payload[i] = 'x'
	}

	msgType := websocket.BinaryMessage
	if *useText {
		msgType = websocket.TextMessage
	}

	fmt.Printf("WebSocket Benchmark Client\n")
	fmt.Printf("  URL:          %s\n", *url)
	fmt.Printf("  Connections:  %d\n", *conns)
	fmt.Printf("  Duration:     %s\n", *duration)
	fmt.Printf("  Message size: %d bytes\n", *msgSize)
	fmt.Printf("  Message type: %s\n", func() string {
		if *useText {
			return "text"
		}
		return "binary"
	}())
	fmt.Println()

	var totalMessages atomic.Int64
	var totalLatencyNs atomic.Int64
	var connErrors atomic.Int64
	var readyWg sync.WaitGroup
	var doneWg sync.WaitGroup

	deadline := time.Now().Add(*duration)
	startCh := make(chan struct{})

	dialer := websocket.Dialer{
		ReadBufferSize:  4096,
		WriteBufferSize: 4096,
	}

	readyWg.Add(*conns)
	doneWg.Add(*conns)

	for i := 0; i < *conns; i++ {
		go func(id int) {
			defer doneWg.Done()

			conn, _, err := dialer.Dial(*url, nil)
			if err != nil {
				connErrors.Add(1)
				readyWg.Done()
				return
			}
			defer conn.Close()

			readyWg.Done()
			<-startCh // Wait for all connections to be established

			recvBuf := make([]byte, *msgSize+128)
			_ = recvBuf

			for time.Now().Before(deadline) {
				start := time.Now()

				if err := conn.WriteMessage(msgType, payload); err != nil {
					break
				}
				_, _, err := conn.ReadMessage()
				if err != nil {
					break
				}

				latency := time.Since(start)
				totalMessages.Add(1)
				totalLatencyNs.Add(int64(latency))
			}
		}(i)
	}

	// Wait for all connections to be established
	fmt.Printf("Establishing %d connections...\n", *conns)
	readyWg.Wait()

	if errors := connErrors.Load(); errors > 0 {
		fmt.Printf("Warning: %d connections failed to establish\n", errors)
	}

	activeConns := int64(*conns) - connErrors.Load()
	fmt.Printf("%d connections established. Starting benchmark...\n\n", activeConns)

	startTime := time.Now()
	close(startCh) // Signal all goroutines to start

	doneWg.Wait()
	elapsed := time.Since(startTime)

	// Report results
	msgs := totalMessages.Load()
	avgLatencyNs := float64(0)
	if msgs > 0 {
		avgLatencyNs = float64(totalLatencyNs.Load()) / float64(msgs)
	}

	throughput := float64(msgs) / elapsed.Seconds()
	transferRate := throughput * float64(*msgSize) * 2 // send + recv

	fmt.Printf("Results:\n")
	fmt.Printf("  Duration:       %s\n", elapsed.Round(time.Millisecond))
	fmt.Printf("  Connections:    %d (active) / %d (total)\n", activeConns, *conns)
	fmt.Printf("  Messages:       %d\n", msgs)
	fmt.Printf("  Throughput:     %.0f msg/s\n", throughput)
	fmt.Printf("  Avg latency:    %s\n", formatDuration(avgLatencyNs))
	fmt.Printf("  Transfer rate:  %s/s (send+recv)\n", formatBytes(transferRate))

	if connErrors.Load() > 0 {
		fmt.Printf("  Conn errors:    %d\n", connErrors.Load())
	}

	os.Exit(0)
}

func formatDuration(ns float64) string {
	if ns < 1000 {
		return fmt.Sprintf("%.0f ns", ns)
	}
	us := ns / 1000
	if us < 1000 {
		return fmt.Sprintf("%.2f μs", us)
	}
	ms := us / 1000
	if ms < 1000 {
		return fmt.Sprintf("%.2f ms", ms)
	}
	return fmt.Sprintf("%.2f s", ms/1000)
}

func formatBytes(bytesPerSec float64) string {
	units := []string{"B", "KB", "MB", "GB"}
	idx := 0
	v := bytesPerSec
	for v >= 1024 && idx < len(units)-1 {
		v /= 1024
		idx++
	}
	if idx == 0 {
		return fmt.Sprintf("%.0f %s", v, units[idx])
	}
	return fmt.Sprintf("%.2f %s", v, units[idx])
}

// Suppress unused import
var _ = math.MaxFloat64
