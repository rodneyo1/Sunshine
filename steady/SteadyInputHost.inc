/**
 * @file SteadyInputHost.inc
 * @brief Windows fixed-buffer prototype bindings, included inside namespace input.
 *
 * The unit-rate mapping anchors HELLO creation to its host receipt. Network delay
 * in that exchange becomes a constant offset. This is an experiment, not a
 * qualified absolute-latency calibration or a certified clock-rate bound.
 */
/** @brief One exclusively owned timed keyboard, with bounded trace storage. */
struct timed_keyboard_t {
  /** @brief Native transition adapter used only by the receiver owner thread. */
  struct backend_t {
    timed_keyboard_t *session;  ///< Owning session, alive until the worker joins.

    /** @brief Emit a physical transition. @param key USB identity. @param down Press state. @return Backend success. */
    bool send(steady::Key key, bool down) noexcept {
      std::lock_guard lock {keyboard_mutex};
      steady::WindowsKey translated;
      if (!platf_input || !steady::windows_key(key, translated)) {
        return false;
      }
      const auto ok = platf::virtualhid::keyboard_update(
        platf::virtualhid::get_input_context(platf_input),
        translated.vk,
        !down,
        translated.normalized ? 0 : SS_KBE_FLAG_NON_NORMALIZED,
        translated.extended
      );
      if (ok) {
        session->completed_transitions.fetch_add(1);
      }
      return session->trace(2, 0, 0, key.usage, down, ok) && ok;
    }
  };

  /** @brief Buffered receipt/transition trace row. */
  struct trace_t {
    std::uint64_t kind = 0, sequence = 0, capture = 0, time = 0, usage = 0, down = 0, ok = 0;  ///< Raw row fields.
  };

  using owner_t = steady::WindowsOwner<256, 512, 2048, backend_t>;  ///< Bounded tested scheduler.
  const input_t *input;  ///< Input session identity, compared only while retained by stream teardown.
  std::uint64_t generation;  ///< Negotiated generation.
  steady::WindowsSessionClock clock;  ///< Process incarnation and power-epoch clock.
  backend_t backend {this};  ///< Platform adapter.
  std::unique_ptr<owner_t> owner;  ///< Heap storage avoids a large stack allocation.
  std::atomic<std::uint64_t> completed_transitions {0};  ///< Successful backend submissions, including cleanup.
  std::thread worker;  ///< Sole scheduler/injection worker.
  std::mutex stop_mutex;  ///< Makes stop/join idempotent across teardown callers.
  std::mutex trace_mutex;  ///< Protects the two trace producers.
  std::array<trace_t, 32768> rows;  ///< Fixed trace capacity, no growth on input.
  std::size_t used = 0, overflow = 0;  ///< Trace accounting.
  std::string trace_path;  ///< New output filename selected at launch.
  bool saved = false;  ///< Trace is written once, after join.

  /** @brief Construct a guarded experimental session. @param in Input identity. @param offer Frozen offer. @param path Trace output. */
  timed_keyboard_t(const input_t *in, const unsigned char *offer, std::string path):
      input(in),
      generation(steady_probe_get64(offer + 8)),
      trace_path(std::move(path)) {
    steady::ClockSample sample;
    const auto anchor = steady_probe_get64(offer + 32);
    if (!clock.sample(sample) || !anchor || anchor > UINT64_MAX - STEADY_INPUT_LIFETIME || sample.now < anchor || sample.now >= anchor + STEADY_INPUT_LIFETIME) {
      return;
    }
    // Fixed 60-second experimental lifetime, explicitly not a derived accuracy guarantee.
    const steady::ClockAgreement agreement {generation, anchor, anchor + STEADY_INPUT_LIFETIME, sample.identity};
    const steady::FrozenMapping mapping {steady_probe_get64(offer + 24), anchor, STEADY_INPUT_RESERVE};
    owner = std::make_unique<owner_t>(generation, mapping, backend, agreement, clock);
    if (!owner->ready()) {
      owner.reset();
    }
  }

  /** @brief Append a bounded raw row. @param kind Receipt=1 or backend=2. @param seq Source identity. @param capture Callback time. @param usage USB usage. @param down State. @param ok Admission/backend result. @return Capacity and clock available. */
  bool trace(std::uint64_t kind, std::uint64_t seq, std::uint64_t capture, std::uint16_t usage, bool down, bool ok) noexcept {
    std::uint64_t now = 0;
    if (!clock.now(now)) {
      return false;
    }
    std::lock_guard lock {trace_mutex};
    if (used == rows.size()) {
      ++overflow;
      return false;
    }
    rows[used++] = {kind, seq, capture, now, usage, down ? 1U : 0U, ok ? 1U : 0U};
    return true;
  }

  /** @brief Stop, join and export once. Cleanup stays on the injection worker. */
  void stop() {
    std::lock_guard lock {stop_mutex};
    if (owner) {
      owner->stop();
    }
    if (worker.joinable()) {
      worker.join();
    }
    if (saved || !owner) {
      return;
    }
    saved = true;
    FILE *file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, trace_path.c_str(), "wx");
#else
    file = std::fopen(trace_path.c_str(), "wx");
#endif
    if (!file) {
      BOOST_LOG(error) << "STEADY input trace could not be created: use a new writable filename";
      return;
    }
    std::fprintf(file, "kind,sequence,capture_ns,host_ns,usage,down,ok,generation,overflow\n");
    for (std::size_t i = 0; i < used; ++i) {
      const auto &r = rows[i];
      std::fprintf(file, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long) r.kind, (unsigned long long) r.sequence, (unsigned long long) r.capture, (unsigned long long) r.time, (unsigned long long) r.usage, (unsigned long long) r.down, (unsigned long long) r.ok, (unsigned long long) generation, (unsigned long long) overflow);
    }
    const auto &r = owner->receiver_counts_after_join();
    BOOST_LOG(info) << "STEADY input originals=" << owner->counts_after_join().submitted << " admitted=" << r.admitted
                    << " dispatched=" << r.dispatched << " late=" << r.late << " abandoned=" << r.abandoned
                    << " ingress_abandoned=" << owner->counts_after_join().queue_abandoned
                    << " cleanup_failures=" << r.cleanup_failures << " trace_overflow=" << overflow;
    const bool write_failed = std::ferror(file) != 0;
    const auto close_result = std::fclose(file);
    if (write_failed || close_result) {
      BOOST_LOG(error) << "STEADY input trace write failed";
    }
  }

  /** @brief Join before destroying the backend and its bounded scheduler. */
  ~timed_keyboard_t() {
    stop();
  }
};

/** @brief Stop one session and relinquish keyboard ownership after cleanup. @param input Disconnected input session. */
void steady_stop(const std::shared_ptr<input_t> &input) {
  std::lock_guard lifecycle {steady_lifecycle_mutex};
  std::shared_ptr<timed_keyboard_t> owner;
  {
    std::lock_guard lock {keyboard_mutex};
    if (steady_owner && steady_owner->input == input.get()) {
      owner = steady_owner;
    }
  }
  if (owner) {
    owner->stop();
    {
      std::lock_guard lock {keyboard_mutex};
      steady_owner.reset();
      steady_keyboard_active = false;
    }
  }
  input->steady_offer.fill(0);
  input->steady_committed = false;
}

/** @brief Stop any timed keyboard before global platform input destruction. */
void steady_stop_all() {
  std::lock_guard lifecycle {steady_lifecycle_mutex};
  std::shared_ptr<timed_keyboard_t> owner;
  {
    std::lock_guard lock {keyboard_mutex};
    owner = steady_owner;
  }
  if (!owner) {
    return;
  }
  owner->stop();
  {
    std::lock_guard lock {keyboard_mutex};
    steady_owner.reset();
    steady_keyboard_active = false;
  }
}

/** @brief Admit one authenticated prototype frame. See input.h for the external contract. */
bool steady_receive(std::shared_ptr<input_t> &input, std::string_view payload, std::vector<std::uint8_t> &response, std::function<void()> failure) {
  response.clear();
  if (!steady_input_opt_in()) {
    return true;
  }
  const auto *p = reinterpret_cast<const unsigned char *>(payload.data());
  std::lock_guard lifecycle {steady_lifecycle_mutex};
  if (!steady_input_valid(p, payload.size())) {
    return !input->steady_committed;
  }
  auto &offer = input->steady_offer;
  const auto reject = [&]() {
    response.assign(p, p + STEADY_INPUT_BYTES);
    response[5] = SI_REJECT;
    steady_probe_put64(response.data() + 16, 0);
    response[56] = response[57] = response[58] = 0;
    return !input->steady_committed;
  };
  if (p[5] == SI_HELLO && !input->steady_committed) {
    if (offer[0]) {
      return reject();  // A generation is never replaced in this transport session.
    }
    std::uint64_t now, frequency;
    char path[32768];
    const auto length = GetEnvironmentVariableA("STEADY_INPUT_TRACE", path, sizeof(path));
    if (!length || length >= sizeof(path) || !config::input.keyboard || config::input.key_rightalt_to_key_win || !steady_probe_now(&now, &frequency) || !steady_probe_get64(p + 24) || steady_probe_get64(p + 32)) {
      return reject();
    }
    // The tested physical profile uses side-specific modifiers; permit the three stock generic remaps only.
    for (const auto &[from, to] : config::input.keybindings) {
      if (from != to && !((from == 0x10 && to == 0xA0) || (from == 0x11 && to == 0xA2) || (from == 0x12 && to == 0xA4))) {
        return reject();
      }
    }
    std::copy(p, p + STEADY_INPUT_BYTES, offer.begin());
    offer[5] = SI_OFFER;
    steady_probe_put64(offer.data() + 32, now);
    response.assign(offer.begin(), offer.end());
    return true;
  }
  if (p[5] == SI_COMMIT && !input->steady_committed) {
    if (!offer[0] || !steady_input_same_offer(p, offer.data())) {
      return reject();
    }
    char path[32768];
    const auto length = GetEnvironmentVariableA("STEADY_INPUT_TRACE", path, sizeof(path));
    if (!length || length >= sizeof(path)) {
      return reject();
    }
    std::shared_ptr<timed_keyboard_t> candidate;
    {
      std::lock_guard lock {keyboard_mutex};
      if (steady_owner || !platf_input) {
        return reject();
      }
      for (const auto &[key, state] : key_press) {
        if (state.pressed) {
          return reject();
        }
      }
      candidate = std::make_shared<timed_keyboard_t>(input.get(), offer.data(), std::string(path, length));
      if (!candidate->owner) {
        return reject();
      }
      task_pool.cancel(key_press_repeat_id);
      key_press_repeat_id = nullptr;
      steady_owner = candidate;
      steady_keyboard_active = true;
      input->steady_committed = true;
    }
    try {
      candidate->worker = std::thread([candidate, failure = std::move(failure)]() {
        if (!candidate->owner->run()) {
          failure();
        }
      });
    } catch (...) {
      {
        std::lock_guard lock {keyboard_mutex};
        steady_owner.reset();
        steady_keyboard_active = false;
      }
      return false;  // Commitment is irreversible, even if thread creation fails.
    }
    response.assign(offer.begin(), offer.end());
    response[5] = SI_READY;
    BOOST_LOG(info) << "STEADY experimental keyboard committed: reliable control, fixed 40 ms reserve, 60 s lifetime; host repeat synthesis disabled";
    return true;
  }
  if (p[5] == SI_KEY && input->steady_committed) {
    std::shared_ptr<timed_keyboard_t> owner;
    {
      std::lock_guard lock {keyboard_mutex};
      if (steady_owner && steady_owner->input == input.get()) {
        owner = steady_owner;
      }
    }
    if (!owner || steady_probe_get64(p + 8) != owner->generation || std::memcmp(p + 32, offer.data() + 32, 24)) {
      return false;
    }
    steady::Record record;
    record.kind = steady::Kind::key;
    record.generation = owner->generation;
    record.seq = steady_probe_get64(p + 16);
    record.capture = steady_probe_get64(p + 24);
    record.key = {7, p[57]};
    record.down = p[58] != 0;
    const auto anchor = steady_probe_get64(offer.data() + 24);
    if (record.capture < anchor || record.capture - anchor >= STEADY_INPUT_LIFETIME || !steady::windows_record(record)) {
      return false;
    }
    if (!owner->trace(1, record.seq, record.capture, record.key.usage, record.down, true)) {
      return false;
    }
    return owner->owner->submit(record, true) == steady::Submit::accepted;
  }
  return !input->steady_committed;
}

#ifdef SUNSHINE_TESTS
namespace testing {
  /** @brief Read completed timed backend calls safely. @return Count for the active owner, or zero. */
  std::uint64_t steady_transition_count() {
    std::lock_guard lock {keyboard_mutex};
    return steady_owner ? steady_owner->completed_transitions.load() : 0;
  }
}  // namespace testing
#endif
