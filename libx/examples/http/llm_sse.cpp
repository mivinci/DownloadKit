/*
 * llm_sse.cpp - Interactive REPL for streaming LLM chat via SSE
 *
 * Usage:
 *   export LLM_API_URL="https://api.openai.com"   # base URL (path
 * auto-appended) export LLM_API_KEY="sk-xxx" export LLM_MODEL="gpt-4o" #
 * optional, defaults to "gpt-4o"
 *   ./llm_sse
 *
 * Type a message and press Enter. The assistant's reply streams in real
 * time via Server-Sent Events. Press Ctrl-D or type "exit" to quit.
 */

#include <x/base/event.h>
#include <x/http/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ── Helpers ───────────────────────────────────────────────────────────── */

/**
 * Escape a string for embedding in a JSON string literal.
 * Handles: \ " \n \r \t
 */
static std::string json_escape(const char *s) {
  std::string out;
  for (; *s; ++s) {
    switch (*s) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += *s;
      break;
    }
  }
  return out;
}

/**
 * A simple chat message.
 */
struct ChatMessage {
  std::string role;
  std::string content;
};

/**
 * Build an OpenAI-compatible chat completion request body
 * from the full conversation history.
 */
static std::string build_request_body(const char *model, const std::vector<ChatMessage> &messages) {
  std::string body;
  body += "{\"model\":\"";
  body += json_escape(model);
  body += "\",\"stream\":true,\"messages\":[";
  for (size_t i = 0; i < messages.size(); i++) {
    if (i > 0) body += ',';
    body += "{\"role\":\"";
    body += json_escape(messages[i].role.c_str());
    body += "\",\"content\":\"";
    body += json_escape(messages[i].content.c_str());
    body += "\"}";
  }
  body += "]}";
  return body;
}

/**
 * Extract the value of a JSON string field from a flat JSON object.
 * Very minimal — works for simple cases like {"content":"hello"}.
 * Returns empty string if not found.
 */
static std::string json_extract_string(const char *json, const char *key) {
  /* Build the search pattern: "key":" */
  std::string pattern = std::string("\"") + key + "\":\"";
  const char *start   = strstr(json, pattern.c_str());
  if (!start) return "";

  start += pattern.size();
  std::string result;
  for (const char *p = start; *p && *p != '"'; ++p) {
    if (*p == '\\' && *(p + 1)) {
      ++p;
      switch (*p) {
      case 'n':
        result += '\n';
        break;
      case 'r':
        result += '\r';
        break;
      case 't':
        result += '\t';
        break;
      case '\\':
        result += '\\';
        break;
      case '"':
        result += '"';
        break;
      default:
        result += '\\';
        result += *p;
        break;
      }
    } else {
      result += *p;
    }
  }
  return result;
}

/* ── REPL state ────────────────────────────────────────────────────────── */

struct ReplCtx {
  xEventLoop  loop;
  bool        done;     /* current SSE stream finished */
  bool        got_done; /* received [DONE] from server */
  std::string reply;    /* accumulated assistant reply  */
};

/* ── SSE callbacks ─────────────────────────────────────────────────────── */

static int on_sse_event(const xSseEvent *ev, void *arg) {
  ReplCtx *ctx = static_cast<ReplCtx *>(arg);

  /* OpenAI signals end-of-stream with data: [DONE] */
  if (ev->data && strcmp(ev->data, "[DONE]") == 0) {
    ctx->got_done = true;
    return 1; /* close connection */
  }

  if (!ev->data) return 0;

  /* Extract delta content from the SSE data JSON */
  std::string content = json_extract_string(ev->data, "content");
  if (!content.empty()) {
    /* Print a blank line before the first chunk for readability */
    if (ctx->reply.empty()) putchar('\n');
    ctx->reply += content;
    fputs(content.c_str(), stdout);
    fflush(stdout);
  }

  return 0;
}

static void on_sse_done(int curl_code, void *arg) {
  ReplCtx *ctx = static_cast<ReplCtx *>(arg);

  if (curl_code != 0 && !ctx->got_done) {
    fprintf(stderr, "\n[error] stream ended with curl code %d\n", curl_code);
  }

  /* Print a blank line after the streamed response for readability */
  putchar('\n');
  putchar('\n');
  fflush(stdout);

  ctx->done = true;
  xEventLoopStop(ctx->loop);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main() {
  const char *api_url = getenv("LLM_API_URL");
  const char *api_key = getenv("LLM_API_KEY");
  const char *model   = getenv("LLM_MODEL");

  if (!api_url || !api_key) {
    fprintf(stderr, "Please set environment variables:\n"
                    "  export LLM_API_URL=\"https://api.openai.com\"\n"
                    "  export LLM_API_KEY=\"sk-xxx\"\n"
                    "  export LLM_MODEL=\"gpt-4o\"  (optional)\n");
    return 1;
  }
  if (!model || model[0] == '\0') model = "gpt-4o";

  /* Initialise event loop and HTTP client */
  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xHttpClient client = xHttpClientCreate(loop, nullptr);
  if (!client) {
    fprintf(stderr, "Failed to create HTTP client\n");
    xEventLoopDestroy(loop);
    return 1;
  }

  /* Build full URL: append /v1/chat/completions if not already present */
  std::string full_url = api_url;
  if (full_url.find("/v1/chat/completions") == std::string::npos) {
    /* Strip trailing slash */
    while (!full_url.empty() && full_url.back() == '/')
      full_url.pop_back();
    full_url += "/v1/chat/completions";
  }

  /* Build Authorization header once */
  std::string auth_header = std::string("Authorization: Bearer ") + api_key;
  const char *headers[]   = {auth_header.c_str(), "Content-Type: application/json", nullptr};

  ReplCtx ctx;
  ctx.loop = loop;

  std::vector<ChatMessage> messages; /* conversation history */
  char                     line[4096];

  printf("LLM SSE REPL (model: %s)\n", model);
  printf("Type a message and press Enter. Ctrl-C or \"exit\" to quit.\n\n");

  while (true) {
    printf("> ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) break; /* EOF (Ctrl-D) */

    /* Strip trailing newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if (len == 0) continue;
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

    /* Append user message to history */
    messages.push_back({"user", line});

    /* Build request with full conversation history */
    std::string body = build_request_body(model, messages);

    xHttpRequestConf config;
    memset(&config, 0, sizeof(config));
    config.url      = full_url.c_str();
    config.method   = xHttpMethod_POST;
    config.body     = body.c_str();
    config.body_len = body.size();
    config.headers  = headers;

    ctx.done     = false;
    ctx.got_done = false;
    ctx.reply.clear();

    xErrno err = xHttpClientDoSse(client, &config, on_sse_event, on_sse_done, &ctx);
    if (err != xErrno_Ok) {
      fprintf(stderr, "[error] failed to send request (errno=%d)\n", err);
      messages.pop_back(); /* remove the failed user message */
      continue;
    }

    /* Drive the event loop until the SSE stream completes */
    xEventLoopRun(loop);

    /* Append assistant reply to history */
    if (!ctx.reply.empty()) messages.push_back({"assistant", std::move(ctx.reply)});
  }

  printf("\nBye!\n");

  xHttpClientDestroy(client);
  xEventLoopDestroy(loop);
  return 0;
}
