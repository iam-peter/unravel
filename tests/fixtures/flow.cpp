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
