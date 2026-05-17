#ifndef LLM_H
#define LLM_H
#include <Arduino.h>
#include <stddef.h>

// 概率索引结构体
typedef struct {
  float prob;
  int index;
} ProbIndex;

// 采样器结构体
typedef struct {
  int vocab_size;
  ProbIndex* probindex;
  float temperature;
  float topp;
  unsigned long long rng_state;
} Sampler;

// 分词索引结构体
typedef struct {
  char* str;
  int id;
} TokenIndex;

// 分词器结构体
typedef struct {
  char** vocab;
  float* vocab_scores;
  TokenIndex* sorted_vocab;
  int vocab_size;
  unsigned int max_token_length;
  unsigned char byte_pieces[512];
} Tokenizer;

// ⚠️ 关键修复：移除 __attribute__((packed))，确保与二进制文件内存对齐一致
typedef struct {
  int dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len;
} Config;

// 模型权重结构体（统一使用 float，避免 ESP32 默认向量对齐问题）
typedef struct {
  float* token_embedding_table;
  float* rms_att_weight;
  float* rms_ffn_weight;
  float *wq, *wk, *wv, *wo;
  float *w1, *w2, *w3;
  float* rms_final_weight;
  float* wcls;
} TransformerWeights;

// 运行时状态结构体
typedef struct {
  float *x, *xb, *xb2, *hb, *hb2, *q, *k, *v, *att, *logits;
  float* key_cache;
  float* value_cache;
} RunState;

// Transformer 总结构体
typedef struct {
  Config config;
  TransformerWeights weights;
  RunState state;
  int fd;
  float* data;
  size_t file_size;
} Transformer;

// 回调函数定义
typedef void (*llm_token_cb)(const char* token, void* user_data);
typedef void (*generated_complete_cb)(float tokens_ps, void* user_data);

// 函数声明（参数类型与实现严格对齐）
void build_transformer(Transformer* t, const char* checkpoint_path);
void build_tokenizer(Tokenizer* t, const char* tokenizer_path, int vocab_size);
void build_sampler(Sampler* sampler, int vocab_size, float temperature, float topp, unsigned long long rng_seed);
void generate(Transformer* transformer, Tokenizer* tokenizer, Sampler* sampler, const char* prompt, int steps, llm_token_cb on_token, void* user_data, generated_complete_cb cb_done);
void free_sampler(Sampler* sampler);
void free_transformer(Transformer* t);
void free_tokenizer(Tokenizer* t);
#endif