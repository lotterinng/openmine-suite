#pragma once
// SPDX-License-Identifier: Apache-2.0
// Synchronous change notifications from the project model to observers (UI, scripting).

#include "openmine/objects/Uuid.h"

#include <functional>
#include <map>

namespace openmine {

enum class EntityKind {
    ProjectInfo,
    Layer,
    Geometry,
    Development,
    Calendar,
    Activity,
    Dependency,
    DesignLink,
    Baseline,
    AttributeDefinition,
};

enum class ChangeType { Added, Modified, Removed, Reset };

struct ChangeEvent {
    EntityKind kind;
    ChangeType type;
    Uuid id; ///< nil for Reset / collection-wide events
};

const char* toString(EntityKind k);

class EventBus {
public:
    using Handler = std::function<void(const ChangeEvent&)>;
    using Token = int;

    Token subscribe(Handler h) {
        const Token t = next_++;
        handlers_.emplace(t, std::move(h));
        return t;
    }
    void unsubscribe(Token t) { handlers_.erase(t); }

    void publish(const ChangeEvent& e) const {
        if (muted_ > 0) return;
        // Copy so handlers may (un)subscribe while being notified.
        auto copy = handlers_;
        for (auto& [_, h] : copy) h(e);
    }

    /// RAII suppression while loading a whole project; publish a Reset afterwards.
    struct Mute {
        explicit Mute(EventBus& b) : bus(b) { ++bus.muted_; }
        ~Mute() { --bus.muted_; }
        EventBus& bus;
    };

private:
    std::map<Token, Handler> handlers_;
    Token next_ = 1;
    int muted_ = 0;
};

} // namespace openmine
