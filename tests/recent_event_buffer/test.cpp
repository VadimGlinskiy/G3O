#include "recent_event_buffer.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

bool IsWellFormedSnapshot(const char* text, size_t length) {
    size_t lineStart = 0;
    while (lineStart < length) {
        size_t lineEnd = lineStart;
        while (lineEnd < length && text[lineEnd] != '\n') {
            ++lineEnd;
        }
        if (lineEnd == length || lineEnd == lineStart) {
            return false;
        }
        const char value = text[lineStart];
        for (size_t i = lineStart; i < lineEnd; ++i) {
            if (text[i] != value) {
                return false;
            }
        }
        lineStart = lineEnd + 1;
    }
    return true;
}

void TestNewestEventsAreCopiedInChronologicalOrder() {
    G3O::RecentEventBuffer<3, 8> events;
    char output[G3O::RecentEventBuffer<3, 8>::SnapshotCapacity] = {};

    Require(events.CopyTo(output, sizeof(output)) == 0, "empty ring starts empty");
    events.Append("one\n", 4);
    events.Append("two\n", 4);
    events.Append("three\n", 6);
    events.Append("four\n", 5);

    const size_t copied = events.CopyTo(output, sizeof(output));
    Require(copied == std::strlen("two\nthree\nfour\n"), "ring returns the newest three events");
    Require(std::strcmp(output, "two\nthree\nfour\n") == 0, "events remain chronological");
}

void TestLongEventIsTruncatedWithTerminator() {
    G3O::RecentEventBuffer<2, 8> events;
    char output[G3O::RecentEventBuffer<2, 8>::SnapshotCapacity] = {};

    events.Append("abcdefgh\n", 9);
    const size_t copied = events.CopyTo(output, sizeof(output));

    Require(copied == std::strlen("abcde~\n"), "truncated event fits slot");
    Require(std::strcmp(output, "abcde~\n") == 0, "truncation is marked and newline-terminated");
}

void TestConcurrentSnapshotNeverContainsTornEvents() {
    G3O::RecentEventBuffer<8, 64> events;
    std::atomic<bool> finished{ false };
    std::atomic<bool> valid{ true };

    std::thread writer([&events, &finished]() {
        char event[48];
        for (unsigned int i = 0; i < 50000; ++i) {
            const char value = static_cast<char>('A' + (i % 26));
            std::memset(event, value, sizeof(event));
            event[sizeof(event) - 1] = '\n';
            events.Append(event, sizeof(event));
        }
        finished.store(true, std::memory_order_release);
    });

    char output[G3O::RecentEventBuffer<8, 64>::SnapshotCapacity] = {};
    do {
        const size_t copied = events.CopyTo(output, sizeof(output));
        if (!IsWellFormedSnapshot(output, copied)) {
            valid.store(false, std::memory_order_relaxed);
            break;
        }
    } while (!finished.load(std::memory_order_acquire));

    writer.join();
    Require(valid.load(std::memory_order_relaxed), "lock-free snapshots contain only complete events");
}

} // namespace

int main() {
    TestNewestEventsAreCopiedInChronologicalOrder();
    TestLongEventIsTruncatedWithTerminator();
    TestConcurrentSnapshotNeverContainsTornEvents();
    std::puts("PASS: preallocated recent-event buffer tests");
    return 0;
}
