#include "lm/interpolate/tune_instance.hh"

#include "lm/common/model_buffer.hh"
#include "lm/common/ngram_stream.hh"
#include "lm/common/renumber.hh"
#include "lm/enumerate_vocab.hh"
#include "lm/interpolate/merge_vocab.hh"
#include "lm/interpolate/universal_vocab.hh"
#include "util/file_piece.hh"
#include "util/murmur_hash.hh"
#include "util/stream/chain.hh"
#include "util/tokenize_piece.hh"

#include <boost/unordered_map.hpp>

#include <cmath>
#include <vector>

namespace lm { namespace interpolate {

// Intermediate representation of an Instance while it is being built.  Has
// backoffs for the context separately so they can be applied to probabilities.
class InstanceBuilder {
  public:
    InstanceBuilder(WordIndex correct, std::size_t max_order) 
      : backoffs_(max_order), correct_(correct) {}

    void MatchedBackoff(uint8_t order, float ln_backoff) {
      backoffs_[order - 1] = ln_backoff;
    }

    void MatchedContext(uint8_t order, WordIndex word, float ln_prob) {
      std::pair<WordIndex, ContinueValue> to_ins;
      to_ins.first = word;
      to_ins.second.ln_prob = ln_prob;
      to_ins.second.order = order;
      to_ins.second.row = extensions_.size();
      std::pair<boost::unordered_map<WordIndex, ContinueValue>::iterator, bool> ret(extensions_.insert(to_ins));
      if (!ret.second && ret.first->second.order < order) {
        ret.first->second.ln_prob = ln_prob;
        ret.first->second.order = order;
      }
    }

    void Dump(std::size_t model_index, UnigramProbs &unigrams, Instance &out) {
      // Turn backoffs into multiplied values (added in log space).
      float accum = 0.0;
      for (std::vector<float>::reverse_iterator i = backoffs_.rbegin(); i != backoffs_.rend(); ++i) {
        accum += *i;
        *i = accum;
      }
      out.backoff_(model_index) = accum;
      std::size_t old_rows = out.extension_values_.rows();
      out.extension_values_.conservativeResize(extensions_.size(), Eigen::NoChange_t());
      out.extension_words_.resize(extensions_.size());
      for (boost::unordered_map<WordIndex, ContinueValue>::iterator i = extensions_.begin(); i != extensions_.end(); ++i) {
        // Compute backed off probability.
        if (i->second.order == 0) {
          i->second.ln_prob = unigrams(i->first, model_index);
          i->second.order = 1;
        }
        i->second.ln_prob += backoffs_[i->second.order - 1];
        out.extension_values_(i->second.row, model_index) = i->second.ln_prob;
        // Fill in other models.
        if (i->second.row > old_rows) {
          for (std::size_t m = 0; m < model_index; ++m) {
            out.extension_values_(i->second.row, m) = unigrams(i->first, m) + out.backoff_(m);
          }
          out.extension_words_[i->second.row] = i->first;
        }
        // Prepare for next model.
        i->second.order = 0;
      }
      // Fill in probability of correct word.
      boost::unordered_map<WordIndex, ContinueValue>::const_iterator correct_it = extensions_.find(correct_);
      if (correct_it == extensions_.end()) {
        out.correct_(model_index) = unigrams(correct_, model_index) + out.backoff_(model_index);
      } else {
        out.correct_(model_index) = out.extension_values_(correct_it->second.row, model_index);
      }
      std::fill(backoffs_.begin(), backoffs_.end(), 0.0);
    }

  private:
    struct ContinueValue {
      float ln_prob;
      uint8_t order;
      std::size_t row;
    };

    // This map persists across models being loaded and maps words to their row of the extension_values_ matrix.
    boost::unordered_map<WordIndex, ContinueValue> extensions_;

    std::vector<float> backoffs_;

    const WordIndex correct_;
};

namespace {

// Forward information to multiple instances of a context.
class DispatchContext {
  public:
    void Register(InstanceBuilder &context) {
      registered_.push_back(&context);
    }

    void MatchedBackoff(uint8_t order, float ln_backoff) {
      for (std::vector<InstanceBuilder*>::iterator i = registered_.begin(); i != registered_.end(); ++i)
        (*i)->MatchedBackoff(order, ln_backoff);
    }

    void MatchedContext(uint8_t order, WordIndex word, float ln_prob) {
      for (std::vector<InstanceBuilder*>::iterator i = registered_.begin(); i != registered_.end(); ++i)
        (*i)->MatchedContext(order, word, ln_prob);
    }

  private:
    std::vector<InstanceBuilder*> registered_;
};

// Map from n-gram hash to contexts in the tuning data.
typedef boost::unordered_map<uint64_t, DispatchContext> ContextMap;

class UnigramLoader {
  public:
    UnigramLoader(ContextMap &contexts_for_backoffs, UnigramProbs &probs, std::size_t model_number)
      : map_(contexts_for_backoffs),
        prob_(probs.col(model_number)) {}

    void Run(const util::stream::ChainPosition &position) {
      // TODO handle the case of a unigram model?
      for (NGramStream<ProbBackoff> input(position); input; ++input) {
        prob_(*input->begin()) = input->Value().prob * M_LN10;
        ContextMap::iterator i = map_.find(util::MurmurHashNative(input->begin(), sizeof(WordIndex)));
        if (i != map_.end()) {
          i->second.MatchedBackoff(1, input->Value().backoff * M_LN10);
        }
      }
    }

  private:
    ContextMap &map_;
    UnigramProbs::ColXpr prob_;
    std::size_t model_;
};

class MiddleLoader {
  public:
    explicit MiddleLoader(ContextMap &map)
      : map_(map) {}

    void Run(const util::stream::ChainPosition &position) {
      NGramStream<ProbBackoff> input(position);
      const std::size_t full_size = (uint8_t*)input->end() - (uint8_t*)input->begin();
      const std::size_t context_size = full_size - sizeof(WordIndex);
      ContextMap::iterator i;
      for (NGramStream<ProbBackoff> input(position); input; ++input) {
        i = map_.find(util::MurmurHashNative(input->begin(), full_size));
        if (i != map_.end()) {
          i->second.MatchedBackoff(input->Order(), input->Value().backoff * M_LN10);
        }
        i = map_.find(util::MurmurHashNative(input->begin(), context_size));
        if (i != map_.end()) {
          i->second.MatchedContext(input->Order(), *(input->end() - 1), input->Value().prob * M_LN10);
        }
      }
    }

  private:
    ContextMap &map_;
};

class HighestLoader {
  public:
    HighestLoader(ContextMap &map, uint8_t order)
      : map_(map), order_(order) {}

    void Run(const util::stream::ChainPosition &position) {
      ContextMap::iterator i;
      const std::size_t context_size = sizeof(WordIndex) * (order_ - 1);
      for (ProxyStream<NGram<float> > input(position, NGram<float>(NULL, order_)); input; ++input) {
        i = map_.find(util::MurmurHashNative(input->begin(), context_size));
        if (i != map_.end()) {
          i->second.MatchedContext(order_, *(input->end() - 1), input->Value() * M_LN10);
        }
      }
    }

  private:
    ContextMap &map_;
    const uint8_t order_;
};

class IdentifyTuning : public EnumerateVocab {
  public:
    IdentifyTuning(int tuning_file, std::vector<WordIndex> &out) : indices_(out) {
      indices_.clear();
      StringPiece line;
      std::size_t counter = 0;
      std::vector<std::size_t> &eos = words_[util::MurmurHashNative("</s>", 4)];
      for (util::FilePiece f(tuning_file); f.ReadLineOrEOF(line);) {
        for (util::TokenIter<util::BoolCharacter, true> word(line, util::kSpaces); word; ++word) {
          words_[util::MurmurHashNative(word->data(), word->size())].push_back(counter++);
        }
        eos.push_back(counter++);
      }
      // Also get <s>
      indices_.resize(counter + 1);
      words_[util::MurmurHashNative("<s>", 3)].push_back(indices_.size() - 1);
    }

    void Add(WordIndex id, const StringPiece &str) {
      boost::unordered_map<uint64_t, std::vector<std::size_t> >::iterator i = words_.find(util::MurmurHashNative(str.data(), str.size()));
      if (i != words_.end()) {
        for (std::vector<std::size_t>::iterator j = i->second.begin(); j != i->second.end(); ++j) {
          indices_[*j] = id;
        }
      }
    }

    WordIndex FinishGetBOS() {
      WordIndex ret = indices_.back();
      indices_.pop_back();
      return ret;
    }

  private:
    std::vector<WordIndex> &indices_;

    boost::unordered_map<uint64_t, std::vector<std::size_t> > words_;
};

} // namespace

void Load(int tuning_file, const std::vector<StringPiece> &model_names, util::FixedArray<Instance> &instances, UnigramProbs &unigrams) {
  util::FixedArray<ModelBuffer> models(model_names.size());
  std::vector<WordIndex> vocab_sizes;
  vocab_sizes.reserve(model_names.size());
  util::FixedArray<util::scoped_fd> vocab_files;
  std::size_t max_order = 0;
  for (std::vector<StringPiece>::const_iterator i = model_names.begin(); i != model_names.end(); ++i) {
    models.push_back(*i);
    vocab_sizes.push_back(models.back().Counts()[0]);
    vocab_files.push_back(models.back().StealVocabFile());
    max_order = std::max(max_order, models.back().Order());
  }
  UniversalVocab vocab(vocab_sizes);
  std::vector<WordIndex> tuning_words;
  WordIndex bos;
  WordIndex combined_vocab_size;
  {
    IdentifyTuning identify(tuning_file, tuning_words);
    combined_vocab_size = MergeVocab(vocab_files, vocab, identify);
    bos = identify.FinishGetBOS();
  }

  instances.Init(tuning_words.size());
  util::FixedArray<InstanceBuilder> builders(tuning_words.size());
  std::vector<WordIndex> context;
  context.push_back(bos);

  // Populate the map from contexts to instance builders.
  ContextMap cmap;
  const WordIndex eos = tuning_words.back();
  for (std::size_t i = 0; i < tuning_words.size(); ++i) {
    instances.push_back(model_names.size());
    builders.push_back(max_order, tuning_words[i]);
    for (std::size_t j = 0; j < context.size(); ++j) {
      cmap[util::MurmurHashNative(&context[j], sizeof(WordIndex) * (context.size() - j))].Register(builders.back());
    }
    // Prepare for next word.
    if (tuning_words[i] == eos) {
      context.clear();
      context.push_back(bos);
    } else {
      if (context.size() == max_order) {
        context.erase(context.begin());
      }
      context.push_back(tuning_words[i]);
    }
  }

  unigrams.resize(combined_vocab_size, models.size());

  // Scan through input files.  Sadly not parallel due to an underlying hash table.
  for (std::size_t m = 0; m < models.size(); ++m) {
    for (std::size_t order = 1; order <= models[m].Order(); ++order) {
      util::stream::Chain chain(util::stream::ChainConfig(sizeof(ProbBackoff) + order * sizeof(WordIndex), 2, 64 * 1048576));
      models[m].Source(order - 1, chain);
      chain >> Renumber(vocab.Mapping(m), order);
      if (order == 1) {
        chain >> UnigramLoader(cmap, unigrams, m);
      } else if (order < models[m].Order()) {
        chain >> MiddleLoader(cmap);
      } else {
        chain >> HighestLoader(cmap, order);
      }
    }
    for (std::size_t instance = 0; instance < tuning_words.size(); ++instance) {
      builders[instance].Dump(m, unigrams, instances[instance]);
    }
  }
}

}} // namespaces
