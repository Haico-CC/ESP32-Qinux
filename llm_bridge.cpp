#include "llm.h"
#include <Arduino.h>
#include <LittleFS.h>

static Transformer g_transformer;
static Tokenizer g_tokenizer;
static Sampler g_sampler;
static bool g_ready = false;
static bool g_busy = false;

bool llm_bridge_init(const char* model_path, const char* tok_path) {
  if (g_ready) return true;
  if (!LittleFS.exists(model_path)) {
    return false;
  }
  if (!LittleFS.exists(tok_path)) {
    return false;
  }

  memset(&g_transformer, 0, sizeof(Transformer));
  memset(&g_tokenizer, 0, sizeof(Tokenizer));
  memset(&g_sampler, 0, sizeof(Sampler));

  build_transformer(&g_transformer, model_path);
  if (!g_transformer.data) {
    return false;
  }

  build_tokenizer(&g_tokenizer, tok_path, g_transformer.config.vocab_size);
  if (!g_tokenizer.vocab) {
    return false;
  }

  build_sampler(&g_sampler, g_transformer.config.vocab_size, 0.8f, 0.9f, 1337);
  g_ready = true;
  return true;
}

bool llm_bridge_generate(const char* prompt, int max_steps, llm_token_cb on_token, generated_complete_cb on_done, void* user_data) {
  if (!g_ready) {
    return false;
  }
  if (g_busy) {
    return false;
  }
  g_busy = true;
  generate(&g_transformer, &g_tokenizer, &g_sampler, prompt, max_steps, on_token, user_data, on_done);
  g_busy = false;
  return true;
}

void llm_bridge_free() {
  if (!g_ready) return;
  free_transformer(&g_transformer);
  free_tokenizer(&g_tokenizer);
  free_sampler(&g_sampler);
  g_ready = false;
  g_busy = false;
}
bool llm_bridge_is_ready() {
  return g_ready;
}
bool llm_bridge_is_busy() {
  return g_busy;
}