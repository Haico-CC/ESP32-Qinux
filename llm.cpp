#include "llm.h"
#include <LittleFS.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LLM_ALLOC(type, count) static_cast<type *>(heap_caps_aligned_calloc(16, count, sizeof(type), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))

// ========== 状态分配 ==========
void malloc_run_state(RunState *s, Config *p) {
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  s->x = LLM_ALLOC(float, p->dim);
  s->xb = LLM_ALLOC(float, p->dim);
  s->xb2 = LLM_ALLOC(float, p->dim);
  s->hb = LLM_ALLOC(float, p->hidden_dim);
  s->hb2 = LLM_ALLOC(float, p->hidden_dim);
  s->q = LLM_ALLOC(float, p->dim);
  s->key_cache = LLM_ALLOC(float, p->n_layers * p->seq_len * kv_dim);
  s->value_cache = LLM_ALLOC(float, p->n_layers * p->seq_len * kv_dim);
  s->att = LLM_ALLOC(float, p->n_heads * p->seq_len);
  s->logits = LLM_ALLOC(float, p->vocab_size);
  
  if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q || 
      !s->key_cache || !s->value_cache || !s->att || !s->logits) {
    ESP_LOGE("LLM", "State allocation failed (PSRAM OOM)");
  }
}

void free_run_state(RunState *s) {
  free(s->x); free(s->xb); free(s->xb2); free(s->hb); free(s->hb2);
  free(s->q); free(s->att); free(s->logits);
  free(s->key_cache); free(s->value_cache);
}

// ========== 权重映射 ==========
void memory_map_weights(TransformerWeights *w, Config *p, float *ptr, int shared_weights) {
  int head_size = p->dim / p->n_heads;
  unsigned long long n_layers = p->n_layers;
  
  w->token_embedding_table = ptr; ptr += p->vocab_size * p->dim;
  w->rms_att_weight = ptr; ptr += n_layers * p->dim;
  w->wq = ptr; ptr += n_layers * p->dim * (p->n_heads * head_size);
  w->wk = ptr; ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
  w->wv = ptr; ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
  w->wo = ptr; ptr += n_layers * (p->n_heads * head_size) * p->dim;
  w->rms_ffn_weight = ptr; ptr += n_layers * p->dim;
  w->w1 = ptr; ptr += n_layers * p->dim * p->hidden_dim;
  w->w2 = ptr; ptr += n_layers * p->hidden_dim * p->dim;
  w->w3 = ptr; ptr += n_layers * p->dim * p->hidden_dim;
  w->rms_final_weight = ptr; ptr += p->dim;
  
  // 跳过 RoPE 频率表
  ptr += p->seq_len * head_size / 2;
  ptr += p->seq_len * head_size / 2;
  
  w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

// ========== 加载 Checkpoint ==========
void read_checkpoint(const char *checkpoint, Config *config, TransformerWeights *weights, 
                     int *fd, float **data, size_t *file_size) {
  String path = String(checkpoint);
  if (!path.startsWith("/")) path = "/" + path;

  File file = LittleFS.open(path.c_str(), "r");
  if (!file) return;

  *file_size = file.size();

  *data = LLM_ALLOC(float, *file_size / sizeof(float) + 1);
  if (!*data) {
    file.close();
    return;
  }

  size_t bytes_read = 0;
  uint8_t *dst = reinterpret_cast<uint8_t *>(*data);
  while (bytes_read < *file_size) {
    size_t chunk = file.read(dst + bytes_read, min((size_t)8192, *file_size - bytes_read));
    if (chunk == 0) break;
    bytes_read += chunk;
  }
  file.close();

  if (bytes_read != *file_size) {
    free(*data);
    *data = nullptr;
    return;
  }

  memcpy(config, *data, sizeof(Config));
  int shared_weights = config->vocab_size > 0 ? 1 : 0;
  config->vocab_size = abs(config->vocab_size);

  float *weights_ptr = *data + sizeof(Config) / sizeof(float);
  memory_map_weights(weights, config, weights_ptr, shared_weights);
}

void build_transformer(Transformer *t, const char *checkpoint_path) {
  read_checkpoint(checkpoint_path, &t->config, &t->weights, &t->fd, &t->data, &t->file_size);
  if (!t->data) return;
  malloc_run_state(&t->state, &t->config);
}

void free_transformer(Transformer *t) {
  if (t->data) { free(t->data); t->data = nullptr; }
  free_run_state(&t->state);
}

// ========== 基础运算 ==========
void rmsnorm(float *o, float *x, float *weight, int size) {
  float ss = 0.0f;
  for (int j = 0; j < size; j++) ss += x[j] * x[j];
  ss /= size; ss += 1e-5f; ss = 1.0f / sqrtf(ss);
  for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
}

void softmax(float *x, int size) {
  float max_val = x[0];
  for (int i = 1; i < size; i++) if (x[i] > max_val) max_val = x[i];
  float sum = 0.0f;
  for (int i = 0; i < size; i++) { x[i] = expf(x[i] - max_val); sum += x[i]; }
  if (sum < 1e-9f) sum = 1e-9f;
  for (int i = 0; i < size; i++) x[i] /= sum;
}

void matmul(float *xout, float *x, float *weight, int n, int d) {
  for (int i = 0; i < d; i++) {
    float val = 0.0f;
    for (int j = 0; j < n; j++) val += x[j] * weight[i * n + j];
    xout[i] = val;
  }
}

// ========== Transformer 前向传播 ==========
float *forward(Transformer *transformer, int token, int pos) {
  Config *p = &transformer->config;
  TransformerWeights *w = &transformer->weights;
  RunState *s = &transformer->state;
  float *x = s->x;
  int dim = p->dim;
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  int kv_mul = p->n_heads / p->n_kv_heads;
  int hidden_dim = p->hidden_dim;
  int head_size = dim / p->n_heads;

  memcpy(x, w->token_embedding_table + token * dim, dim * sizeof(float));

  for (unsigned long long l = 0; l < p->n_layers; l++) {
    rmsnorm(s->xb, x, w->rms_att_weight + l * dim, dim);
    int loff = l * p->seq_len * kv_dim;
    s->k = s->key_cache + loff + pos * kv_dim;
    s->v = s->value_cache + loff + pos * kv_dim;

    matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
    matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
    matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);

    // RoPE 位置编码
    for (int i = 0; i < dim; i += 2) {
      int head_dim = i % head_size;
      float freq = 1.0f / powf(10000.0f, (float)head_dim / (float)head_size);
      float val = pos * freq;
      float fcr = cosf(val), fci = sinf(val);
      int rotn = i < kv_dim ? 2 : 1;
      for (int v = 0; v < rotn; v++) {
        float *vec = v == 0 ? s->q : s->k;
        float v0 = vec[i], v1 = vec[i + 1];
        vec[i] = v0 * fcr - v1 * fci;
        vec[i + 1] = v0 * fci + v1 * fcr;
      }
    }

    // 多头注意力
    for (int h = 0; h < p->n_heads; h++) {
      float *q = s->q + h * head_size;
      float *att = s->att + h * p->seq_len;
      for (int t = 0; t <= pos; t++) {
        float *k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        float score = 0.0f;
        for (int i = 0; i < head_size; i++) score += q[i] * k[i];
        att[t] = score / sqrtf(head_size);
      }
      softmax(att, pos + 1);
      float *xb = s->xb + h * head_size;
      memset(xb, 0, head_size * sizeof(float));
      for (int t = 0; t <= pos; t++) {
        float *v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        float a = att[t];
        for (int i = 0; i < head_size; i++) xb[i] += a * v[i];
      }
    }

    matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);
    for (int i = 0; i < dim; i++) x[i] += s->xb2[i];

    // FFN
    rmsnorm(s->xb, x, w->rms_ffn_weight + l * dim, dim);
    matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
    matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);
    for (int i = 0; i < hidden_dim; i++) {
      float val = s->hb[i];
      val *= (1.0f / (1.0f + expf(-val)));  // SiLU
      val *= s->hb2[i];
      s->hb[i] = val;
    }
    matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);
    for (int i = 0; i < dim; i++) x[i] += s->xb[i];
  }

  rmsnorm(x, x, w->rms_final_weight, dim);
  matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
  return s->logits;
}

// ========== 分词器 ==========
int compare_tokens(const void *a, const void *b) {
  return strcmp(static_cast<const TokenIndex *>(a)->str, static_cast<const TokenIndex *>(b)->str);
}

void build_tokenizer(Tokenizer *t, const char *tokenizer_path, int vocab_size) {
  t->vocab_size = vocab_size;
  t->vocab = LLM_ALLOC(char *, vocab_size);
  t->vocab_scores = LLM_ALLOC(float, vocab_size);
  t->sorted_vocab = nullptr;
  
  for (int i = 0; i < 256; i++) {
    t->byte_pieces[i * 2] = (unsigned char)i;
    t->byte_pieces[i * 2 + 1] = '\0';
  }

  String path = String(tokenizer_path);
  if (!path.startsWith("/")) path = "/" + path;

  File file = LittleFS.open(path, "r");
  if (!file) return;

  file.read((uint8_t *)&t->max_token_length, sizeof(int));

  int loaded_count = 0;
  for (int i = 0; i < vocab_size; i++) {
    file.read((uint8_t *)(t->vocab_scores + i), sizeof(float));
    int len = 0;
    file.read((uint8_t *)&len, sizeof(int));
    t->vocab[i] = LLM_ALLOC(char, len + 1);
    file.read((uint8_t *)t->vocab[i], len);
    t->vocab[i][len] = '\0';
    loaded_count++;
  }
  file.close();
}

void free_tokenizer(Tokenizer *t) {
  for (int i = 0; i < t->vocab_size; i++) free(t->vocab[i]);
  free(t->vocab); free(t->vocab_scores); free(t->sorted_vocab);
}

const char *decode(Tokenizer *t, int prev_token, int token) {
  if (token < 0 || token >= t->vocab_size || !t->vocab[token]) return "<unk>";
  char *piece = t->vocab[token];
  if (prev_token == 1 && piece[0] == ' ') piece++;
  unsigned char byte_val;
  if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1) 
    piece = (char *)t->byte_pieces + byte_val * 2;
  return piece;
}

int str_lookup(char *str, TokenIndex *sorted_vocab, int vocab_size) {
  TokenIndex tok = { str, 0 };
  TokenIndex *res = (TokenIndex *)bsearch(&tok, sorted_vocab, vocab_size, sizeof(TokenIndex), compare_tokens);
  return res ? res->id : -1;
}

void encode(Tokenizer *t, char *text, int8_t bos, int8_t eos, int *tokens, int *n_tokens) {
  if (!text) return;
  if (!t->sorted_vocab) {
    t->sorted_vocab = LLM_ALLOC(TokenIndex, t->vocab_size);
    for (int i = 0; i < t->vocab_size; i++) {
      t->sorted_vocab[i].str = t->vocab[i];
      t->sorted_vocab[i].id = i;
    }
    qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
  }
  char *str_buffer = LLM_ALLOC(char, t->max_token_length * 2 + 4);
  size_t str_len = 0;
  *n_tokens = 0;
  if (bos) tokens[(*n_tokens)++] = 1;
  if (text[0] != '\0') {
    int dummy = str_lookup((char *)" ", t->sorted_vocab, t->vocab_size);
    tokens[(*n_tokens)++] = dummy < 0 ? 0 : dummy;
  }
  for (char *c = text; *c != '\0'; c++) {
    if ((*c & 0xC0) != 0x80) str_len = 0;
    str_buffer[str_len++] = *c;
    str_buffer[str_len] = '\0';
    if ((*(c + 1) & 0xC0) == 0x80 && str_len < 4) continue;
    int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
    if (id != -1) tokens[(*n_tokens)++] = id;
    else
      for (int i = 0; i < str_len; i++) tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
    str_len = 0;
  }
  free(str_buffer);
  if (eos) tokens[(*n_tokens)++] = 2;
}

// ========== 采样器 ==========
int sample_argmax(float *probabilities, int n) {
  int max_i = 0; float max_p = probabilities[0];
  for (int i = 1; i < n; i++) if (probabilities[i] > max_p) { max_i = i; max_p = probabilities[i]; }
  return max_i;
}

int sample_mult(float *probabilities, int n, float coin) {
  float cdf = 0.0f;
  for (int i = 0; i < n; i++) { cdf += probabilities[i]; if (coin < cdf) return i; }
  return n - 1;
}

int compare_prob(const void *a, const void *b) {
  const ProbIndex *a_ = (const ProbIndex *)a, *b_ = (const ProbIndex *)b;
  return (a_->prob > b_->prob) ? -1 : (a_->prob < b_->prob) ? 1 : 0;
}

int sample_topp(float *probabilities, int n, float topp, ProbIndex *probindex, float coin) {
  int n0 = 0;
  const float cutoff = (1.0f - topp) / (n - 1);
  for (int i = 0; i < n; i++)
    if (probabilities[i] >= cutoff) { probindex[n0].index = i; probindex[n0].prob = probabilities[i]; n0++; }
  qsort(probindex, n0, sizeof(ProbIndex), compare_prob);
  float cumulative_prob = 0.0f;
  int last_idx = n0 - 1;
  for (int i = 0; i < n0; i++) {
    cumulative_prob += probindex[i].prob;
    if (cumulative_prob > topp) { last_idx = i; break; }
  }
  float r = coin * cumulative_prob;
  float cdf = 0.0f;
  for (int i = 0; i <= last_idx; i++) {
    cdf += probindex[i].prob;
    if (r < cdf) return probindex[i].index;
  }
  return probindex[last_idx].index;
}

void build_sampler(Sampler *sampler, int vocab_size, float temperature, float topp, unsigned long long rng_seed) {
  sampler->vocab_size = vocab_size;
  sampler->temperature = temperature;
  sampler->topp = topp;
  sampler->rng_state = rng_seed;
  sampler->probindex = LLM_ALLOC(ProbIndex, vocab_size);
}

void free_sampler(Sampler *sampler) { free(sampler->probindex); }

unsigned int random_u32(unsigned long long *state) {
  *state ^= *state >> 12; *state ^= *state << 25; *state ^= *state >> 27;
  return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}

float random_f32(unsigned long long *state) { return (random_u32(state) >> 8) / 16777216.0f; }

int sample(Sampler *sampler, float *logits) {
  int next;
  if (sampler->temperature == 0.0f) 
    next = sample_argmax(logits, sampler->vocab_size);
  else {
    for (int q = 0; q < sampler->vocab_size; q++) logits[q] /= sampler->temperature;
    softmax(logits, sampler->vocab_size);
    float coin = random_f32(&sampler->rng_state);
    next = (sampler->topp <= 0 || sampler->topp >= 1) 
           ? sample_mult(logits, sampler->vocab_size, coin) 
           : sample_topp(logits, sampler->vocab_size, sampler->topp, sampler->probindex, coin);
  }
  return next;
}

long time_in_ms() { return (long)(esp_timer_get_time() / 1000); }

// ========== 生成主循环 ==========
void generate(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler, 
              const char *prompt, int steps, 
              llm_token_cb on_token, void *user_data, 
              generated_complete_cb cb_done) {
  if (!prompt) prompt = "";
  int num_prompt_tokens = 0;
  int *prompt_tokens = LLM_ALLOC(int, strlen(prompt) + 3);
  encode(tokenizer, const_cast<char *>(prompt), 1, 0, prompt_tokens, &num_prompt_tokens);
  if (num_prompt_tokens < 1) { free(prompt_tokens); return; }

  long start = 0;
  int next, token = prompt_tokens[0], pos = 0;
  
  while (pos < steps) {
    float *logits = forward(transformer, token, pos);
    next = (pos < num_prompt_tokens - 1) ? prompt_tokens[pos + 1] : sample(sampler, logits);
    pos++;
    if (next == 1 || next == 2) break;

    const char *piece = decode(tokenizer, token, next);
    if (on_token) on_token(piece, user_data);
    token = next;
    if (start == 0) start = time_in_ms();

    if (pos % 16 == 0) yield();  // 防止看门狗复位
  }
  
  if (pos > 1 && cb_done) {
    long end = time_in_ms();
    float tks = (pos - 1) / (double)(end - start) * 1000;
    cb_done(tks, user_data);
  }
  free(prompt_tokens);
}