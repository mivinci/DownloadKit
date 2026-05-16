/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_echo_client.cpp - TCP echo client for end-to-end benchmarking
 *
 * Usage: tcp_echo_client [host] [port] [msg_size] [num_messages] [concurrency]
 *   Defaults: host=127.0.0.1 port=9000 msg_size=128 num_messages=100000
 *             concurrency=1
 *
 * Opens `concurrency` connections to the echo server, sends `num_messages`
 * messages of `msg_size` bytes each, and reports throughput and latency.
 */

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <x/base/time.h>
}

static double run_client(const char *host, uint16_t port, size_t msg_size,
                         int64_t num_messages) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host, &addr.sin_addr);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }

  std::vector<char> send_buf(msg_size, 'A');
  std::vector<char> recv_buf(msg_size);

  uint64_t start = xMonoMs();

  for (int64_t i = 0; i < num_messages; i++) {
    // Send
    ssize_t total_sent = 0;
    while (total_sent < (ssize_t)msg_size) {
      ssize_t n =
          write(fd, send_buf.data() + total_sent, msg_size - total_sent);
      if (n <= 0) {
        if (errno == EINTR) continue;
        perror("write");
        close(fd);
        return -1;
      }
      total_sent += n;
    }

    // Receive echo
    ssize_t total_recv = 0;
    while (total_recv < (ssize_t)msg_size) {
      ssize_t n =
          read(fd, recv_buf.data() + total_recv, msg_size - total_recv);
      if (n <= 0) {
        if (errno == EINTR) continue;
        perror("read");
        close(fd);
        return -1;
      }
      total_recv += n;
    }
  }

  uint64_t end = xMonoMs();
  close(fd);

  return (double)(end - start);
}

int main(int argc, char *argv[]) {
  const char *host = "127.0.0.1";
  uint16_t port = 9000;
  size_t msg_size = 128;
  int64_t num_messages = 100000;
  int concurrency = 1;

  if (argc > 1) host = argv[1];
  if (argc > 2) port = (uint16_t)atoi(argv[2]);
  if (argc > 3) msg_size = (size_t)atoi(argv[3]);
  if (argc > 4) num_messages = atoll(argv[4]);
  if (argc > 5) concurrency = atoi(argv[5]);

  fprintf(stdout,
          "TCP echo benchmark: host=%s port=%u msg_size=%zu "
          "num_messages=%lld concurrency=%d\n",
          host, port, msg_size, (long long)num_messages, concurrency);

  // Split messages across concurrent connections
  int64_t msgs_per_conn = num_messages / concurrency;

  std::vector<std::thread> threads;
  std::vector<double> durations(concurrency);

  uint64_t wall_start = xMonoMs();

  for (int c = 0; c < concurrency; c++) {
    threads.emplace_back([&, c]() {
      durations[c] = run_client(host, port, msg_size, msgs_per_conn);
    });
  }

  for (auto &t : threads) t.join();

  uint64_t wall_end = xMonoMs();
  double wall_ms = (double)(wall_end - wall_start);

  // Check for errors
  for (int c = 0; c < concurrency; c++) {
    if (durations[c] < 0) {
      fprintf(stderr, "Client %d failed\n", c);
      return 1;
    }
  }

  int64_t total_msgs = msgs_per_conn * concurrency;
  double total_bytes = (double)total_msgs * (double)msg_size * 2.0; // send+recv
  double throughput_msgs = (double)total_msgs / (wall_ms / 1000.0);
  double throughput_mb = total_bytes / (wall_ms / 1000.0) / (1024.0 * 1024.0);
  double avg_rtt_us = wall_ms * 1000.0 / (double)total_msgs;

  fprintf(stdout, "\n=== Results ===\n");
  fprintf(stdout, "Wall time:       %.2f ms\n", wall_ms);
  fprintf(stdout, "Total messages:  %lld\n", (long long)total_msgs);
  fprintf(stdout, "Throughput:      %.0f msg/s\n", throughput_msgs);
  fprintf(stdout, "Throughput:      %.2f MB/s\n", throughput_mb);
  fprintf(stdout, "Avg RTT:         %.2f us/msg\n", avg_rtt_us);

  return 0;
}
