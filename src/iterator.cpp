#include "iterator.hpp"

#include <algorithm>

#include "merging_iterator.hpp"
#include "table/format.hpp"
#include "table/table_iterator.hpp"

namespace lsm::internal {
namespace {
// Higher levels are disjoint. Keep only one table cursor, opening it on demand;
// the Version owns all metadata/readers without copying file or entry arrays.
class LevelIterator final : public InternalIterator {
 public:
  LevelIterator(std::shared_ptr<const Version> version, std::size_t level)
      : version_(std::move(version)), level_(level) {}
  [[nodiscard]] auto Valid() const -> bool override {
    return status_.ok() && current_ && current_->Valid();
  }
  void SeekToFirst() override {
    if (Load(0)) {
      current_->SeekToFirst();
      Skip(false);
    }
  }
  void SeekToLast() override {
    if (!Files().empty() && Load(Files().size() - 1)) {
      current_->SeekToLast();
      Skip(true);
    }
  }
  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    if (target.size() < 8) {
      status_ = Status::InvalidArgument("short level iterator seek");
      return;
    }
    const auto found = std::ranges::lower_bound(
        Files(), table::UserKey(target), {},
        [&](auto i) -> const std::string& { return version_->files[i].largest; });
    if (Load(static_cast<std::size_t>(found - Files().begin()))) {
      current_->Seek(target);
      Skip(false);
    }
  }
  void Next() override {
    if (Valid()) {
      current_->Next();
      Skip(false);
    }
  }
  void Prev() override {
    if (Valid()) {
      current_->Prev();
      Skip(true);
    }
  }
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? current_->key() : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? current_->value() : std::string_view{};
  }
  [[nodiscard]] auto StatusRef() const -> const Status& override { return status_; }

 private:
  [[nodiscard]] auto Files() const -> const std::vector<std::size_t>& {
    return version_->levels[level_];
  }
  auto Load(std::size_t index) -> bool {
    current_.reset();
    if (!status_.ok() || index >= Files().size()) return false;
    index_ = index;
    current_ = std::make_unique<table::TableIterator>(version_->files[Files()[index]].reader);
    return true;
  }
  void Skip(bool reverse) {
    while (current_) {
      if (!current_->StatusRef().ok()) status_ = current_->StatusRef();
      if (!status_.ok() || current_->Valid()) return;
      if (reverse && index_ == 0) {
        current_.reset();
        return;
      }
      if (!Load(reverse ? index_ - 1 : index_ + 1)) return;
      if (reverse)
        current_->SeekToLast();
      else
        current_->SeekToFirst();
    }
  }
  std::shared_ptr<const Version> version_;
  std::size_t level_;
  std::size_t index_ = 0;
  std::unique_ptr<InternalIterator> current_;
  Status status_;
};

class DBIter final : public Iterator {
 public:
  DBIter(std::vector<std::unique_ptr<InternalIterator>> children, std::uint64_t sequence,
         std::shared_ptr<const Version> version, Status status, std::function<void()> released)
      : input_(std::make_unique<MergingIterator>(std::move(children))),
        version_(std::move(version)),
        sequence_(sequence),
        status_(std::move(status)),
        released_(std::move(released)) {
    if (status_.ok() && !input_->StatusRef().ok()) status_ = input_->StatusRef();
  }
  ~DBIter() override {
    input_.reset();  // Release table cursors and both memtables before waking cleanup.
    version_.reset();
    if (released_) released_();
  }
  [[nodiscard]] auto Valid() const -> bool override { return valid_ && status_.ok(); }
  void SeekToFirst() override {
    if (status_.ok()) {
      input_->SeekToFirst();
      Read(false);
    }
  }
  void SeekToLast() override {
    if (status_.ok()) {
      input_->SeekToLast();
      Read(true);
    }
  }
  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    input_->Seek(table::InternalKey(target, kMaxSequence, WriteBatch::Type::kPut));
    Read(false);
  }
  void Next() override {
    if (!Valid()) return;
    if (reverse_) {
      input_->Seek(table::InternalKey(key_, 0, WriteBatch::Type::kDelete));
    }
    Read(false);
  }
  void Prev() override {
    if (!Valid()) return;
    if (!reverse_) {
      input_->Seek(table::InternalKey(key_, kMaxSequence, WriteBatch::Type::kPut));
      if (input_->Valid())
        input_->Prev();
      else if (input_->StatusRef().ok())
        input_->SeekToLast();
    }
    Read(true);
  }
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? key_ : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? value_ : std::string_view{};
  }
  [[nodiscard]] auto status() const -> Status override { return status_; }

 private:
  void Read(bool reverse) {
    valid_ = false;
    reverse_ = reverse;
    // Member buffers are reused across entries; a scan allocates only when a
    // key or value outgrows every earlier one.
    while (input_->Valid()) {
      user_.assign(table::UserKey(input_->key()));
      bool selected = false;
      bool put = false;
      bool have_previous = false;
      while (input_->Valid() && table::UserKey(input_->key()) == user_) {
        const auto key = input_->key();
        const auto value = input_->value();
        if (have_previous && previous_key_ == key && previous_value_ != value) {
          status_ = Status::Corruption("conflicting iterator internal key");
          return;
        }
        if ((table::Tag(key) >> 8) <= sequence_ && (reverse || !selected)) {
          selected = true;
          put = (table::Tag(key) & 0xffU) != 0;
          if (put) value_.assign(value);
        }
        previous_key_.assign(key);
        previous_value_.assign(value);
        have_previous = true;
        if (reverse)
          input_->Prev();
        else
          input_->Next();
      }
      if (!input_->StatusRef().ok()) status_ = input_->StatusRef();
      if (!status_.ok()) return;
      if (selected && put) {
        key_.assign(user_);
        valid_ = true;
        return;
      }
    }
    if (!input_->StatusRef().ok()) status_ = input_->StatusRef();
  }
  std::unique_ptr<MergingIterator> input_;
  std::shared_ptr<const Version> version_;
  std::uint64_t sequence_;
  Status status_;
  std::function<void()> released_;
  bool valid_ = false;
  bool reverse_ = false;
  std::string key_;
  std::string value_;
  std::string user_;
  std::string previous_key_;
  std::string previous_value_;
};
}  // namespace

auto MakeVersionIterators(const std::shared_ptr<const Version>& version)
    -> std::vector<std::unique_ptr<InternalIterator>> {
  std::vector<std::unique_ptr<InternalIterator>> children;
  for (const auto index : version->levels[0])
    children.push_back(std::make_unique<table::TableIterator>(version->files[index].reader));
  for (std::size_t level = 1; level < version->levels.size(); ++level)
    if (!version->levels[level].empty())
      children.push_back(std::make_unique<LevelIterator>(version, level));
  return children;
}

auto MakeIterator(std::vector<std::unique_ptr<InternalIterator>> children, std::uint64_t sequence,
                  std::shared_ptr<const Version> version, Status status,
                  std::function<void()> released) -> std::unique_ptr<Iterator> {
  return std::make_unique<DBIter>(std::move(children), sequence, std::move(version),
                                  std::move(status), std::move(released));
}
}  // namespace lsm::internal
