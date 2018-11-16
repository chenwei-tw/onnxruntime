// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "tokenizer.h"
#include "onnx/defs/schema.h"
#include "core/common/common.h"
#include "core/framework/tensor.h"

#include "core/common/utf8_util.h"

#include <codecvt>
#include <locale>

namespace onnxruntime {
namespace contrib {

using namespace utf8_util;

ONNX_CPU_OPERATOR_TYPED_MS_KERNEL(
    Tokenizer,
    1,
    string,
    KernelDefBuilder()
        .TypeConstraint("T", DataTypeImpl::GetTensorType<std::string>()),
    contrib::Tokenizer);

namespace tokenizer_details {

const char start_text = 0x2;
const char end_text = 0x3;

const std::string conv_error("Conversion Error");
const std::wstring wconv_error(L"Conversion Error");

// Use a Trie like structure for searching multiple strings
// at once but convert it to a ternary tree for saving space.
// We insert in the same order separators are specified.
// Template parameter is a CharT which can be a char/wchar_t
// or anything else that supports operator ><,== as long as
// this is not a variable length sequence. We convert utf8 to utf16
// before inserting.
// Value is a supplementary information useful for search hit
// and is present in the nodes that terminate the whole search pattern
template <class CharT, class Value>
class TernarySearchTree {
 private:
  struct Node {
    CharT c_;  // character
    bool has_val_;
    Value value_;
    std::unique_ptr<Node> left_;
    std::unique_ptr<Node> mid_;
    std::unique_ptr<Node> right_;
    explicit Node(CharT c) : c_(c), has_val_(false), value_() {
    }
    explicit Node(CharT c, const Value& v) : c_(c), has_val_(true), value_(v) {
    }
    ~Node() = default;
  };

 public:
  TernarySearchTree() = default;
  // XXX: Might need to reconsider
  // We hope that the search patterns are not
  // too long. Otherwise, reconsider destruction
  // and search depth traversals
  ~TernarySearchTree() = default;

  /**
  * Returns a ptr to an associated value and nullptr on search miss.
  */
  const Value* get(const CharT* str, size_t len) const {
    if (len == 0) {
      return nullptr;
    }
    const Node* n = get(root_.get(), str, len, 0);
    if (n == nullptr || !n->has_val_) {
      return nullptr;
    }
    return &n->value_;
  }

  /**
  * Returns true if successful and false on empty strings
  * and duplicates.
  */
  bool put(const CharT* str, size_t len, const Value& v) {
    if (len < 1) {
      assert(false);
      return false;
    }
    Node* new_root = put(root_.get(), str, len, v, 0);
    if (new_root != nullptr) {
      root_.release();
      root_.reset(new_root);
      return true;
    }
    return false;
  }

 private:
  const Node* get(const Node* node, const CharT* str, size_t len, size_t depth) const {
    if (node == nullptr) {
      return nullptr;
    }
    assert(depth < len);
    CharT c = str[depth];
    if (c < node->c_) {
      return get(node->left_.get(), str, len, depth);
    } else if (c > node->c_) {
      return get(node->right_.get(), str, len, depth);
    } else if (depth < (len - 1)) {
      // Greedy match trying to match the longest
      // if this is a leaf, return a match
      if (node->mid_ != nullptr) {
        return get(node->mid_.get(), str, len, depth + 1);
      }
    }
    return node;
  }

  Node* put(Node* node, const CharT* str, size_t len, const Value& v, size_t depth) {
    CharT c = str[depth];

    std::unique_ptr<Node> new_node;
    if (node == nullptr) {
      new_node.reset(new Node(c));
    }

    Node* new_link = nullptr;
    Node* n = (node != nullptr) ? node : new_node.get();
    if (c < n->c_) {
      new_link = put(n->left_.get(), str, len, v, depth);
      if (new_link != nullptr) {
        n->left_.release();
        n->left_.reset(new_link);
      }
    } else if (c > n->c_) {
      new_link = put(n->right_.get(), str, len, v, depth);
      if (new_link != nullptr) {
        n->right_.release();
        n->right_.reset(new_link);
      }
    } else if (depth < (len - 1)) {
      new_link = put(n->mid_.get(), str, len, v, depth + 1);
      if (new_link != nullptr) {
        n->mid_.release();
        n->mid_.reset(new_link);
      }
    } else {
      if (!n->has_val_) {
        n->value_ = v;
        n->has_val_ = true;
        new_link = n;
      }
    }
    if (new_link != nullptr) {
      new_node.release();
      return n;
    }
    return nullptr;
  }
  std::unique_ptr<Node> root_;
};

}  // namespace tokenizer_details

Status onnxruntime::contrib::Tokenizer::CharTokenize(OpKernelContext* ctx, size_t N, size_t C,
                                                     const std::vector<int64_t>& input_dims) const {
  using namespace tokenizer_details;
  // With char tokenzation we get as many tokens as the number of
  // utf8 characters in the string. So for every string we calculate its character(utf-8) length
  // add padding and add start/end test separators if necessary
  size_t max_tokens = 0;
  auto X = ctx->Input<Tensor>(0);
  auto const input_data = X->template Data<std::string>();
  for (size_t n = 0; n < N; ++n) {
    for (size_t c = 0; c < C; ++c) {
      const auto* s = input_data + (n * C) + c;
      size_t tokens = 0;  // length in utf8 chars
      if (!utf8_validate(reinterpret_cast<const unsigned char*>(s->data()), s->size(),
                         tokens)) {
        return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                      "Input string contains invalid utf8 chars: " + *s);
      }
      if (mark_) {
        tokens += 2;  // Start/end markers as separate tokens
      }
      max_tokens = std::max(max_tokens, tokens);
    }
  }

  std::vector<int64_t> output_dims(input_dims);
  output_dims.push_back(max_tokens);
  TensorShape output_shape(output_dims);
  auto output_tensor = ctx->Output(0, output_shape);
  auto const output_data = output_tensor->template MutableData<std::string>();

  size_t output_index = 0;
  for (size_t n = 0; n < N; ++n) {
    for (size_t c = 0; c < C; ++c) {
      const auto* s = input_data + (n * C) + c;
      if (mark_) {
        new (output_data + output_index) std::string(&start_text, 1);
        ++output_index;
      }
      size_t tokens = 0;
      const size_t str_len = s->size();
      for (size_t token_idx = 0; token_idx < str_len;) {
        size_t tlen = utf8_bytes(static_cast<unsigned char>((*s)[token_idx]));
        assert(token_idx + tlen <= str_len);
        new (output_data + output_index) std::string(s->substr(token_idx, tlen));
        ++output_index;
        token_idx += tlen;
        ++tokens;
      }
      // Padding strings
      assert(tokens + (mark_ * 2) <= max_tokens);
      const size_t pads = max_tokens - (mark_ * 2) - tokens;
      for (size_t p = 0; p < pads; ++p) {
        new (output_data + output_index) std::string(padvalue_);
        ++output_index;
      }
      if (mark_) {
        new (output_data + output_index) std::string(&end_text, 1);
        ++output_index;
      }
    }
  }
  return Status::OK();
}

Status onnxruntime::contrib::Tokenizer::SeparatorTokenize(OpKernelContext* ctx,
                                                          size_t N, size_t C,
                                                          const std::vector<int64_t>& input_dims) const {
  using namespace tokenizer_details;

  // We store the length of the original pattern within the
  // Ternary Tree so on search miss we drop this many characters from the
  // string
  struct ValueType {
    size_t w_len;
  };

  using SearchTree = TernarySearchTree<wchar_t, ValueType>;

  SearchTree stree;
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter(conv_error, wconv_error);
  for (auto& sep : separators_) {
    std::wstring wsep = converter.from_bytes(sep);
    if (wsep.empty()) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                    "No empty separators allowed");
    }
    if (wsep == wconv_error) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                    "Separator strings contains invalid utf8 chars: " + sep);
    }
    stree.put(wsep.c_str(), wsep.length(), {wsep.length()});
  }

  // Scan all strings and attempt to find separators in them
  // collect all the output tokens here
  size_t max_tokens = 0;
  std::vector<std::vector<std::wstring>> tokenized_strings;
  tokenized_strings.reserve(N * C);
  auto X = ctx->Input<Tensor>(0);
  auto const input_data = X->template Data<std::string>();
  for (size_t n = 0; n < N; ++n) {
    for (size_t c = 0; c < C; ++c) {
      const size_t input_idx = (n * C) + c;
      const auto* s = input_data + input_idx;
      tokenized_strings.emplace_back();
      auto& row_tokens = tokenized_strings.back();

      std::wstring wstr = converter.from_bytes(*s);
      if (wstr == wconv_error) {
        return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                      "Invalid utf8 chars in the input: " + *s);
      }
      const wchar_t* ws = wstr.c_str();
      const wchar_t* last_mismatch = ws;
      size_t len_remaining = wstr.length();
      while (len_remaining > 0) {
        const auto* val = stree.get(ws, len_remaining);
        if (val != nullptr) {
          auto token_len = ws - last_mismatch;
          if (token_len > mincharnum_) {
            row_tokens.emplace_back(last_mismatch, token_len);
          }
          assert(val->w_len <= len_remaining);
          ws += val->w_len;
          len_remaining -= val->w_len;
          last_mismatch = ws;
        } else {
          // No match, start with a next char
          ++ws;
          --len_remaining;
        }
      }

      // When we have no matches or a trailing token
      if (ws > last_mismatch) {
        row_tokens.emplace_back(last_mismatch, ws - last_mismatch);
      }

      size_t tokens = row_tokens.size();
      if (mark_) {
        tokens += 2;  // Start/end markers as separate tokens
      }
      max_tokens = std::max(max_tokens, tokens);
    }
  }

  std::vector<int64_t> output_dims(input_dims);
  output_dims.push_back(max_tokens);
  TensorShape output_shape(output_dims);

  auto output_tensor = ctx->Output(0, output_shape);
  auto const output_data = output_tensor->template MutableData<std::string>();

  const size_t max_output_index = N * C * max_tokens;
  size_t output_index = 0;
  for (auto& row : tokenized_strings) {
#ifdef _DEBUG
    size_t c_idx = output_index;
#endif
    if (mark_) {
      new (output_data + output_index) std::string(&start_text, 1);
      ++output_index;
    }
    // Output tokens for this row
    for (auto& token : row) {
      new (output_data + output_index) std::string(converter.to_bytes(token));
      ++output_index;
    }
    const size_t pads = max_tokens - (mark_ * 2) - row.size();
    for (size_t p = 0; p < pads; ++p) {
      new (output_data + output_index) std::string(padvalue_);
      ++output_index;
    }
    if (mark_) {
      new (output_data + output_index) std::string(&end_text, 1);
      ++output_index;
    }
    assert(output_index <= max_output_index);
    assert((output_index - c_idx) <= max_tokens);
  }
  return Status::OK();
}

Status Tokenizer::Compute(OpKernelContext* ctx) const {
  using namespace tokenizer_details;

  if (separators_.empty()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "requires at least one separator");
  }

  // Special case for a single empty string separator
  // which means character level tokenization.
  const bool char_tokenezation = (separators_.size() == 1 &&
                                  separators_[0].empty());

  if (char_tokenezation && mincharnum_ > 1) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "mincharnum is too big for char level tokenezation");
  }

  // Get input buffer ptr
  auto X = ctx->Input<Tensor>(0);
  if (X->DataType() != DataTypeImpl::GetType<std::string>()) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "tensor(string) expected as input");
  }

  auto& input_dims = X->Shape().GetDims();
  if (input_dims.size() != 1 && input_dims.size() != 2) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  "Input dimensions are either [C] or [N][C] allowed");
  }

  size_t N = 0;
  size_t C = 0;
  if (input_dims.size() == 1) {
    N = 1;
    if (input_dims[0] < 1) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                    "Invalid C dimension value");
    }
    C = input_dims[0];
  } else if (input_dims.size() == 2) {
    if (input_dims[0] < 1 || input_dims[1] < 1) {
      return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                    "Invalid N and/or C dimension values");
    }
    N = input_dims[0];
    C = input_dims[1];
  } else {
    assert(false);
  }

  // Max tokens in the last axis
  Status s;
  if (char_tokenezation) {
    s = CharTokenize(ctx, N, C, input_dims);
  } else {
    s = SeparatorTokenize(ctx, N, C, input_dims);
  }
  return s;
}
}  // namespace contrib
}  // namespace onnxruntime