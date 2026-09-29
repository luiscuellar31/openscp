#include "logic/updates/MacUpdater.hpp"

namespace openscpui {
struct MacUpdater::Impl {};
MacUpdater::MacUpdater(std::function<bool()>, std::function<void()>,
                       QObject *parent)
    : QObject(parent) {
}
MacUpdater::~MacUpdater() = default;
bool MacUpdater::available() const {
    return false;
}
bool MacUpdater::check() {
    return false;
}
} // namespace openscpui
