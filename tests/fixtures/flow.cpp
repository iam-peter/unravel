int flow_fixture(int count, int mode) {
  if (count < 0)
    return -1;

  int total = 0;
  for (int i = 0; i < count; ++i) {
    if (i == 2)
      continue;
    total += i;
  }

  switch (mode) {
  case 0:
    total += 1;
    [[fallthrough]];
  case 1:
    total += 2;
    break;
  default:
    total += 3;
  }
  return total;
}

int flow_fixture_other(int value) { return value * 2; }

enum class FlowState { idle = 0, ready = 1 };

int enum_flow_fixture(FlowState input, FlowState &state, int *value) {
  FlowState local = FlowState::idle;
  state = FlowState::ready;
  if (input == FlowState::ready)
    state = FlowState::idle;
  return value ? *value : int(local);
}

struct Stateful {
  FlowState state;
  void update() {
    if (state == FlowState::idle)
      state = FlowState::ready;
    state = FlowState::idle;
    if (state == FlowState::idle) {
      state = FlowState::ready;
      state = FlowState::idle; // prior guard no longer constrains this write
    }
    state = static_cast<FlowState>(42);
  }
};

void unknown_effect(int &);
int local_slice_fixture(int input, bool branch) {
  int value = input;
  if (branch)
    value = 10;
  else
    value = 20;
  int joined = value;
  {
    int value = 30;
    joined += value;
  }
  value = 40;
  unknown_effect(value);
  return value + joined;
}

void empty_flow_fixture() {}

// Minimal API declarations keep this fixture independent of installed Qt
// modules while exercising compiler-resolved API identities, not text matches.
class QObject {
public:
  explicit QObject(QObject *parent = nullptr);
  void setParent(QObject *);
};
class QAbstractState : public QObject {};
class QAbstractTransition {
public:
  void setTargetState(QAbstractState *);
};
class QState : public QAbstractState {
public:
  QAbstractTransition *addTransition(QAbstractState *);
};
void qt_state_fixture(QState *ready, QState *failed) {
  auto *transition = ready->addTransition(failed);
  transition->setTargetState(ready);
  auto *child = new QObject(ready);
  child->setParent(failed);
}

#include <memory>

template <typename T> class QSharedPointer {
public:
  static QSharedPointer<T> create();
};
template <typename T> class QWeakPointer {
public:
  QWeakPointer(const QSharedPointer<T> &);
};
template <typename T> class QScopedPointer {
public:
  explicit QScopedPointer(T *);
};
template <typename T> class QPointer {
public:
  QPointer(T *);
};

void ownership_fixture() {
  int value = 0;
  int &borrow = value;
  int *raw = &value;
  auto shared = std::make_shared<int>(42);
  auto another = shared;
  auto &sharedRef = shared;
  std::weak_ptr<int> observer = shared;
  auto unique = std::make_unique<int>(1);
  auto pointer = unique.get();
  unique.release(); // unresolved transfer remains visible
  auto qtShared = QSharedPointer<int>::create();
  auto qtCopy = qtShared;
  QWeakPointer<int> qtWeak(qtShared);
  QScopedPointer<int> qtScoped(new int(1));
  QPointer<QObject> watched(new QObject);
}

struct OwnershipFields {
  int value;
  int *unknown;
  std::unique_ptr<int> owner;
  std::shared_ptr<int> shared;
  std::weak_ptr<int> observer;
  void inspect() {}
};

void nested_function_fixture() {
  FlowState outer = FlowState::idle;
  auto deferred = [captured = (outer = FlowState::ready)] {
    FlowState state = FlowState::idle;
    auto inside = std::make_unique<int>(1);
  };
  struct Nested {
    void run() { FlowState state = FlowState::ready; }
  };
}
