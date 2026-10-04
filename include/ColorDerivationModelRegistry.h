#pragma once
#include <string>
#include <vector>
#include <memory>
#include "ColorDerivationModel.h"

// Holds all available ColorDerivationModel implementations, each under a
// unique name, and tracks which one is currently active. The single
// (Meyers-singleton) owner of every registered model's lifetime -- models
// are expected to be registered exactly once, at boot (see main.cpp::setup()),
// not added/removed at runtime afterward.
class ColorDerivationModelRegistry {
public:
  static ColorDerivationModelRegistry& instance();

  void registerModel(const std::string& name, std::unique_ptr<ColorDerivationModel> model);

  size_t count() const { return entries_.size(); }
  const std::string& nameAt(size_t index) const { return entries_[index].name; }

  // Invariant maintained by setup() (never checked defensively here, same
  // style as TipCatalog's "never empty" invariant): at least one model is
  // registered before active()/activeName() is ever called.
  ColorDerivationModel& active() const { return *entries_[activeIndex_].model; }
  const std::string& activeName() const { return entries_[activeIndex_].name; }

  // No-op (returns false) if 'name' isn't registered -- e.g. a persisted
  // selection from a firmware version that had a model this one doesn't;
  // the previously active model (by default, whichever was registered
  // first -- "Default") simply stays active.
  bool setActive(const std::string& name);

private:
  ColorDerivationModelRegistry() = default;
  struct Entry { std::string name; std::unique_ptr<ColorDerivationModel> model; };
  std::vector<Entry> entries_;
  size_t activeIndex_ = 0;
};
