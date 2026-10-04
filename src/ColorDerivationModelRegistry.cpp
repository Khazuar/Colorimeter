#include "ColorDerivationModelRegistry.h"

ColorDerivationModelRegistry& ColorDerivationModelRegistry::instance() {
  static ColorDerivationModelRegistry inst;
  return inst;
}

void ColorDerivationModelRegistry::registerModel(const std::string& name,
                                                  std::unique_ptr<ColorDerivationModel> model) {
  entries_.push_back({ name, std::move(model) });
}

bool ColorDerivationModelRegistry::setActive(const std::string& name) {
  for (size_t i = 0; i < entries_.size(); i++) {
    if (entries_[i].name == name) {
      activeIndex_ = i;
      return true;
    }
  }
  return false;
}
