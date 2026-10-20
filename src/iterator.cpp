#include "iterator.hpp"

#include <algorithm>

#include "compaction.hpp"
#include "table/format.hpp"

namespace lsm::internal {
namespace {
class MaterializedIterator final : public Iterator {
 public:
  MaterializedIterator(std::shared_ptr<const Version> version, Status status,
                       std::function<void()> released)
      : version_(std::move(version)), status_(std::move(status)), released_(std::move(released)) {}
  ~MaterializedIterator() override {
    version_.reset();
    if (released_) {
      released_();
    }
  }
  void Build(std::vector<std::vector<table::Entry>> streams, std::uint64_t sequence) {
    if (!status_.ok()) {
      return;
    }
    MergingIterator input(std::move(streams));
    std::optional<table::Entry> previous;
    std::string user;
    bool selected = false;
    while (input.Valid()) {
      const auto& entry = input.entry();
      if (previous && previous->key == entry.key && previous->value != entry.value) {
        status_ = Status::Corruption("conflicting iterator internal key");
        return;
      }
      if (!previous || user != table::UserKey(entry.key)) {
        user = table::UserKey(entry.key);
        selected = false;
      }
      if (!selected && (table::Tag(entry.key) >> 8) <= sequence) {
        selected = true;
        if ((table::Tag(entry.key) & 0xffU) != 0) {
          rows_.emplace_back(user, entry.value);
        }
      }
      previous = entry;
      input.Next();
    }
    status_ = input.status();
    position_ = rows_.size();
  }
  [[nodiscard]] auto Valid() const -> bool override {
    return status_.ok() && position_ < rows_.size();
  }
  void SeekToFirst() override { position_ = 0; }
  void SeekToLast() override { position_ = rows_.empty() ? 0 : rows_.size() - 1; }
  void Seek(std::string_view target) override {
    const auto found = std::ranges::lower_bound(rows_, target, {}, &Row::first);
    position_ = static_cast<std::size_t>(found - rows_.begin());
  }
  void Next() override {
    if (Valid()) ++position_;
  }
  void Prev() override {
    if (Valid()) position_ = position_ == 0 ? rows_.size() : position_ - 1;
  }
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? std::string_view(rows_[position_].first) : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? std::string_view(rows_[position_].second) : std::string_view{};
  }
  [[nodiscard]] auto status() const -> Status override { return status_; }

 private:
  using Row = std::pair<std::string, std::string>;
  std::shared_ptr<const Version> version_;
  Status status_;
  std::function<void()> released_;
  std::vector<Row> rows_;
  std::size_t position_ = 0;
};
}  // namespace

auto MakeIterator(std::vector<std::vector<table::Entry>> streams, std::uint64_t sequence,
                  std::shared_ptr<const Version> version, Status status,
                  std::function<void()> released) -> std::unique_ptr<Iterator> {
  auto iterator = std::make_unique<MaterializedIterator>(std::move(version), std::move(status),
                                                         std::move(released));
  iterator->Build(std::move(streams), sequence);
  return iterator;
}
}  // namespace lsm::internal
